#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <climits>
#include <cmath>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace redisclone {

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

static std::string to_upper(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

static std::vector<std::string> split_args(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

static bool parse_int64(const std::string &s, long long &out) {
    try {
        size_t idx = 0;
        long long v = std::stoll(s, &idx);
        if (idx != s.size()) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_double(const std::string &s, double &out) {
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (idx != s.size()) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static bool glob_match_impl(const std::string &pat, const std::string &str, size_t i, size_t j,
                            std::vector<std::vector<int>> &memo) {
    if (memo[i][j] != -1) {
        return memo[i][j] == 1;
    }
    bool res = false;
    if (i == pat.size()) {
        res = (j == str.size());
    } else if (pat[i] == '*') {
        res = glob_match_impl(pat, str, i + 1, j, memo) || (j < str.size() && glob_match_impl(pat, str, i, j + 1, memo));
    } else if (j < str.size() && (pat[i] == '?' || pat[i] == str[j])) {
        res = glob_match_impl(pat, str, i + 1, j + 1, memo);
    }
    memo[i][j] = res ? 1 : 0;
    return res;
}

static bool glob_match(const std::string &pat, const std::string &str) {
    std::vector<std::vector<int>> memo(pat.size() + 1, std::vector<int>(str.size() + 1, -1));
    return glob_match_impl(pat, str, 0, 0, memo);
}

static std::string resp_simple(const std::string &s) {
    return "+" + s + "\r\n";
}

static std::string resp_err(const std::string &s) {
    return "-ERR " + s + "\r\n";
}

static std::string resp_integer(long long v) {
    return ":" + std::to_string(v) + "\r\n";
}

static std::string resp_bulk(const std::string &s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

static std::string resp_nil_bulk() {
    return "$-1\r\n";
}

static std::string resp_array_header(size_t n) {
    return "*" + std::to_string(n) + "\r\n";
}

static std::string resp_null_array() {
    return "*-1\r\n";
}

static std::string resp_array_bulk(const std::vector<std::optional<std::string>> &items) {
    if (items.empty()) {
        return "*0\r\n";
    }
    std::string out = resp_array_header(items.size());
    for (const auto &it : items) {
        if (!it.has_value()) {
            out += resp_nil_bulk();
        } else {
            out += resp_bulk(*it);
        }
    }
    return out;
}

static std::string resp_array_raw(const std::vector<std::string> &items) {
    if (items.empty()) {
        return "*0\r\n";
    }
    std::string out = resp_array_header(items.size());
    for (const auto &it : items) {
        out += it;
    }
    return out;
}

static std::string resp_command(const std::vector<std::string> &argv) {
    std::string out = resp_array_header(argv.size());
    for (const auto &arg : argv) {
        out += resp_bulk(arg);
    }
    return out;
}

static bool parse_resp_bulk_value(const std::string &resp, std::string &value, bool &is_nil) {
    is_nil = false;
    value.clear();
    if (resp.size() < 3 || resp[0] != '$') {
        return false;
    }
    if (resp.rfind("$-1", 0) == 0) {
        is_nil = true;
        return true;
    }
    size_t line_end = resp.find("\r\n", 1);
    if (line_end == std::string::npos) {
        return false;
    }
    long long len = 0;
    if (!parse_int64(resp.substr(1, line_end - 1), len) || len < 0) {
        return false;
    }
    size_t start = line_end + 2;
    if (start + static_cast<size_t>(len) > resp.size()) {
        return false;
    }
    value = resp.substr(start, static_cast<size_t>(len));
    return true;
}

static std::vector<std::vector<std::string>> parse_commands(std::string &buffer);
extern const char kHelpText[];
extern const std::vector<std::string> kDemoScript;

struct StreamId {
    long long ms = 0;
    long long seq = 0;
};

static bool parse_stream_id(const std::string &s, StreamId &out) {
    size_t dash = s.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    long long ms = 0;
    long long seq = 0;
    if (!parse_int64(s.substr(0, dash), ms) || !parse_int64(s.substr(dash + 1), seq)) {
        return false;
    }
    out.ms = ms;
    out.seq = seq;
    return true;
}

static int compare_stream_id(const StreamId &a, const StreamId &b) {
    if (a.ms != b.ms) {
        return (a.ms < b.ms) ? -1 : 1;
    }
    if (a.seq != b.seq) {
        return (a.seq < b.seq) ? -1 : 1;
    }
    return 0;
}

static std::string stream_id_to_string(const StreamId &id) {
    return std::to_string(id.ms) + "-" + std::to_string(id.seq);
}

struct StreamEntry {
    StreamId id;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct StreamValue {
    std::vector<StreamEntry> entries;
    StreamId last_id{0, 0};
};

struct GeoPoint {
    double lon = 0.0;
    double lat = 0.0;
};

struct Value {
    enum class Type {
        String,
        List,
        Set,
        Hash,
        ZSet,
        Stream,
        Geo
    } type = Type::String;

    std::string str;
    std::deque<std::string> list;
    std::unordered_set<std::string> set;
    std::unordered_map<std::string, std::string> hash;
    std::unordered_map<std::string, double> zset;
    StreamValue stream;
    std::unordered_map<std::string, GeoPoint> geo;
};

struct Entry {
    Value value;
    std::optional<TimePoint> expire_at;
};

class RedisClone {
public:
    void set_persistence_file(const std::string &path) {
        persistence_path_ = path;
    }

    void set_notify_callback(std::function<void(const std::string &, const std::string &)> cb) {
        notify_cb_ = std::move(cb);
    }

    void enable_aof(const std::string &path) {
        aof_path_ = path;
        if (!aof_path_.empty()) {
            aof_out_.open(aof_path_, std::ios::binary | std::ios::app);
        }
    }

    bool load_aof(const std::string &path, std::string &err_out) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            err_out = "could not open AOF";
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string buf = ss.str();
        auto cmds = parse_commands(buf);
        aof_loading_ = true;
        for (const auto &cmd : cmds) {
            exec(cmd);
        }
        aof_loading_ = false;
        return true;
    }

    bool rewrite_aof(const std::string &path, std::string &err_out) {
        std::string tmp = path + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            err_out = "could not open temp AOF";
            return false;
        }
        cleanup_expired();
        for (const auto &kv : store_) {
            const std::string &key = kv.first;
            const Entry &e = kv.second;
            if (e.value.type == Value::Type::String) {
                out << resp_command({"SET", key, e.value.str});
            } else if (e.value.type == Value::Type::List) {
                if (!e.value.list.empty()) {
                    std::vector<std::string> cmd = {"RPUSH", key};
                    cmd.insert(cmd.end(), e.value.list.begin(), e.value.list.end());
                    out << resp_command(cmd);
                }
            } else if (e.value.type == Value::Type::Set) {
                if (!e.value.set.empty()) {
                    std::vector<std::string> cmd = {"SADD", key};
                    for (const auto &v : e.value.set) {
                        cmd.push_back(v);
                    }
                    out << resp_command(cmd);
                }
            } else if (e.value.type == Value::Type::Hash) {
                if (!e.value.hash.empty()) {
                    std::vector<std::string> cmd = {"HSET", key};
                    for (const auto &it : e.value.hash) {
                        cmd.push_back(it.first);
                        cmd.push_back(it.second);
                    }
                    out << resp_command(cmd);
                }
            } else if (e.value.type == Value::Type::ZSet) {
                if (!e.value.zset.empty()) {
                    std::vector<std::string> cmd = {"ZADD", key};
                    for (const auto &it : e.value.zset) {
                        cmd.push_back(std::to_string(it.second));
                        cmd.push_back(it.first);
                    }
                    out << resp_command(cmd);
                }
            } else if (e.value.type == Value::Type::Geo) {
                if (!e.value.geo.empty()) {
                    std::vector<std::string> cmd = {"GEOADD", key};
                    for (const auto &it : e.value.geo) {
                        cmd.push_back(std::to_string(it.second.lon));
                        cmd.push_back(std::to_string(it.second.lat));
                        cmd.push_back(it.first);
                    }
                    out << resp_command(cmd);
                }
            } else if (e.value.type == Value::Type::Stream) {
                for (const auto &entry : e.value.stream.entries) {
                    std::vector<std::string> cmd = {"XADD", key, stream_id_to_string(entry.id)};
                    for (const auto &fv : entry.fields) {
                        cmd.push_back(fv.first);
                        cmd.push_back(fv.second);
                    }
                    out << resp_command(cmd);
                }
            }
            if (e.expire_at) {
                long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    e.expire_at.value().time_since_epoch()).count();
                out << resp_command({"PEXPIREAT", key, std::to_string(ms)});
            }
        }
        out.close();
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            err_out = "could not replace AOF";
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

    void set_publish_callback(std::function<int(const std::string &, const std::string &)> cb) {
        publish_cb_ = std::move(cb);
    }

    void set_client_count_callback(std::function<size_t()> cb) {
        client_count_cb_ = std::move(cb);
    }

    void set_tcp_port(int port) {
        tcp_port_ = port;
    }

    void record_slowlog(const std::vector<std::string> &cmd, long long duration_us) {
        if (duration_us < slowlog_threshold_us_) {
            return;
        }
        SlowlogEntry e;
        e.id = ++slowlog_id_;
        e.ts = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count());
        e.duration_us = duration_us;
        e.cmd = cmd;
        slowlog_.push_back(std::move(e));
        while (slowlog_.size() > slowlog_max_len_) {
            slowlog_.pop_front();
        }
    }

    bool save_to_file(const std::string &path, std::string &err_out) {
        cleanup_expired();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            err_out = "could not open file";
            return false;
        }
        write_token(out, "RCLONE1");
        out << "\n";
        for (const auto &kv : store_) {
            const Entry &e = kv.second;
            long long exp_ms = -1;
            if (e.expire_at) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.expire_at.value().time_since_epoch()).count();
                exp_ms = static_cast<long long>(ms);
            }
            write_token(out, type_to_string(e.value.type));
            write_token(out, std::to_string(exp_ms));
            write_token(out, kv.first);
            if (e.value.type == Value::Type::String) {
                write_token(out, "1");
                write_token(out, e.value.str);
            } else if (e.value.type == Value::Type::List) {
                write_token(out, std::to_string(e.value.list.size()));
                for (const auto &v : e.value.list) {
                    write_token(out, v);
                }
            } else if (e.value.type == Value::Type::Set) {
                write_token(out, std::to_string(e.value.set.size()));
                for (const auto &v : e.value.set) {
                    write_token(out, v);
                }
            } else if (e.value.type == Value::Type::Hash) {
                write_token(out, std::to_string(e.value.hash.size()));
                for (const auto &it : e.value.hash) {
                    write_token(out, it.first);
                    write_token(out, it.second);
                }
            } else if (e.value.type == Value::Type::ZSet) {
                write_token(out, std::to_string(e.value.zset.size()));
                for (const auto &it : e.value.zset) {
                    write_token(out, it.first);
                    write_token(out, std::to_string(it.second));
                }
            } else if (e.value.type == Value::Type::Stream) {
                write_token(out, std::to_string(e.value.stream.entries.size()));
                for (const auto &entry : e.value.stream.entries) {
                    write_token(out, stream_id_to_string(entry.id));
                    write_token(out, std::to_string(entry.fields.size()));
                    for (const auto &fv : entry.fields) {
                        write_token(out, fv.first);
                        write_token(out, fv.second);
                    }
                }
            } else if (e.value.type == Value::Type::Geo) {
                write_token(out, std::to_string(e.value.geo.size()));
                for (const auto &it : e.value.geo) {
                    write_token(out, it.first);
                    write_token(out, std::to_string(it.second.lon));
                    write_token(out, std::to_string(it.second.lat));
                }
            }
            out << "\n";
        }
        return true;
    }

    bool load_from_file(const std::string &path, std::string &err_out) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            err_out = "could not open file";
            return false;
        }
        std::string token;
        if (!read_token(in, token) || token != "RCLONE1") {
            err_out = "invalid file format";
            return false;
        }
        store_.clear();
        while (true) {
            std::string type_s;
            if (!read_token(in, type_s)) {
                break;
            }
            std::string exp_s;
            std::string key;
            std::string count_s;
            if (!read_token(in, exp_s) || !read_token(in, key) || !read_token(in, count_s)) {
                err_out = "corrupt file";
                return false;
            }
            long long exp_ms = -1;
            if (!parse_int64(exp_s, exp_ms)) {
                err_out = "corrupt expiry";
                return false;
            }
            long long count = 0;
            if (!parse_int64(count_s, count) || count < 0) {
                err_out = "corrupt count";
                return false;
            }
            Value::Type type;
            if (!string_to_type(type_s, type)) {
                err_out = "unknown type";
                return false;
            }
            Entry e;
            e.value.type = type;
            if (exp_ms >= 0) {
                e.expire_at = TimePoint(std::chrono::milliseconds(exp_ms));
            }
            if (type == Value::Type::String) {
                std::string val;
                if (!read_token(in, val)) {
                    err_out = "corrupt string";
                    return false;
                }
                e.value.str = val;
            } else if (type == Value::Type::List) {
                for (long long i = 0; i < count; ++i) {
                    std::string val;
                    if (!read_token(in, val)) {
                        err_out = "corrupt list";
                        return false;
                    }
                    e.value.list.push_back(val);
                }
            } else if (type == Value::Type::Set) {
                for (long long i = 0; i < count; ++i) {
                    std::string val;
                    if (!read_token(in, val)) {
                        err_out = "corrupt set";
                        return false;
                    }
                    e.value.set.insert(val);
                }
            } else if (type == Value::Type::Hash) {
                for (long long i = 0; i < count; ++i) {
                    std::string field;
                    std::string val;
                    if (!read_token(in, field) || !read_token(in, val)) {
                        err_out = "corrupt hash";
                        return false;
                    }
                    e.value.hash[field] = val;
                }
            } else if (type == Value::Type::ZSet) {
                for (long long i = 0; i < count; ++i) {
                    std::string member;
                    std::string score_s;
                    if (!read_token(in, member) || !read_token(in, score_s)) {
                        err_out = "corrupt zset";
                        return false;
                    }
                    double score = 0.0;
                    if (!parse_double(score_s, score)) {
                        err_out = "corrupt zset score";
                        return false;
                    }
                    e.value.zset[member] = score;
                }
            } else if (type == Value::Type::Stream) {
                for (long long i = 0; i < count; ++i) {
                    std::string id_s;
                    std::string field_count_s;
                    if (!read_token(in, id_s) || !read_token(in, field_count_s)) {
                        err_out = "corrupt stream";
                        return false;
                    }
                    long long field_count = 0;
                    if (!parse_int64(field_count_s, field_count) || field_count < 0) {
                        err_out = "corrupt stream fields";
                        return false;
                    }
                    StreamId id;
                    if (!parse_stream_id(id_s, id)) {
                        err_out = "corrupt stream id";
                        return false;
                    }
                    StreamEntry entry;
                    entry.id = id;
                    for (long long f = 0; f < field_count; ++f) {
                        std::string field;
                        std::string val;
                        if (!read_token(in, field) || !read_token(in, val)) {
                            err_out = "corrupt stream fields";
                            return false;
                        }
                        entry.fields.emplace_back(field, val);
                    }
                    e.value.stream.entries.push_back(std::move(entry));
                    if (compare_stream_id(id, e.value.stream.last_id) > 0) {
                        e.value.stream.last_id = id;
                    }
                }
            } else if (type == Value::Type::Geo) {
                for (long long i = 0; i < count; ++i) {
                    std::string member;
                    std::string lon_s;
                    std::string lat_s;
                    if (!read_token(in, member) || !read_token(in, lon_s) || !read_token(in, lat_s)) {
                        err_out = "corrupt geo";
                        return false;
                    }
                    double lon = 0.0;
                    double lat = 0.0;
                    if (!parse_double(lon_s, lon) || !parse_double(lat_s, lat)) {
                        err_out = "corrupt geo coord";
                        return false;
                    }
                    e.value.geo[member] = GeoPoint{lon, lat};
                }
            }
            if (e.expire_at && Clock::now() >= *e.expire_at) {
                continue;
            }
            store_[key] = std::move(e);
        }
        cleanup_expired();
        return true;
    }

    std::string exec(const std::vector<std::string> &argv) {
        if (argv.empty()) {
            return resp_err("empty command");
        }
        ++commands_processed_;
        std::string cmd = to_upper(argv[0]);
        cleanup_expired();

        std::string resp;
        if (cmd == "PING") {
            resp = resp_simple("PONG");
        } else if (cmd == "ECHO") {
            if (argv.size() < 2) {
                resp = resp_err("wrong number of arguments for 'echo'");
            } else {
                resp = resp_bulk(argv[1]);
            }
        } else if (cmd == "SET") {
            resp = cmd_set(argv);
        } else if (cmd == "GET") {
            resp = cmd_get(argv);
        } else if (cmd == "GETSET") {
            resp = cmd_getset(argv);
        } else if (cmd == "SETNX") {
            resp = cmd_setnx(argv);
        } else if (cmd == "MSET") {
            resp = cmd_mset(argv, false);
        } else if (cmd == "MSETNX") {
            resp = cmd_mset(argv, true);
        } else if (cmd == "MGET") {
            resp = cmd_mget(argv);
        } else if (cmd == "APPEND") {
            resp = cmd_append(argv);
        } else if (cmd == "STRLEN") {
            resp = cmd_strlen(argv);
        } else if (cmd == "INCR") {
            resp = cmd_incr(argv, 1);
        } else if (cmd == "DECR") {
            resp = cmd_incr(argv, -1);
        } else if (cmd == "INCRBY") {
            resp = cmd_incrby(argv, 1);
        } else if (cmd == "DECRBY") {
            resp = cmd_incrby(argv, -1);
        } else if (cmd == "DEL") {
            resp = cmd_del(argv);
        } else if (cmd == "EXISTS") {
            resp = cmd_exists(argv);
        } else if (cmd == "EXPIRE") {
            resp = cmd_expire(argv, false);
        } else if (cmd == "PEXPIRE") {
            resp = cmd_expire(argv, true);
        } else if (cmd == "EXPIREAT") {
            resp = cmd_expireat(argv, false);
        } else if (cmd == "PEXPIREAT") {
            resp = cmd_expireat(argv, true);
        } else if (cmd == "PERSIST") {
            resp = cmd_persist(argv);
        } else if (cmd == "TTL") {
            resp = cmd_ttl(argv, false);
        } else if (cmd == "PTTL") {
            resp = cmd_ttl(argv, true);
        } else if (cmd == "KEYS") {
            resp = cmd_keys(argv);
        } else if (cmd == "TYPE") {
            resp = cmd_type(argv);
        } else if (cmd == "SCAN") {
            resp = cmd_scan(argv);
        } else if (cmd == "HELP") {
            resp = cmd_help(argv);
        } else if (cmd == "INFO") {
            resp = cmd_info(argv);
        } else if (cmd == "SLOWLOG") {
            resp = cmd_slowlog(argv);
        } else if (cmd == "DBSIZE") {
            resp = resp_integer(static_cast<long long>(store_.size()));
        } else if (cmd == "FLUSHDB" || cmd == "FLUSHALL") {
            store_.clear();
            notify("flushdb", "");
            resp = resp_simple("OK");
        } else if (cmd == "RENAME") {
            resp = cmd_rename(argv, false);
        } else if (cmd == "RENAMENX") {
            resp = cmd_rename(argv, true);
        } else if (cmd == "RPUSH") {
            resp = cmd_rpush(argv);
        } else if (cmd == "LPUSH") {
            resp = cmd_lpush(argv);
        } else if (cmd == "RPOP") {
            resp = cmd_rpop(argv);
        } else if (cmd == "LPOP") {
            resp = cmd_lpop(argv);
        } else if (cmd == "LRANGE") {
            resp = cmd_lrange(argv);
        } else if (cmd == "LLEN") {
            resp = cmd_llen(argv);
        } else if (cmd == "LINDEX") {
            resp = cmd_lindex(argv);
        } else if (cmd == "LSET") {
            resp = cmd_lset(argv);
        } else if (cmd == "LTRIM") {
            resp = cmd_ltrim(argv);
        } else if (cmd == "GETBIT") {
            resp = cmd_getbit(argv);
        } else if (cmd == "SETBIT") {
            resp = cmd_setbit(argv);
        } else if (cmd == "BITCOUNT") {
            resp = cmd_bitcount(argv);
        } else if (cmd == "BITOP") {
            resp = cmd_bitop(argv);
        } else if (cmd == "SADD") {
            resp = cmd_sadd(argv);
        } else if (cmd == "SREM") {
            resp = cmd_srem(argv);
        } else if (cmd == "SMEMBERS") {
            resp = cmd_smembers(argv);
        } else if (cmd == "SCARD") {
            resp = cmd_scard(argv);
        } else if (cmd == "SISMEMBER") {
            resp = cmd_sismember(argv);
        } else if (cmd == "SSCAN") {
            resp = cmd_sscan(argv);
        } else if (cmd == "SMOVE") {
            resp = cmd_smove(argv);
        } else if (cmd == "SUNION") {
            resp = cmd_sunion(argv);
        } else if (cmd == "SINTER") {
            resp = cmd_sinter(argv);
        } else if (cmd == "HSET") {
            resp = cmd_hset(argv);
        } else if (cmd == "HMSET") {
            resp = cmd_hmset(argv);
        } else if (cmd == "HGET") {
            resp = cmd_hget(argv);
        } else if (cmd == "HMGET") {
            resp = cmd_hmget(argv);
        } else if (cmd == "HDEL") {
            resp = cmd_hdel(argv);
        } else if (cmd == "HEXISTS") {
            resp = cmd_hexists(argv);
        } else if (cmd == "HLEN") {
            resp = cmd_hlen(argv);
        } else if (cmd == "HGETALL") {
            resp = cmd_hgetall(argv);
        } else if (cmd == "HSCAN") {
            resp = cmd_hscan(argv);
        } else if (cmd == "ZADD") {
            resp = cmd_zadd(argv);
        } else if (cmd == "ZREM") {
            resp = cmd_zrem(argv);
        } else if (cmd == "ZRANGE") {
            resp = cmd_zrange(argv);
        } else if (cmd == "ZCARD") {
            resp = cmd_zcard(argv);
        } else if (cmd == "ZSCORE") {
            resp = cmd_zscore(argv);
        } else if (cmd == "ZSCAN") {
            resp = cmd_zscan(argv);
        } else if (cmd == "GEOADD") {
            resp = cmd_geoadd(argv);
        } else if (cmd == "GEOPOS") {
            resp = cmd_geopos(argv);
        } else if (cmd == "GEODIST") {
            resp = cmd_geodist(argv);
        } else if (cmd == "GEORADIUS") {
            resp = cmd_georadius(argv);
        } else if (cmd == "XADD") {
            resp = cmd_xadd(argv);
        } else if (cmd == "XDEL") {
            resp = cmd_xdel(argv);
        } else if (cmd == "XLEN") {
            resp = cmd_xlen(argv);
        } else if (cmd == "XRANGE") {
            resp = cmd_xrange(argv);
        } else if (cmd == "XREAD") {
            resp = cmd_xread(argv);
        } else if (cmd == "SAVE") {
            resp = cmd_save(argv);
        } else if (cmd == "LOAD") {
            resp = cmd_load(argv);
        } else if (cmd == "REWRITEAOF" || cmd == "BGREWRITEAOF") {
            resp = cmd_bgrewriteaof(argv);
        } else if (cmd == "PUBLISH") {
            resp = cmd_publish(argv);
        } else {
            resp = resp_err("unknown command");
        }

        if (!aof_loading_ && is_write_command(cmd) && !resp.empty() && resp[0] != '-') {
            append_aof(argv);
        }
        return resp;
    }

private:
    std::unordered_map<std::string, Entry> store_;
    std::string persistence_path_;
    std::function<int(const std::string &, const std::string &)> publish_cb_;
    std::function<void(const std::string &, const std::string &)> notify_cb_;
    std::function<size_t()> client_count_cb_;
    std::string aof_path_;
    std::ofstream aof_out_;
    std::mutex aof_mu_;
    bool aof_loading_ = false;
    std::atomic<bool> aof_rewrite_in_progress_{false};
    std::string aof_last_error_;
    long long commands_processed_ = 0;
    TimePoint start_time_ = Clock::now();
    int tcp_port_ = 0;
    long long slowlog_id_ = 0;
    size_t slowlog_max_len_ = 128;
    long long slowlog_threshold_us_ = 10000;

    struct SlowlogEntry {
        long long id = 0;
        long long ts = 0;
        long long duration_us = 0;
        std::vector<std::string> cmd;
    };
    std::deque<SlowlogEntry> slowlog_;

    void cleanup_expired() {
        if (store_.empty()) {
            return;
        }
        auto now = Clock::now();
        std::vector<std::string> to_remove;
        to_remove.reserve(store_.size());
        for (const auto &kv : store_) {
            if (kv.second.expire_at && now >= *kv.second.expire_at) {
                to_remove.push_back(kv.first);
            }
        }
        for (const auto &k : to_remove) {
            store_.erase(k);
            notify("expired", k);
        }
    }

    static std::string wrong_type_err() {
        return resp_err("WRONGTYPE Operation against a key holding the wrong kind of value");
    }

    void notify(const std::string &event, const std::string &key) {
        if (notify_cb_) {
            notify_cb_(event, key);
        }
    }

    bool is_write_command(const std::string &cmd) const {
        static const std::unordered_set<std::string> writes = {
            "SET", "GETSET", "SETNX", "MSET", "MSETNX", "APPEND", "INCR", "DECR", "INCRBY", "DECRBY",
            "DEL", "EXPIRE", "PEXPIRE", "EXPIREAT", "PEXPIREAT", "PERSIST", "RENAME", "RENAMENX",
            "RPUSH", "LPUSH", "RPOP", "LPOP", "LSET", "LTRIM",
            "SETBIT", "BITOP",
            "SADD", "SREM", "SMOVE", "SUNIONSTORE", "SINTERSTORE",
            "HSET", "HMSET", "HDEL",
            "ZADD", "ZREM",
            "GEOADD",
            "XADD", "XDEL",
            "FLUSHDB", "FLUSHALL", "PUBLISH"
        };
        return writes.count(cmd) > 0;
    }

    void append_aof(const std::vector<std::string> &argv) {
        if (aof_path_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(aof_mu_);
        if (!aof_out_.is_open()) {
            aof_out_.open(aof_path_, std::ios::binary | std::ios::app);
        }
        if (aof_out_.is_open()) {
            aof_out_ << resp_command(argv);
            aof_out_.flush();
        }
    }

    static void write_token(std::ostream &out, const std::string &token) {
        out << token.size() << ":" << token << " ";
    }

    static bool read_token(std::istream &in, std::string &out) {
        in >> std::ws;
        if (!in.good()) {
            return false;
        }
        size_t len = 0;
        char c = 0;
        while (in.get(c)) {
            if (c == ':') {
                break;
            }
            if (c < '0' || c > '9') {
                return false;
            }
            len = (len * 10) + static_cast<size_t>(c - '0');
        }
        if (!in.good()) {
            return false;
        }
        out.resize(len);
        in.read(&out[0], static_cast<std::streamsize>(len));
        return in.good();
    }

    static std::string type_to_string(Value::Type t) {
        switch (t) {
        case Value::Type::String:
            return "string";
        case Value::Type::List:
            return "list";
        case Value::Type::Set:
            return "set";
        case Value::Type::Hash:
            return "hash";
        case Value::Type::ZSet:
            return "zset";
        case Value::Type::Stream:
            return "stream";
        case Value::Type::Geo:
            return "geo";
        }
        return "string";
    }

    static bool string_to_type(const std::string &s, Value::Type &out) {
        if (s == "string") {
            out = Value::Type::String;
            return true;
        }
        if (s == "list") {
            out = Value::Type::List;
            return true;
        }
        if (s == "set") {
            out = Value::Type::Set;
            return true;
        }
        if (s == "hash") {
            out = Value::Type::Hash;
            return true;
        }
        if (s == "zset") {
            out = Value::Type::ZSet;
            return true;
        }
        if (s == "stream") {
            out = Value::Type::Stream;
            return true;
        }
        if (s == "geo") {
            out = Value::Type::Geo;
            return true;
        }
        return false;
    }

    Entry *get_entry(const std::string &key) {
        auto it = store_.find(key);
        if (it == store_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    Entry *get_or_create(const std::string &key, Value::Type type, bool create, std::string &err_out) {
        auto it = store_.find(key);
        if (it == store_.end()) {
            if (!create) {
                return nullptr;
            }
            Entry e;
            e.value.type = type;
            return &store_.emplace(key, std::move(e)).first->second;
        }
        if (it->second.value.type != type) {
            err_out = wrong_type_err();
            return nullptr;
        }
        return &it->second;
    }

    static long long normalize_index(long long idx, long long size) {
        if (idx < 0) {
            idx += size;
        }
        return idx;
    }

    static int get_bit_at(const std::string &s, size_t offset) {
        size_t byte_index = offset / 8;
        size_t bit_index = 7 - (offset % 8);
        if (byte_index >= s.size()) {
            return 0;
        }
        unsigned char byte = static_cast<unsigned char>(s[byte_index]);
        return (byte >> bit_index) & 1;
    }

    static int set_bit_at(std::string &s, size_t offset, int value) {
        size_t byte_index = offset / 8;
        size_t bit_index = 7 - (offset % 8);
        if (byte_index >= s.size()) {
            s.resize(byte_index + 1, '\0');
        }
        unsigned char byte = static_cast<unsigned char>(s[byte_index]);
        int prev = (byte >> bit_index) & 1;
        if (value) {
            byte |= static_cast<unsigned char>(1u << bit_index);
        } else {
            byte &= static_cast<unsigned char>(~(1u << bit_index));
        }
        s[byte_index] = static_cast<char>(byte);
        return prev;
    }

    static long long popcount_byte(unsigned char v) {
        long long c = 0;
        while (v) {
            v &= static_cast<unsigned char>(v - 1);
            ++c;
        }
        return c;
    }

    static long long bitcount_range(const std::string &s, long long start, long long end) {
        if (s.empty()) {
            return 0;
        }
        long long size = static_cast<long long>(s.size());
        if (start < 0) start += size;
        if (end < 0) end += size;
        if (start < 0) start = 0;
        if (end >= size) end = size - 1;
        if (start > end || start >= size) {
            return 0;
        }
        long long count = 0;
        for (long long i = start; i <= end; ++i) {
            count += popcount_byte(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        }
        return count;
    }

    static double deg2rad(double deg) {
        return deg * 3.14159265358979323846 / 180.0;
    }

    static double geo_distance_m(double lon1, double lat1, double lon2, double lat2) {
        double dlat = deg2rad(lat2 - lat1);
        double dlon = deg2rad(lon2 - lon1);
        double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(deg2rad(lat1)) * std::cos(deg2rad(lat2)) *
                   std::sin(dlon / 2) * std::sin(dlon / 2);
        double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
        return 6371000.0 * c;
    }

    static double unit_multiplier(const std::string &unit) {
        std::string u = to_upper(unit);
        if (u == "M") {
            return 1.0;
        }
        if (u == "KM") {
            return 1000.0;
        }
        if (u == "MI") {
            return 1609.344;
        }
        if (u == "FT") {
            return 0.3048;
        }
        return 1.0;
    }

    static bool parse_stream_start_end(const std::string &s, StreamId &out, bool is_start) {
        if (s == "-") {
            out = StreamId{0, 0};
            return true;
        }
        if (s == "+") {
            out = StreamId{LLONG_MAX, LLONG_MAX};
            return true;
        }
        if (s == "$") {
            out = StreamId{LLONG_MAX, LLONG_MAX};
            return true;
        }
        return parse_stream_id(s, out);
    }

    std::string cmd_set(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'set'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        e->value.str = argv[2];
        e->expire_at.reset();
        if (argv.size() >= 5) {
            std::string opt = to_upper(argv[3]);
            long long val = 0;
            if (!parse_int64(argv[4], val)) {
                return resp_err("value is not an integer or out of range");
            }
            if (opt == "EX") {
                e->expire_at = Clock::now() + std::chrono::seconds(val);
            } else if (opt == "PX") {
                e->expire_at = Clock::now() + std::chrono::milliseconds(val);
            }
        }
        notify("set", argv[1]);
        return resp_simple("OK");
    }

    std::string cmd_get(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'get'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_nil_bulk();
        }
        if (e->value.type != Value::Type::String) {
            return wrong_type_err();
        }
        return resp_bulk(e->value.str);
    }

    std::string cmd_getset(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'getset'");
        }
        std::string prev = "";
        bool had_prev = false;
        Entry *e = get_entry(argv[1]);
        if (e) {
            if (e->value.type != Value::Type::String) {
                return wrong_type_err();
            }
            prev = e->value.str;
            had_prev = true;
        }
        std::string err;
        Entry *target = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        target->value.str = argv[2];
        target->expire_at.reset();
        notify("set", argv[1]);
        return had_prev ? resp_bulk(prev) : resp_nil_bulk();
    }

    std::string cmd_setnx(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'setnx'");
        }
        if (store_.find(argv[1]) != store_.end()) {
            return resp_integer(0);
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        e->value.str = argv[2];
        notify("set", argv[1]);
        return resp_integer(1);
    }

    std::string cmd_mset(const std::vector<std::string> &argv, bool nx) {
        if (argv.size() < 3 || (argv.size() % 2) == 0) {
            return resp_err("wrong number of arguments for 'mset'");
        }
        if (nx) {
            for (size_t i = 1; i < argv.size(); i += 2) {
                if (store_.find(argv[i]) != store_.end()) {
                    return resp_integer(0);
                }
            }
        }
        for (size_t i = 1; i < argv.size(); i += 2) {
            std::string err;
            Entry *e = get_or_create(argv[i], Value::Type::String, true, err);
            if (!err.empty()) {
                return err;
            }
            e->value.str = argv[i + 1];
            e->expire_at.reset();
            notify("set", argv[i]);
        }
        return nx ? resp_integer(1) : resp_simple("OK");
    }

    std::string cmd_mget(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'mget'");
        }
        std::vector<std::optional<std::string>> items;
        items.reserve(argv.size() - 1);
        for (size_t i = 1; i < argv.size(); ++i) {
            Entry *e = get_entry(argv[i]);
            if (!e || e->value.type != Value::Type::String) {
                items.emplace_back(std::nullopt);
            } else {
                items.emplace_back(e->value.str);
            }
        }
        return resp_array_bulk(items);
    }

    std::string cmd_append(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'append'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        e->value.str += argv[2];
        notify("append", argv[1]);
        return resp_integer(static_cast<long long>(e->value.str.size()));
    }

    std::string cmd_strlen(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'strlen'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_integer(0);
        }
        if (e->value.type != Value::Type::String) {
            return wrong_type_err();
        }
        return resp_integer(static_cast<long long>(e->value.str.size()));
    }

    std::string cmd_incr(const std::vector<std::string> &argv, long long delta) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'incr'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        long long v = 0;
        if (!e->value.str.empty()) {
            if (!parse_int64(e->value.str, v)) {
                return resp_err("value is not an integer or out of range");
            }
        }
        v += delta;
        e->value.str = std::to_string(v);
        notify(delta >= 0 ? "incr" : "decr", argv[1]);
        return resp_integer(v);
    }

    std::string cmd_incrby(const std::vector<std::string> &argv, long long sign) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'incrby'");
        }
        long long delta = 0;
        if (!parse_int64(argv[2], delta)) {
            return resp_err("value is not an integer or out of range");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        long long base = 0;
        if (!e->value.str.empty()) {
            if (!parse_int64(e->value.str, base)) {
                return resp_err("value is not an integer or out of range");
            }
        }
        base += sign * delta;
        e->value.str = std::to_string(base);
        notify(sign >= 0 ? "incrby" : "decrby", argv[1]);
        return resp_integer(base);
    }

    std::string cmd_del(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'del'");
        }
        long long removed = 0;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (store_.erase(argv[i]) > 0) {
                ++removed;
                notify("del", argv[i]);
            }
        }
        return resp_integer(removed);
    }

    std::string cmd_exists(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'exists'");
        }
        long long count = 0;
        for (size_t i = 1; i < argv.size(); ++i) {
            if (store_.find(argv[i]) != store_.end()) {
                ++count;
            }
        }
        return resp_integer(count);
    }

    std::string cmd_expire(const std::vector<std::string> &argv, bool ms) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'expire'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_integer(0);
        }
        long long val = 0;
        if (!parse_int64(argv[2], val)) {
            return resp_err("value is not an integer or out of range");
        }
        if (ms) {
            e->expire_at = Clock::now() + std::chrono::milliseconds(val);
        } else {
            e->expire_at = Clock::now() + std::chrono::seconds(val);
        }
        notify("expire", argv[1]);
        return resp_integer(1);
    }

    std::string cmd_expireat(const std::vector<std::string> &argv, bool ms) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'expireat'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_integer(0);
        }
        long long val = 0;
        if (!parse_int64(argv[2], val)) {
            return resp_err("value is not an integer or out of range");
        }
        if (ms) {
            e->expire_at = TimePoint(std::chrono::milliseconds(val));
        } else {
            e->expire_at = TimePoint(std::chrono::seconds(val));
        }
        notify("expire", argv[1]);
        return resp_integer(1);
    }

    std::string cmd_persist(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'persist'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e || !e->expire_at) {
            return resp_integer(0);
        }
        e->expire_at.reset();
        notify("persist", argv[1]);
        return resp_integer(1);
    }

    std::string cmd_ttl(const std::vector<std::string> &argv, bool ms) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'ttl'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_integer(-2);
        }
        if (!e->expire_at) {
            return resp_integer(-1);
        }
        auto now = Clock::now();
        if (now >= *e->expire_at) {
            store_.erase(argv[1]);
            return resp_integer(-2);
        }
        auto diff = *e->expire_at - now;
        if (ms) {
            return resp_integer(std::chrono::duration_cast<std::chrono::milliseconds>(diff).count());
        }
        return resp_integer(std::chrono::duration_cast<std::chrono::seconds>(diff).count());
    }

    std::string cmd_keys(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'keys'");
        }
        std::vector<std::optional<std::string>> items;
        for (const auto &kv : store_) {
            if (glob_match(argv[1], kv.first)) {
                items.emplace_back(kv.first);
            }
        }
        return resp_array_bulk(items);
    }

    bool parse_scan_args(const std::vector<std::string> &argv, size_t start_idx, long long &cursor,
                         std::string &pattern, long long &count, std::string &err_out) {
        if (start_idx >= argv.size()) {
            err_out = "wrong number of arguments";
            return false;
        }
        if (!parse_int64(argv[start_idx], cursor) || cursor < 0) {
            err_out = "invalid cursor";
            return false;
        }
        pattern = "*";
        count = 10;
        for (size_t i = start_idx + 1; i < argv.size(); ++i) {
            std::string opt = to_upper(argv[i]);
            if (opt == "MATCH" && i + 1 < argv.size()) {
                pattern = argv[++i];
            } else if (opt == "COUNT" && i + 1 < argv.size()) {
                parse_int64(argv[++i], count);
            } else {
                err_out = "syntax error";
                return false;
            }
        }
        if (count <= 0) {
            count = 10;
        }
        return true;
    }

    std::string cmd_scan(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'scan'");
        }
        long long cursor = 0;
        long long count = 10;
        std::string pattern;
        std::string err;
        if (!parse_scan_args(argv, 1, cursor, pattern, count, err)) {
            return resp_err(err);
        }
        std::vector<std::string> keys;
        keys.reserve(store_.size());
        for (const auto &kv : store_) {
            if (glob_match(pattern, kv.first)) {
                keys.push_back(kv.first);
            }
        }
        std::sort(keys.begin(), keys.end());
        size_t idx = static_cast<size_t>(cursor);
        std::vector<std::optional<std::string>> items;
        for (size_t i = idx; i < keys.size() && items.size() < static_cast<size_t>(count); ++i) {
            items.emplace_back(keys[i]);
            idx = i + 1;
        }
        std::vector<std::string> out;
        long long next_cursor = (idx >= keys.size()) ? 0 : static_cast<long long>(idx);
        out.push_back(resp_bulk(std::to_string(next_cursor)));
        out.push_back(resp_array_bulk(items));
        return resp_array_raw(out);
    }

    std::string cmd_type(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'type'");
        }
        Entry *e = get_entry(argv[1]);
        if (!e) {
            return resp_simple("none");
        }
        return resp_simple(type_to_string(e->value.type));
    }

    std::string cmd_info(const std::vector<std::string> &argv) {
        if (argv.size() > 2) {
            return resp_err("wrong number of arguments for 'info'");
        }
        auto now = Clock::now();
        long long uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        size_t clients = client_count_cb_ ? client_count_cb_() : 0;
        std::ostringstream oss;
        oss << "# Server\n";
        oss << "redis_clone_version:0.1\n";
        oss << "uptime_in_seconds:" << uptime << "\n";
        oss << "tcp_port:" << tcp_port_ << "\n";
        oss << "connected_clients:" << clients << "\n";
        oss << "\n# Stats\n";
        oss << "total_commands_processed:" << commands_processed_ << "\n";
        oss << "\n# Keyspace\n";
        oss << "db0:keys=" << store_.size() << "\n";
        return resp_bulk(oss.str());
    }

    std::string cmd_help(const std::vector<std::string> &argv) {
        if (argv.size() > 2) {
            return resp_err("wrong number of arguments for 'help'");
        }
        if (argv.size() == 2) {
            std::string needle = to_upper(argv[1]);
            std::istringstream iss(kHelpText);
            std::ostringstream filtered;
            std::string line;
            while (std::getline(iss, line)) {
                std::string upper = to_upper(line);
                if (upper.find(needle) != std::string::npos) {
                    filtered << line << "\n";
                }
            }
            return resp_bulk(filtered.str());
        }
        return resp_bulk(kHelpText);
    }

    std::string cmd_slowlog(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'slowlog'");
        }
        std::string sub = to_upper(argv[1]);
        if (sub == "LEN") {
            return resp_integer(static_cast<long long>(slowlog_.size()));
        }
        if (sub == "RESET") {
            slowlog_.clear();
            return resp_simple("OK");
        }
        if (sub == "GET") {
            long long count = 10;
            if (argv.size() >= 3) {
                parse_int64(argv[2], count);
            }
            if (count < 0) {
                count = 0;
            }
            std::vector<std::string> out;
            long long remaining = count;
            for (auto it = slowlog_.rbegin(); it != slowlog_.rend() && remaining > 0; ++it, --remaining) {
                std::vector<std::string> row;
                row.push_back(resp_integer(it->id));
                row.push_back(resp_integer(it->ts));
                row.push_back(resp_integer(it->duration_us));
                std::vector<std::string> cmd_array;
                for (const auto &c : it->cmd) {
                    cmd_array.push_back(resp_bulk(c));
                }
                row.push_back(resp_array_raw(cmd_array));
                out.push_back(resp_array_raw(row));
            }
            return resp_array_raw(out);
        }
        return resp_err("unknown slowlog subcommand");
    }

    std::string cmd_rename(const std::vector<std::string> &argv, bool nx) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'rename'");
        }
        auto it = store_.find(argv[1]);
        if (it == store_.end()) {
            return resp_err("no such key");
        }
        if (nx && store_.find(argv[2]) != store_.end()) {
            return resp_integer(0);
        }
        store_[argv[2]] = std::move(it->second);
        store_.erase(it);
        notify("rename", argv[2]);
        return nx ? resp_integer(1) : resp_simple("OK");
    }

    std::string cmd_rpush(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'rpush'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, true, err);
        if (!err.empty()) {
            return err;
        }
        for (size_t i = 2; i < argv.size(); ++i) {
            e->value.list.push_back(argv[i]);
        }
        notify("rpush", argv[1]);
        return resp_integer(static_cast<long long>(e->value.list.size()));
    }

    std::string cmd_lpush(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'lpush'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, true, err);
        if (!err.empty()) {
            return err;
        }
        for (size_t i = 2; i < argv.size(); ++i) {
            e->value.list.push_front(argv[i]);
        }
        notify("lpush", argv[1]);
        return resp_integer(static_cast<long long>(e->value.list.size()));
    }

    std::string cmd_rpop(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'rpop'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e || e->value.list.empty()) {
            return resp_nil_bulk();
        }
        std::string v = e->value.list.back();
        e->value.list.pop_back();
        notify("rpop", argv[1]);
        return resp_bulk(v);
    }

    std::string cmd_lpop(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'lpop'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e || e->value.list.empty()) {
            return resp_nil_bulk();
        }
        std::string v = e->value.list.front();
        e->value.list.pop_front();
        notify("lpop", argv[1]);
        return resp_bulk(v);
    }

    std::string cmd_lrange(const std::vector<std::string> &argv) {
        if (argv.size() != 4) {
            return resp_err("wrong number of arguments for 'lrange'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        long long start = 0;
        long long stop = 0;
        if (!parse_int64(argv[2], start) || !parse_int64(argv[3], stop)) {
            return resp_err("value is not an integer or out of range");
        }
        long long size = static_cast<long long>(e->value.list.size());
        start = normalize_index(start, size);
        stop = normalize_index(stop, size);
        if (start < 0) start = 0;
        if (stop >= size) stop = size - 1;
        if (size == 0 || start > stop) {
            return "*0\r\n";
        }
        std::vector<std::optional<std::string>> items;
        for (long long i = start; i <= stop; ++i) {
            items.emplace_back(e->value.list[static_cast<size_t>(i)]);
        }
        return resp_array_bulk(items);
    }

    std::string cmd_llen(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'llen'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(static_cast<long long>(e->value.list.size()));
    }

    std::string cmd_lindex(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'lindex'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_nil_bulk();
        }
        long long idx = 0;
        if (!parse_int64(argv[2], idx)) {
            return resp_err("value is not an integer or out of range");
        }
        idx = normalize_index(idx, static_cast<long long>(e->value.list.size()));
        if (idx < 0 || idx >= static_cast<long long>(e->value.list.size())) {
            return resp_nil_bulk();
        }
        return resp_bulk(e->value.list[static_cast<size_t>(idx)]);
    }

    std::string cmd_lset(const std::vector<std::string> &argv) {
        if (argv.size() != 4) {
            return resp_err("wrong number of arguments for 'lset'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_err("no such key");
        }
        long long idx = 0;
        if (!parse_int64(argv[2], idx)) {
            return resp_err("value is not an integer or out of range");
        }
        idx = normalize_index(idx, static_cast<long long>(e->value.list.size()));
        if (idx < 0 || idx >= static_cast<long long>(e->value.list.size())) {
            return resp_err("index out of range");
        }
        e->value.list[static_cast<size_t>(idx)] = argv[3];
        notify("lset", argv[1]);
        return resp_simple("OK");
    }

    std::string cmd_ltrim(const std::vector<std::string> &argv) {
        if (argv.size() != 4) {
            return resp_err("wrong number of arguments for 'ltrim'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::List, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_simple("OK");
        }
        long long start = 0;
        long long stop = 0;
        if (!parse_int64(argv[2], start) || !parse_int64(argv[3], stop)) {
            return resp_err("value is not an integer or out of range");
        }
        long long size = static_cast<long long>(e->value.list.size());
        start = normalize_index(start, size);
        stop = normalize_index(stop, size);
        if (start < 0) start = 0;
        if (stop >= size) stop = size - 1;
        if (size == 0 || start > stop) {
            e->value.list.clear();
            return resp_simple("OK");
        }
        std::deque<std::string> trimmed;
        for (long long i = start; i <= stop; ++i) {
            trimmed.push_back(e->value.list[static_cast<size_t>(i)]);
        }
        e->value.list.swap(trimmed);
        notify("ltrim", argv[1]);
        return resp_simple("OK");
    }

    std::string cmd_getbit(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'getbit'");
        }
        long long offset = 0;
        if (!parse_int64(argv[2], offset) || offset < 0) {
            return resp_err("bit offset is not an integer or out of range");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(get_bit_at(e->value.str, static_cast<size_t>(offset)));
    }

    std::string cmd_setbit(const std::vector<std::string> &argv) {
        if (argv.size() != 4) {
            return resp_err("wrong number of arguments for 'setbit'");
        }
        long long offset = 0;
        if (!parse_int64(argv[2], offset) || offset < 0) {
            return resp_err("bit offset is not an integer or out of range");
        }
        long long bit = 0;
        if (!parse_int64(argv[3], bit) || (bit != 0 && bit != 1)) {
            return resp_err("bit is not 0 or 1");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        int prev = set_bit_at(e->value.str, static_cast<size_t>(offset), static_cast<int>(bit));
        notify("setbit", argv[1]);
        return resp_integer(prev);
    }

    std::string cmd_bitcount(const std::vector<std::string> &argv) {
        if (argv.size() != 2 && argv.size() != 4) {
            return resp_err("wrong number of arguments for 'bitcount'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::String, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        long long start = 0;
        long long end = static_cast<long long>(e->value.str.size()) - 1;
        if (argv.size() == 4) {
            if (!parse_int64(argv[2], start) || !parse_int64(argv[3], end)) {
                return resp_err("value is not an integer or out of range");
            }
        }
        long long count = bitcount_range(e->value.str, start, end);
        return resp_integer(count);
    }

    std::string cmd_bitop(const std::vector<std::string> &argv) {
        if (argv.size() < 4) {
            return resp_err("wrong number of arguments for 'bitop'");
        }
        std::string op = to_upper(argv[1]);
        std::string dest = argv[2];
        std::vector<std::string> keys(argv.begin() + 3, argv.end());
        if (op == "NOT" && keys.size() != 1) {
            return resp_err("BITOP NOT must be called with a single key");
        }
        size_t max_len = 0;
        std::vector<std::string> values;
        for (const auto &key : keys) {
            std::string err;
            Entry *e = get_or_create(key, Value::Type::String, false, err);
            if (!err.empty()) {
                return err;
            }
            if (!e) {
                values.emplace_back("");
            } else {
                values.push_back(e->value.str);
                max_len = std::max(max_len, e->value.str.size());
            }
        }
        std::string result(max_len, '\0');
        for (size_t i = 0; i < max_len; ++i) {
            unsigned char acc = 0;
            if (op == "NOT") {
                unsigned char v = (i < values[0].size()) ? static_cast<unsigned char>(values[0][i]) : 0;
                acc = static_cast<unsigned char>(~v);
            } else {
                if (op == "AND") {
                    acc = 0xFF;
                    for (const auto &v : values) {
                        unsigned char b = (i < v.size()) ? static_cast<unsigned char>(v[i]) : 0;
                        acc &= b;
                    }
                } else if (op == "OR") {
                    acc = 0;
                    for (const auto &v : values) {
                        unsigned char b = (i < v.size()) ? static_cast<unsigned char>(v[i]) : 0;
                        acc |= b;
                    }
                } else if (op == "XOR") {
                    acc = 0;
                    for (const auto &v : values) {
                        unsigned char b = (i < v.size()) ? static_cast<unsigned char>(v[i]) : 0;
                        acc ^= b;
                    }
                } else {
                    return resp_err("unknown bitop operation");
                }
            }
            result[i] = static_cast<char>(acc);
        }
        std::string err;
        Entry *dst = get_or_create(dest, Value::Type::String, true, err);
        if (!err.empty()) {
            return err;
        }
        dst->value.str = result;
        notify("bitop", dest);
        return resp_integer(static_cast<long long>(result.size()));
    }

    std::string cmd_sadd(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'sadd'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, true, err);
        if (!err.empty()) {
            return err;
        }
        long long added = 0;
        for (size_t i = 2; i < argv.size(); ++i) {
            added += e->value.set.insert(argv[i]).second ? 1 : 0;
        }
        if (added > 0) {
            notify("sadd", argv[1]);
        }
        return resp_integer(added);
    }

    std::string cmd_srem(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'srem'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        long long removed = 0;
        for (size_t i = 2; i < argv.size(); ++i) {
            removed += e->value.set.erase(argv[i]);
        }
        if (removed > 0) {
            notify("srem", argv[1]);
        }
        return resp_integer(removed);
    }

    std::string cmd_smembers(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'smembers'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        std::vector<std::optional<std::string>> items;
        for (const auto &v : e->value.set) {
            items.emplace_back(v);
        }
        return resp_array_bulk(items);
    }

    std::string cmd_scard(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'scard'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(static_cast<long long>(e->value.set.size()));
    }

    std::string cmd_sismember(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'sismember'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(e->value.set.count(argv[2]) ? 1 : 0);
    }

    std::string cmd_sscan(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'sscan'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Set, false, err);
        if (!err.empty()) {
            return err;
        }
        long long cursor = 0;
        long long count = 10;
        std::string pattern;
        if (!parse_scan_args(argv, 2, cursor, pattern, count, err)) {
            return resp_err(err);
        }
        std::vector<std::string> values;
        if (e) {
            for (const auto &v : e->value.set) {
                if (glob_match(pattern, v)) {
                    values.push_back(v);
                }
            }
        }
        std::sort(values.begin(), values.end());
        size_t idx = static_cast<size_t>(cursor);
        std::vector<std::optional<std::string>> items;
        for (size_t i = idx; i < values.size() && items.size() < static_cast<size_t>(count); ++i) {
            items.emplace_back(values[i]);
            idx = i + 1;
        }
        long long next_cursor = (idx >= values.size()) ? 0 : static_cast<long long>(idx);
        std::vector<std::string> out;
        out.push_back(resp_bulk(std::to_string(next_cursor)));
        out.push_back(resp_array_bulk(items));
        return resp_array_raw(out);
    }

    std::string cmd_smove(const std::vector<std::string> &argv) {
        if (argv.size() != 4) {
            return resp_err("wrong number of arguments for 'smove'");
        }
        std::string err1;
        Entry *src = get_or_create(argv[1], Value::Type::Set, false, err1);
        if (!err1.empty()) {
            return err1;
        }
        if (!src) {
            return resp_integer(0);
        }
        if (src->value.set.erase(argv[3]) == 0) {
            return resp_integer(0);
        }
        std::string err2;
        Entry *dst = get_or_create(argv[2], Value::Type::Set, true, err2);
        if (!err2.empty()) {
            return err2;
        }
        dst->value.set.insert(argv[3]);
        notify("smove", argv[1]);
        return resp_integer(1);
    }

    std::string cmd_sunion(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'sunion'");
        }
        std::unordered_set<std::string> result;
        for (size_t i = 1; i < argv.size(); ++i) {
            std::string err;
            Entry *e = get_or_create(argv[i], Value::Type::Set, false, err);
            if (!err.empty()) {
                return err;
            }
            if (!e) {
                continue;
            }
            for (const auto &v : e->value.set) {
                result.insert(v);
            }
        }
        std::vector<std::optional<std::string>> items;
        for (const auto &v : result) {
            items.emplace_back(v);
        }
        return resp_array_bulk(items);
    }

    std::string cmd_sinter(const std::vector<std::string> &argv) {
        if (argv.size() < 2) {
            return resp_err("wrong number of arguments for 'sinter'");
        }
        std::unordered_set<std::string> result;
        bool first = true;
        for (size_t i = 1; i < argv.size(); ++i) {
            std::string err;
            Entry *e = get_or_create(argv[i], Value::Type::Set, false, err);
            if (!err.empty()) {
                return err;
            }
            if (!e) {
                result.clear();
                first = false;
                break;
            }
            if (first) {
                result = e->value.set;
                first = false;
            } else {
                for (auto it = result.begin(); it != result.end();) {
                    if (e->value.set.count(*it) == 0) {
                        it = result.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (result.empty()) {
                break;
            }
        }
        std::vector<std::optional<std::string>> items;
        for (const auto &v : result) {
            items.emplace_back(v);
        }
        return resp_array_bulk(items);
    }

    std::string cmd_hset(const std::vector<std::string> &argv) {
        if (argv.size() < 4 || (argv.size() % 2) != 0) {
            return resp_err("wrong number of arguments for 'hset'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, true, err);
        if (!err.empty()) {
            return err;
        }
        long long added = 0;
        for (size_t i = 2; i + 1 < argv.size(); i += 2) {
            auto res = e->value.hash.insert({argv[i], argv[i + 1]});
            if (!res.second) {
                res.first->second = argv[i + 1];
            } else {
                ++added;
            }
        }
        notify("hset", argv[1]);
        return resp_integer(added);
    }

    std::string cmd_hmset(const std::vector<std::string> &argv) {
        if (argv.size() < 4 || (argv.size() % 2) != 0) {
            return resp_err("wrong number of arguments for 'hmset'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, true, err);
        if (!err.empty()) {
            return err;
        }
        for (size_t i = 2; i + 1 < argv.size(); i += 2) {
            e->value.hash[argv[i]] = argv[i + 1];
        }
        notify("hset", argv[1]);
        return resp_simple("OK");
    }

    std::string cmd_hget(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'hget'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_nil_bulk();
        }
        auto it = e->value.hash.find(argv[2]);
        if (it == e->value.hash.end()) {
            return resp_nil_bulk();
        }
        return resp_bulk(it->second);
    }

    std::string cmd_hmget(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'hmget'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        std::vector<std::optional<std::string>> items;
        for (size_t i = 2; i < argv.size(); ++i) {
            if (!e) {
                items.emplace_back(std::nullopt);
                continue;
            }
            auto it = e->value.hash.find(argv[i]);
            if (it == e->value.hash.end()) {
                items.emplace_back(std::nullopt);
            } else {
                items.emplace_back(it->second);
            }
        }
        return resp_array_bulk(items);
    }

    std::string cmd_hdel(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'hdel'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        long long removed = 0;
        for (size_t i = 2; i < argv.size(); ++i) {
            removed += e->value.hash.erase(argv[i]);
        }
        if (removed > 0) {
            notify("hdel", argv[1]);
        }
        return resp_integer(removed);
    }

    std::string cmd_hexists(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'hexists'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(e->value.hash.count(argv[2]) ? 1 : 0);
    }

    std::string cmd_hlen(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'hlen'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(static_cast<long long>(e->value.hash.size()));
    }

    std::string cmd_hgetall(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'hgetall'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        std::vector<std::optional<std::string>> items;
        for (const auto &kv : e->value.hash) {
            items.emplace_back(kv.first);
            items.emplace_back(kv.second);
        }
        return resp_array_bulk(items);
    }

    std::string cmd_hscan(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'hscan'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Hash, false, err);
        if (!err.empty()) {
            return err;
        }
        long long cursor = 0;
        long long count = 10;
        std::string pattern;
        if (!parse_scan_args(argv, 2, cursor, pattern, count, err)) {
            return resp_err(err);
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        if (e) {
            for (const auto &kv : e->value.hash) {
                if (glob_match(pattern, kv.first)) {
                    pairs.push_back(kv);
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        size_t idx = static_cast<size_t>(cursor);
        std::vector<std::optional<std::string>> items;
        for (size_t i = idx; i < pairs.size() && items.size() < static_cast<size_t>(count * 2); ++i) {
            items.emplace_back(pairs[i].first);
            items.emplace_back(pairs[i].second);
            idx = i + 1;
        }
        long long next_cursor = (idx >= pairs.size()) ? 0 : static_cast<long long>(idx);
        std::vector<std::string> out;
        out.push_back(resp_bulk(std::to_string(next_cursor)));
        out.push_back(resp_array_bulk(items));
        return resp_array_raw(out);
    }

    std::vector<std::pair<std::string, double>> zset_sorted(const Entry &e) const {
        std::vector<std::pair<std::string, double>> items;
        items.reserve(e.value.zset.size());
        for (const auto &kv : e.value.zset) {
            items.emplace_back(kv.first, kv.second);
        }
        std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
            if (a.second != b.second) {
                return a.second < b.second;
            }
            return a.first < b.first;
        });
        return items;
    }

    std::string cmd_zadd(const std::vector<std::string> &argv) {
        if (argv.size() < 4 || (argv.size() % 2) != 0) {
            return resp_err("wrong number of arguments for 'zadd'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, true, err);
        if (!err.empty()) {
            return err;
        }
        long long added = 0;
        for (size_t i = 2; i + 1 < argv.size(); i += 2) {
            double score = 0.0;
            if (!parse_double(argv[i], score)) {
                return resp_err("value is not a valid float");
            }
            auto it = e->value.zset.find(argv[i + 1]);
            if (it == e->value.zset.end()) {
                ++added;
            }
            e->value.zset[argv[i + 1]] = score;
        }
        notify("zadd", argv[1]);
        return resp_integer(added);
    }

    std::string cmd_zrem(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'zrem'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        long long removed = 0;
        for (size_t i = 2; i < argv.size(); ++i) {
            removed += e->value.zset.erase(argv[i]);
        }
        if (removed > 0) {
            notify("zrem", argv[1]);
        }
        return resp_integer(removed);
    }

    std::string cmd_zrange(const std::vector<std::string> &argv) {
        if (argv.size() < 4) {
            return resp_err("wrong number of arguments for 'zrange'");
        }
        bool with_scores = (argv.size() == 5 && to_upper(argv[4]) == "WITHSCORES");
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        long long start = 0;
        long long stop = 0;
        if (!parse_int64(argv[2], start) || !parse_int64(argv[3], stop)) {
            return resp_err("value is not an integer or out of range");
        }
        auto items = zset_sorted(*e);
        long long size = static_cast<long long>(items.size());
        start = normalize_index(start, size);
        stop = normalize_index(stop, size);
        if (start < 0) start = 0;
        if (stop >= size) stop = size - 1;
        if (size == 0 || start > stop) {
            return "*0\r\n";
        }
        std::vector<std::optional<std::string>> out;
        for (long long i = start; i <= stop; ++i) {
            out.emplace_back(items[static_cast<size_t>(i)].first);
            if (with_scores) {
                out.emplace_back(std::to_string(items[static_cast<size_t>(i)].second));
            }
        }
        return resp_array_bulk(out);
    }

    std::string cmd_zcard(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'zcard'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(static_cast<long long>(e->value.zset.size()));
    }

    std::string cmd_zscore(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'zscore'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_nil_bulk();
        }
        auto it = e->value.zset.find(argv[2]);
        if (it == e->value.zset.end()) {
            return resp_nil_bulk();
        }
        return resp_bulk(std::to_string(it->second));
    }

    std::string cmd_zscan(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'zscan'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::ZSet, false, err);
        if (!err.empty()) {
            return err;
        }
        long long cursor = 0;
        long long count = 10;
        std::string pattern;
        if (!parse_scan_args(argv, 2, cursor, pattern, count, err)) {
            return resp_err(err);
        }
        std::vector<std::pair<std::string, double>> pairs;
        if (e) {
            for (const auto &kv : e->value.zset) {
                if (glob_match(pattern, kv.first)) {
                    pairs.push_back(kv);
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        size_t idx = static_cast<size_t>(cursor);
        std::vector<std::optional<std::string>> items;
        for (size_t i = idx; i < pairs.size() && items.size() < static_cast<size_t>(count * 2); ++i) {
            items.emplace_back(pairs[i].first);
            items.emplace_back(std::to_string(pairs[i].second));
            idx = i + 1;
        }
        long long next_cursor = (idx >= pairs.size()) ? 0 : static_cast<long long>(idx);
        std::vector<std::string> out;
        out.push_back(resp_bulk(std::to_string(next_cursor)));
        out.push_back(resp_array_bulk(items));
        return resp_array_raw(out);
    }

    std::string cmd_geoadd(const std::vector<std::string> &argv) {
        if (argv.size() < 5 || (argv.size() % 3) != 2) {
            return resp_err("wrong number of arguments for 'geoadd'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Geo, true, err);
        if (!err.empty()) {
            return err;
        }
        long long added = 0;
        for (size_t i = 2; i + 2 < argv.size(); i += 3) {
            double lon = 0.0;
            double lat = 0.0;
            if (!parse_double(argv[i], lon) || !parse_double(argv[i + 1], lat)) {
                return resp_err("invalid longitude or latitude");
            }
            auto it = e->value.geo.find(argv[i + 2]);
            if (it == e->value.geo.end()) {
                ++added;
            }
            e->value.geo[argv[i + 2]] = GeoPoint{lon, lat};
        }
        notify("geoadd", argv[1]);
        return resp_integer(added);
    }

    std::string cmd_geopos(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'geopos'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Geo, false, err);
        if (!err.empty()) {
            return err;
        }
        std::vector<std::string> out;
        if (!e) {
            for (size_t i = 2; i < argv.size(); ++i) {
                out.push_back(resp_null_array());
            }
            return resp_array_raw(out);
        }
        for (size_t i = 2; i < argv.size(); ++i) {
            auto it = e->value.geo.find(argv[i]);
            if (it == e->value.geo.end()) {
                out.push_back(resp_null_array());
            } else {
                std::vector<std::optional<std::string>> pair;
                pair.emplace_back(std::to_string(it->second.lon));
                pair.emplace_back(std::to_string(it->second.lat));
                out.push_back(resp_array_bulk(pair));
            }
        }
        return resp_array_raw(out);
    }

    std::string cmd_geodist(const std::vector<std::string> &argv) {
        if (argv.size() < 4 || argv.size() > 5) {
            return resp_err("wrong number of arguments for 'geodist'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Geo, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_nil_bulk();
        }
        auto it1 = e->value.geo.find(argv[2]);
        auto it2 = e->value.geo.find(argv[3]);
        if (it1 == e->value.geo.end() || it2 == e->value.geo.end()) {
            return resp_nil_bulk();
        }
        double dist_m = geo_distance_m(it1->second.lon, it1->second.lat, it2->second.lon, it2->second.lat);
        double mult = 1.0;
        if (argv.size() == 5) {
            mult = unit_multiplier(argv[4]);
        }
        double dist = dist_m / mult;
        return resp_bulk(std::to_string(dist));
    }

    std::string cmd_georadius(const std::vector<std::string> &argv) {
        if (argv.size() < 6) {
            return resp_err("wrong number of arguments for 'georadius'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Geo, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        double lon = 0.0;
        double lat = 0.0;
        double radius = 0.0;
        if (!parse_double(argv[2], lon) || !parse_double(argv[3], lat) || !parse_double(argv[4], radius)) {
            return resp_err("invalid longitude, latitude or radius");
        }
        double mult = unit_multiplier(argv[5]);
        bool withdist = false;
        bool withcoord = false;
        long long count = -1;
        for (size_t i = 6; i < argv.size(); ++i) {
            std::string opt = to_upper(argv[i]);
            if (opt == "WITHDIST") {
                withdist = true;
            } else if (opt == "WITHCOORD") {
                withcoord = true;
            } else if (opt == "COUNT" && i + 1 < argv.size()) {
                parse_int64(argv[++i], count);
            }
        }
        std::vector<std::pair<std::string, double>> hits;
        for (const auto &kv : e->value.geo) {
            double dist_m = geo_distance_m(lon, lat, kv.second.lon, kv.second.lat);
            double dist = dist_m / mult;
            if (dist <= radius) {
                hits.emplace_back(kv.first, dist);
            }
        }
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
            return a.second < b.second;
        });
        if (count >= 0 && static_cast<long long>(hits.size()) > count) {
            hits.resize(static_cast<size_t>(count));
        }
        std::vector<std::string> out;
        for (const auto &hit : hits) {
            if (!withdist && !withcoord) {
                out.push_back(resp_bulk(hit.first));
            } else {
                std::vector<std::string> row;
                row.push_back(resp_bulk(hit.first));
                if (withdist) {
                    row.push_back(resp_bulk(std::to_string(hit.second)));
                }
                if (withcoord) {
                    auto it = e->value.geo.find(hit.first);
                    if (it != e->value.geo.end()) {
                        std::vector<std::optional<std::string>> coord;
                        coord.emplace_back(std::to_string(it->second.lon));
                        coord.emplace_back(std::to_string(it->second.lat));
                        row.push_back(resp_array_bulk(coord));
                    }
                }
                out.push_back(resp_array_raw(row));
            }
        }
        return resp_array_raw(out);
    }

    std::string cmd_xadd(const std::vector<std::string> &argv) {
        if (argv.size() < 5 || (argv.size() % 2) == 0) {
            return resp_err("wrong number of arguments for 'xadd'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Stream, true, err);
        if (!err.empty()) {
            return err;
        }
        std::string id_str = argv[2];
        StreamId id;
        if (id_str == "*") {
            long long ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count());
            if (ms == e->value.stream.last_id.ms) {
                id = StreamId{ms, e->value.stream.last_id.seq + 1};
            } else {
                id = StreamId{ms, 0};
            }
        } else {
            size_t dash = id_str.find('-');
            if (dash != std::string::npos && id_str.substr(dash + 1) == "*") {
                long long ms = 0;
                if (!parse_int64(id_str.substr(0, dash), ms)) {
                    return resp_err("invalid stream id");
                }
                long long seq = 0;
                if (ms == e->value.stream.last_id.ms) {
                    seq = e->value.stream.last_id.seq + 1;
                }
                id = StreamId{ms, seq};
            } else if (!parse_stream_id(id_str, id)) {
                return resp_err("invalid stream id");
            }
        }
        if (compare_stream_id(id, e->value.stream.last_id) <= 0) {
            return resp_err("The ID specified is equal or smaller than the target stream top item");
        }
        StreamEntry entry;
        entry.id = id;
        for (size_t i = 3; i + 1 < argv.size(); i += 2) {
            entry.fields.emplace_back(argv[i], argv[i + 1]);
        }
        e->value.stream.entries.push_back(std::move(entry));
        e->value.stream.last_id = id;
        notify("xadd", argv[1]);
        return resp_bulk(stream_id_to_string(id));
    }

    std::string cmd_xdel(const std::vector<std::string> &argv) {
        if (argv.size() < 3) {
            return resp_err("wrong number of arguments for 'xdel'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Stream, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        long long removed = 0;
        for (size_t i = 2; i < argv.size(); ++i) {
            StreamId id;
            if (!parse_stream_id(argv[i], id)) {
                continue;
            }
            auto &entries = e->value.stream.entries;
            auto it = std::remove_if(entries.begin(), entries.end(), [&](const StreamEntry &en) {
                return compare_stream_id(en.id, id) == 0;
            });
            if (it != entries.end()) {
                removed += std::distance(it, entries.end());
                entries.erase(it, entries.end());
            }
        }
        if (removed > 0) {
            notify("xdel", argv[1]);
        }
        return resp_integer(removed);
    }

    std::string cmd_xlen(const std::vector<std::string> &argv) {
        if (argv.size() != 2) {
            return resp_err("wrong number of arguments for 'xlen'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Stream, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return resp_integer(0);
        }
        return resp_integer(static_cast<long long>(e->value.stream.entries.size()));
    }

    std::string cmd_xrange(const std::vector<std::string> &argv) {
        if (argv.size() < 4) {
            return resp_err("wrong number of arguments for 'xrange'");
        }
        std::string err;
        Entry *e = get_or_create(argv[1], Value::Type::Stream, false, err);
        if (!err.empty()) {
            return err;
        }
        if (!e) {
            return "*0\r\n";
        }
        StreamId start;
        StreamId end;
        if (!parse_stream_start_end(argv[2], start, true) || !parse_stream_start_end(argv[3], end, false)) {
            return resp_err("invalid stream id");
        }
        long long count = -1;
        if (argv.size() >= 6 && to_upper(argv[4]) == "COUNT") {
            parse_int64(argv[5], count);
        }
        std::vector<std::string> out;
        for (const auto &en : e->value.stream.entries) {
            if (compare_stream_id(en.id, start) < 0) {
                continue;
            }
            if (compare_stream_id(en.id, end) > 0) {
                break;
            }
            std::vector<std::string> entry;
            entry.push_back(resp_bulk(stream_id_to_string(en.id)));
            std::vector<std::string> field_array;
            for (const auto &fv : en.fields) {
                field_array.push_back(resp_bulk(fv.first));
                field_array.push_back(resp_bulk(fv.second));
            }
            entry.push_back(resp_array_raw(field_array));
            out.push_back(resp_array_raw(entry));
            if (count > 0 && static_cast<long long>(out.size()) >= count) {
                break;
            }
        }
        return resp_array_raw(out);
    }

    std::string cmd_xread(const std::vector<std::string> &argv) {
        if (argv.size() < 4) {
            return resp_err("wrong number of arguments for 'xread'");
        }
        size_t idx = 1;
        long long count = -1;
        long long block_ms = -1;
        while (idx < argv.size() && to_upper(argv[idx]) != "STREAMS") {
            std::string opt = to_upper(argv[idx]);
            if (opt == "COUNT" && idx + 1 < argv.size()) {
                parse_int64(argv[idx + 1], count);
                idx += 2;
            } else if (opt == "BLOCK" && idx + 1 < argv.size()) {
                parse_int64(argv[idx + 1], block_ms);
                idx += 2;
            } else {
                return resp_err("syntax error");
            }
        }
        if (idx >= argv.size() || to_upper(argv[idx]) != "STREAMS") {
            return resp_err("syntax error");
        }
        ++idx;
        size_t remaining = argv.size() - idx;
        if (remaining == 0 || (remaining % 2) != 0) {
            return resp_err("wrong number of arguments for 'xread'");
        }
        size_t num_streams = remaining / 2;
        std::vector<std::string> keys(argv.begin() + idx, argv.begin() + idx + num_streams);
        std::vector<std::string> ids(argv.begin() + idx + num_streams, argv.end());

        auto start = std::chrono::steady_clock::now();
        while (true) {
            std::vector<std::string> top_array;
            for (size_t i = 0; i < keys.size(); ++i) {
                std::string err;
                Entry *e = get_or_create(keys[i], Value::Type::Stream, false, err);
                if (!err.empty()) {
                    return err;
                }
                if (!e) {
                    continue;
                }
                StreamId last_id;
                if (ids[i] == "$") {
                    last_id = e->value.stream.last_id;
                } else if (!parse_stream_id(ids[i], last_id)) {
                    return resp_err("invalid stream id");
                }
                std::vector<std::string> entries;
                for (const auto &en : e->value.stream.entries) {
                    if (compare_stream_id(en.id, last_id) <= 0) {
                        continue;
                    }
                    std::vector<std::string> entry;
                    entry.push_back(resp_bulk(stream_id_to_string(en.id)));
                    std::vector<std::string> field_array;
                    for (const auto &fv : en.fields) {
                        field_array.push_back(resp_bulk(fv.first));
                        field_array.push_back(resp_bulk(fv.second));
                    }
                    entry.push_back(resp_array_raw(field_array));
                    entries.push_back(resp_array_raw(entry));
                    if (count > 0 && static_cast<long long>(entries.size()) >= count) {
                        break;
                    }
                }
                if (!entries.empty()) {
                    std::vector<std::string> stream_array;
                    stream_array.push_back(resp_bulk(keys[i]));
                    stream_array.push_back(resp_array_raw(entries));
                    top_array.push_back(resp_array_raw(stream_array));
                }
            }
            if (!top_array.empty()) {
                return resp_array_raw(top_array);
            }
            if (block_ms < 0) {
                break;
            }
            if (block_ms == 0) {
                return resp_null_array();
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= block_ms) {
                return resp_null_array();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return resp_null_array();
    }

    std::string cmd_save(const std::vector<std::string> &argv) {
        std::string path = persistence_path_;
        if (argv.size() == 2) {
            path = argv[1];
        }
        if (path.empty()) {
            return resp_err("no persistence path configured");
        }
        std::string err;
        if (!save_to_file(path, err)) {
            return resp_err(err);
        }
        return resp_simple("OK");
    }

    std::string cmd_load(const std::vector<std::string> &argv) {
        std::string path = persistence_path_;
        if (argv.size() == 2) {
            path = argv[1];
        }
        if (path.empty()) {
            return resp_err("no persistence path configured");
        }
        std::string err;
        if (!load_from_file(path, err)) {
            return resp_err(err);
        }
        return resp_simple("OK");
    }

    std::string cmd_bgrewriteaof(const std::vector<std::string> &argv) {
        std::string path = aof_path_;
        if (argv.size() == 2) {
            path = argv[1];
        }
        if (path.empty()) {
            return resp_err("no AOF path configured");
        }
        if (aof_rewrite_in_progress_) {
            return resp_err("AOF rewrite already in progress");
        }
        aof_rewrite_in_progress_ = true;
        std::thread([this, path]() {
            std::string err;
            if (!rewrite_aof(path, err)) {
                aof_last_error_ = err;
            } else {
                aof_last_error_.clear();
            }
            aof_rewrite_in_progress_ = false;
        }).detach();
        return resp_simple("OK");
    }

    std::string cmd_publish(const std::vector<std::string> &argv) {
        if (argv.size() != 3) {
            return resp_err("wrong number of arguments for 'publish'");
        }
        if (!publish_cb_) {
            return resp_integer(0);
        }
        int count = publish_cb_(argv[1], argv[2]);
        return resp_integer(count);
    }
};

struct ClientConn {
    int fd = -1;
    std::string inbuf;
    bool in_multi = false;
    std::vector<std::vector<std::string>> queued;
    std::unordered_set<std::string> subs;
    long long id = 0;
    std::string addr;
    std::string user = "default";
    bool authed = true;
};

static bool send_all(int fd, const std::string &data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t sent = send(fd, data.data() + total, data.size() - total, 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

static bool parse_resp_array(const std::string &buf, size_t &consumed, std::vector<std::string> &out) {
    if (buf.empty() || buf[0] != '*') {
        return false;
    }
    size_t idx = 1;
    size_t line_end = buf.find("\r\n", idx);
    if (line_end == std::string::npos) {
        return false;
    }
    long long count = 0;
    if (!parse_int64(buf.substr(idx, line_end - idx), count) || count < 0) {
        return false;
    }
    idx = line_end + 2;
    out.clear();
    for (long long i = 0; i < count; ++i) {
        if (idx >= buf.size() || buf[idx] != '$') {
            return false;
        }
        ++idx;
        line_end = buf.find("\r\n", idx);
        if (line_end == std::string::npos) {
            return false;
        }
        long long len = 0;
        if (!parse_int64(buf.substr(idx, line_end - idx), len) || len < 0) {
            return false;
        }
        idx = line_end + 2;
        if (static_cast<size_t>(len) + idx + 2 > buf.size()) {
            return false;
        }
        out.push_back(buf.substr(idx, static_cast<size_t>(len)));
        idx += static_cast<size_t>(len);
        if (buf.substr(idx, 2) != "\r\n") {
            return false;
        }
        idx += 2;
    }
    consumed = idx;
    return true;
}

static bool parse_inline(const std::string &buf, size_t &consumed, std::vector<std::string> &out) {
    size_t line_end = buf.find('\n');
    if (line_end == std::string::npos) {
        return false;
    }
    std::string line = buf.substr(0, line_end);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    out = split_args(line);
    consumed = line_end + 1;
    return true;
}

static std::vector<std::vector<std::string>> parse_commands(std::string &buffer) {
    std::vector<std::vector<std::string>> cmds;
    while (!buffer.empty()) {
        std::vector<std::string> cmd;
        size_t consumed = 0;
        bool ok = false;
        if (buffer[0] == '*') {
            ok = parse_resp_array(buffer, consumed, cmd);
        } else {
            ok = parse_inline(buffer, consumed, cmd);
        }
        if (!ok || consumed == 0) {
            break;
        }
        buffer.erase(0, consumed);
        if (!cmd.empty()) {
            cmds.push_back(std::move(cmd));
        }
    }
    return cmds;
}

static std::string subscribe_msg(const std::string &kind, const std::string &channel, size_t count) {
    std::vector<std::optional<std::string>> items;
    items.emplace_back(kind);
    items.emplace_back(channel);
    items.emplace_back(std::to_string(count));
    return resp_array_bulk(items);
}

static std::string publish_msg(const std::string &channel, const std::string &payload) {
    std::vector<std::optional<std::string>> items;
    items.emplace_back("message");
    items.emplace_back(channel);
    items.emplace_back(payload);
    return resp_array_bulk(items);
}

struct AclUser {
    std::string name = "default";
    bool enabled = true;
    bool all_commands = true;
    std::string password;
    std::unordered_set<std::string> allowed;
};

static bool acl_allows(const AclUser &user, const std::string &cmd) {
    if (!user.enabled) {
        return false;
    }
    if (user.all_commands) {
        return true;
    }
    return user.allowed.count(cmd) > 0;
}

static std::string acl_user_line(const AclUser &user) {
    std::ostringstream oss;
    oss << "user " << user.name << " ";
    oss << (user.enabled ? "on " : "off ");
    if (user.password.empty()) {
        oss << "nopass ";
    } else {
        oss << ">" << user.password << " ";
    }
    if (user.all_commands) {
        oss << "+@all";
    } else {
        for (const auto &cmd : user.allowed) {
            oss << " +" << cmd;
        }
    }
    return oss.str();
}

static std::string blocking_pop(RedisClone &db, std::mutex &db_mu, const std::vector<std::string> &argv,
                                bool left, std::atomic<bool> &running) {
    if (argv.size() < 3) {
        return resp_err("wrong number of arguments for 'blpop'");
    }
    long long timeout = 0;
    if (!parse_int64(argv.back(), timeout) || timeout < 0) {
        return resp_err("timeout is not an integer or out of range");
    }
    std::vector<std::string> keys(argv.begin() + 1, argv.end() - 1);
    auto start = std::chrono::steady_clock::now();
    while (running) {
        for (const auto &key : keys) {
            std::vector<std::string> pop_cmd = {left ? "LPOP" : "RPOP", key};
            std::string resp;
            {
                std::lock_guard<std::mutex> lock(db_mu);
                resp = db.exec(pop_cmd);
            }
            if (!resp.empty() && resp[0] == '-') {
                return resp;
            }
            bool is_nil = false;
            std::string value;
            if (!parse_resp_bulk_value(resp, value, is_nil)) {
                return resp;
            }
            if (!is_nil) {
                std::vector<std::optional<std::string>> items;
                items.emplace_back(key);
                items.emplace_back(value);
                return resp_array_bulk(items);
            }
        }
        if (timeout > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout) {
                return resp_null_array();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return resp_null_array();
}

static int run_server(RedisClone &db, std::mutex &db_mu, int port, std::atomic<bool> &running, bool enable_notify) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 64) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(listen_fd);
        return 1;
    }

    std::unordered_map<int, ClientConn> clients;
    std::unordered_map<std::string, std::unordered_set<int>> channels;
    std::unordered_map<std::string, AclUser> users;
    users["default"] = AclUser{};
    long long next_client_id = 1;

    db.set_client_count_callback([&]() { return clients.size(); });
    db.set_tcp_port(port);

    db.set_publish_callback([&](const std::string &channel, const std::string &message) -> int {
        int count = 0;
        auto it = channels.find(channel);
        if (it == channels.end()) {
            return 0;
        }
        std::string msg = publish_msg(channel, message);
        for (int fd : it->second) {
            if (send_all(fd, msg)) {
                ++count;
            }
        }
        return count;
    });

    if (enable_notify) {
        db.set_notify_callback([&](const std::string &event, const std::string &key) {
            if (key.empty()) {
                return;
            }
            std::string keyspace = "__keyspace@0__:" + key;
            std::string keyevent = "__keyevent@0__:" + event;
            std::string payload = event;
            std::string msg_keyspace = publish_msg(keyspace, payload);
            std::string msg_keyevent = publish_msg(keyevent, key);
            auto it1 = channels.find(keyspace);
            if (it1 != channels.end()) {
                for (int fd : it1->second) {
                    send_all(fd, msg_keyspace);
                }
            }
            auto it2 = channels.find(keyevent);
            if (it2 != channels.end()) {
                for (int fd : it2->second) {
                    send_all(fd, msg_keyevent);
                }
            }
        });
    }

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;
        for (const auto &kv : clients) {
            FD_SET(kv.first, &readfds);
            max_fd = std::max(max_fd, kv.first);
        }
        timeval tv{};
        tv.tv_sec = 1;
        int rv = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (FD_ISSET(listen_fd, &readfds)) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            int client_fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client_fd >= 0) {
                ClientConn conn;
                conn.fd = client_fd;
                conn.id = next_client_id++;
                conn.user = "default";
                conn.authed = users["default"].password.empty();
                char addrbuf[INET_ADDRSTRLEN];
                const char *ip = inet_ntop(AF_INET, &peer.sin_addr, addrbuf, sizeof(addrbuf));
                if (ip) {
                    conn.addr = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
                } else {
                    conn.addr = "unknown";
                }
                clients[client_fd] = std::move(conn);
            }
        }

        std::vector<int> to_close;
        for (auto &kv : clients) {
            int fd = kv.first;
            ClientConn &client = kv.second;
            if (!FD_ISSET(fd, &readfds)) {
                continue;
            }
            char buf[4096];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                to_close.push_back(fd);
                continue;
            }
            client.inbuf.append(buf, static_cast<size_t>(n));
            auto cmds = parse_commands(client.inbuf);
            for (const auto &cmd : cmds) {
                if (cmd.empty()) {
                    continue;
                }
                std::string upper = to_upper(cmd[0]);
                if (upper == "AUTH") {
                    if (cmd.size() != 2 && cmd.size() != 3) {
                        send_all(fd, resp_err("wrong number of arguments for 'auth'"));
                        continue;
                    }
                    std::string user = "default";
                    std::string pass;
                    if (cmd.size() == 2) {
                        pass = cmd[1];
                    } else {
                        user = cmd[1];
                        pass = cmd[2];
                    }
                    auto uit = users.find(user);
                    if (uit == users.end() || !uit->second.enabled) {
                        send_all(fd, resp_err("invalid username-password pair"));
                        continue;
                    }
                    if (!uit->second.password.empty() && uit->second.password != pass) {
                        send_all(fd, resp_err("invalid username-password pair"));
                        continue;
                    }
                    client.authed = true;
                    client.user = user;
                    send_all(fd, resp_simple("OK"));
                    continue;
                }
                if (upper == "ACL") {
                    if (cmd.size() < 2) {
                        send_all(fd, resp_err("wrong number of arguments for 'acl'"));
                        continue;
                    }
                    std::string sub = to_upper(cmd[1]);
                    if (sub == "LIST") {
                        std::vector<std::optional<std::string>> items;
                        for (const auto &kv : users) {
                            items.emplace_back(acl_user_line(kv.second));
                        }
                        send_all(fd, resp_array_bulk(items));
                        continue;
                    }
                    if (sub == "WHOAMI") {
                        send_all(fd, resp_bulk(client.user));
                        continue;
                    }
                    if (sub == "SETUSER") {
                        if (cmd.size() < 3) {
                            send_all(fd, resp_err("wrong number of arguments for 'acl setuser'"));
                            continue;
                        }
                        std::string uname = cmd[2];
                        AclUser &u = users[uname];
                        u.name = uname;
                        for (size_t i = 3; i < cmd.size(); ++i) {
                            std::string tok = cmd[i];
                            std::string up = to_upper(tok);
                            if (up == "ON") {
                                u.enabled = true;
                            } else if (up == "OFF") {
                                u.enabled = false;
                            } else if (up == "NOPASS") {
                                u.password.clear();
                            } else if (up == "RESET") {
                                u.allowed.clear();
                                u.all_commands = false;
                            } else if (up == "ALLCOMMANDS" || up == "+@ALL") {
                                u.all_commands = true;
                            } else if (!tok.empty() && tok[0] == '>') {
                                u.password = tok.substr(1);
                            } else if (!tok.empty() && (tok[0] == '+' || tok[0] == '-')) {
                                std::string cmdname = to_upper(tok.substr(1));
                                if (tok[0] == '+') {
                                    u.allowed.insert(cmdname);
                                    u.all_commands = false;
                                } else {
                                    u.allowed.erase(cmdname);
                                }
                            }
                        }
                        send_all(fd, resp_simple("OK"));
                        continue;
                    }
                    send_all(fd, resp_err("unknown acl subcommand"));
                    continue;
                }

                if (!client.authed) {
                    send_all(fd, resp_err("NOAUTH Authentication required."));
                    continue;
                }
                auto uit = users.find(client.user);
                if (uit != users.end() && !acl_allows(uit->second, upper)) {
                    send_all(fd, resp_err("NOPERM this user has no permissions to run the command"));
                    continue;
                }
                if (upper == "SUBSCRIBE") {
                    for (size_t i = 1; i < cmd.size(); ++i) {
                        channels[cmd[i]].insert(fd);
                        client.subs.insert(cmd[i]);
                        send_all(fd, subscribe_msg("subscribe", cmd[i], client.subs.size()));
                    }
                    continue;
                }
                if (upper == "UNSUBSCRIBE") {
                    if (cmd.size() == 1) {
                        std::vector<std::string> to_remove(client.subs.begin(), client.subs.end());
                        for (const auto &ch : to_remove) {
                            channels[ch].erase(fd);
                            client.subs.erase(ch);
                            send_all(fd, subscribe_msg("unsubscribe", ch, client.subs.size()));
                        }
                    } else {
                        for (size_t i = 1; i < cmd.size(); ++i) {
                            channels[cmd[i]].erase(fd);
                            client.subs.erase(cmd[i]);
                            send_all(fd, subscribe_msg("unsubscribe", cmd[i], client.subs.size()));
                        }
                    }
                    continue;
                }

                if (upper == "MULTI") {
                    if (client.in_multi) {
                        send_all(fd, resp_err("MULTI calls can not be nested"));
                    } else {
                        client.in_multi = true;
                        client.queued.clear();
                        send_all(fd, resp_simple("OK"));
                    }
                    continue;
                }
                if (upper == "DISCARD") {
                    if (!client.in_multi) {
                        send_all(fd, resp_err("DISCARD without MULTI"));
                    } else {
                        client.in_multi = false;
                        client.queued.clear();
                        send_all(fd, resp_simple("OK"));
                    }
                    continue;
                }
                if (upper == "EXEC") {
                    if (!client.in_multi) {
                        send_all(fd, resp_err("EXEC without MULTI"));
                    } else {
                        client.in_multi = false;
                        std::vector<std::string> replies;
                        for (const auto &qcmd : client.queued) {
                            std::lock_guard<std::mutex> lock(db_mu);
                            replies.push_back(db.exec(qcmd));
                        }
                        client.queued.clear();
                        send_all(fd, resp_array_raw(replies));
                    }
                    continue;
                }

                if (upper == "CLIENT" && cmd.size() >= 2 && to_upper(cmd[1]) == "LIST") {
                    std::ostringstream oss;
                    for (const auto &ckv : clients) {
                        const ClientConn &c = ckv.second;
                        oss << "id=" << c.id << " addr=" << c.addr << " user=" << c.user
                            << " subs=" << c.subs.size() << "\n";
                    }
                    send_all(fd, resp_bulk(oss.str()));
                    continue;
                }

                if (client.in_multi) {
                    client.queued.push_back(cmd);
                    send_all(fd, resp_simple("QUEUED"));
                    continue;
                }

                if (upper == "BLPOP" || upper == "BRPOP") {
                    auto start = std::chrono::steady_clock::now();
                    std::string resp = blocking_pop(db, db_mu, cmd, upper == "BLPOP", running);
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    db.record_slowlog(cmd, dur);
                    send_all(fd, resp);
                    continue;
                }

                std::string resp;
                auto start = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock(db_mu);
                    resp = db.exec(cmd);
                }
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                db.record_slowlog(cmd, dur);
                send_all(fd, resp);
            }
        }

        for (int fd : to_close) {
            auto it = clients.find(fd);
            if (it != clients.end()) {
                for (const auto &ch : it->second.subs) {
                    channels[ch].erase(fd);
                }
            }
            close(fd);
            clients.erase(fd);
        }
    }

    close(listen_fd);
    for (auto &kv : clients) {
        close(kv.first);
    }
    return 0;
}

static void run_cli(RedisClone &db, std::mutex &db_mu, std::atomic<bool> &running) {
    std::string line;
    bool in_multi = false;
    std::vector<std::vector<std::string>> queued;
    std::cout << "Redis clone ready. Type commands like: SET key value" << std::endl;
    while (std::getline(std::cin, line)) {
        auto args = split_args(line);
        if (args.empty()) {
            continue;
        }
        std::string cmd = to_upper(args[0]);
        if (cmd == "QUIT" || cmd == "EXIT") {
            std::cout << resp_simple("OK");
            break;
        }
        if (cmd == "MULTI") {
            if (in_multi) {
                std::cout << resp_err("MULTI calls can not be nested");
            } else {
                in_multi = true;
                queued.clear();
                std::cout << resp_simple("OK");
            }
            continue;
        }
        if (cmd == "DISCARD") {
            if (!in_multi) {
                std::cout << resp_err("DISCARD without MULTI");
            } else {
                in_multi = false;
                queued.clear();
                std::cout << resp_simple("OK");
            }
            continue;
        }
        if (cmd == "EXEC") {
            if (!in_multi) {
                std::cout << resp_err("EXEC without MULTI");
            } else {
                in_multi = false;
                std::vector<std::string> replies;
                for (const auto &qcmd : queued) {
                    std::lock_guard<std::mutex> lock(db_mu);
                    replies.push_back(db.exec(qcmd));
                }
                queued.clear();
                std::cout << resp_array_raw(replies);
            }
            continue;
        }
        if (in_multi) {
            queued.push_back(args);
            std::cout << resp_simple("QUEUED");
            continue;
        }

        if (cmd == "BLPOP" || cmd == "BRPOP") {
            auto start = std::chrono::steady_clock::now();
            std::string resp = blocking_pop(db, db_mu, args, cmd == "BLPOP", running);
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            db.record_slowlog(args, dur);
            std::cout << resp;
            continue;
        }
        std::string resp;
        auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(db_mu);
            resp = db.exec(args);
        }
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        db.record_slowlog(args, dur);
        std::cout << resp;
        std::cout.flush();
    }
    running = false;
}

} // namespace redisclone

namespace redisclone {

const char kHelpText[] = R"HELP(
Redis Clone Help

Core Commands
PING - Ping the server
ECHO message - Echo a message
SET key value [EX seconds|PX ms] - Set string
GET key - Get string
GETSET key value - Set and return old value
SETNX key value - Set if not exists
MSET key value [key value...] - Set multiple
MSETNX key value [key value...] - Set multiple if none exist
MGET key [key...] - Get multiple
APPEND key value - Append to string
STRLEN key - String length
INCR key - Increment integer
DECR key - Decrement integer
INCRBY key delta - Increment by delta
DECRBY key delta - Decrement by delta
DEL key [key...] - Delete keys
EXISTS key [key...] - Check existence
EXPIRE key seconds - Set expire seconds
PEXPIRE key ms - Set expire milliseconds
EXPIREAT key unix - Set expire at seconds
PEXPIREAT key unixms - Set expire at ms
PERSIST key - Remove TTL
TTL key - TTL in seconds
PTTL key - TTL in ms
KEYS pattern - Glob match keys
SCAN cursor [MATCH pattern] [COUNT n] - Incremental scan
TYPE key - Key type
DBSIZE - Number of keys
FLUSHDB - Clear DB
RENAME key newkey - Rename
RENAMENX key newkey - Rename if newkey doesn't exist

List Commands
LPUSH key value [value...] - Push left
RPUSH key value [value...] - Push right
LPOP key - Pop left
RPOP key - Pop right
BLPOP key [key...] timeout - Blocking left pop
BRPOP key [key...] timeout - Blocking right pop
LRANGE key start stop - Range
LLEN key - List length
LINDEX key index - List index
LSET key index value - Set element
LTRIM key start stop - Trim range

Set Commands
SADD key member [member...] - Add members
SREM key member [member...] - Remove members
SMEMBERS key - List members
SCARD key - Set size
SISMEMBER key member - Check member
SMOVE src dst member - Move member
SUNION key [key...] - Union
SINTER key [key...] - Intersect
SSCAN key cursor [MATCH pattern] [COUNT n] - Scan set

Hash Commands
HSET key field value [field value...] - Set fields
HMSET key field value [field value...] - Set fields
HGET key field - Get field
HMGET key field [field...] - Get multiple
HDEL key field [field...] - Delete fields
HEXISTS key field - Check field
HLEN key - Number of fields
HGETALL key - All fields
HSCAN key cursor [MATCH pattern] [COUNT n] - Scan hash

Sorted Set Commands
ZADD key score member [score member...] - Add
ZREM key member [member...] - Remove
ZRANGE key start stop [WITHSCORES] - Range
ZCARD key - Count
ZSCORE key member - Score
ZSCAN key cursor [MATCH pattern] [COUNT n] - Scan

Bitmap Commands
SETBIT key offset value - Set bit
GETBIT key offset - Get bit
BITCOUNT key [start end] - Count bits
BITOP op dest key [key...] - Bit operations

Geo Commands
GEOADD key lon lat member [lon lat member...] - Add points
GEOPOS key member [member...] - Positions
GEODIST key member1 member2 [unit] - Distance
GEORADIUS key lon lat radius unit [WITHDIST] [WITHCOORD] [COUNT n] - Radius query

Stream Commands
XADD key id field value [field value...] - Add entry
XDEL key id [id...] - Delete entry
XLEN key - Stream length
XRANGE key start end [COUNT n] - Range
XREAD [COUNT n] [BLOCK ms] STREAMS key id [key id...] - Read

Persistence
SAVE [file] - Save snapshot
LOAD [file] - Load snapshot
REWRITEAOF [file] - Rewrite AOF
BGREWRITEAOF [file] - Rewrite AOF in background

Pub/Sub
PUBLISH channel message - Publish
SUBSCRIBE channel [channel...] - Subscribe
UNSUBSCRIBE [channel...] - Unsubscribe

Server
INFO - Info
SLOWLOG LEN|GET [n]|RESET - Slowlog
CLIENT LIST - Client list
AUTH [user] password - Authenticate
ACL LIST|WHOAMI|SETUSER - ACL management

Examples
EXAMPLE 001: SET k1 v1
EXAMPLE 002: GET k1
EXAMPLE 003: INCR counter
EXAMPLE 004: LPUSH list a b c
EXAMPLE 005: LRANGE list 0 -1
EXAMPLE 006: SADD set a b c
EXAMPLE 007: SMEMBERS set
EXAMPLE 008: HSET hash f1 v1 f2 v2
EXAMPLE 009: HGET hash f1
EXAMPLE 010: ZADD z 1 a 2 b
EXAMPLE 011: ZRANGE z 0 -1 WITHSCORES
EXAMPLE 012: GEOADD geo 13.361389 38.115556 a
EXAMPLE 013: GEODIST geo a b km
EXAMPLE 014: XADD stream * f1 v1 f2 v2
EXAMPLE 015: XRANGE stream - +
EXAMPLE 016: SETBIT bits 10 1
EXAMPLE 017: BITCOUNT bits
EXAMPLE 018: EXPIRE k1 60
EXAMPLE 019: TTL k1
EXAMPLE 020: PERSIST k1
EXAMPLE 021: SCAN 0 MATCH k* COUNT 10
EXAMPLE 022: HSCAN hash 0 MATCH f* COUNT 10
EXAMPLE 023: ZSCAN z 0 MATCH a* COUNT 10
EXAMPLE 024: SSCAN set 0 MATCH a* COUNT 10
EXAMPLE 025: BLPOP list 5
EXAMPLE 026: BRPOP list 5
EXAMPLE 027: PUBLISH news hello
EXAMPLE 028: INFO
EXAMPLE 029: SLOWLOG LEN
EXAMPLE 030: CLIENT LIST
EXAMPLE 031: AUTH password
EXAMPLE 032: ACL LIST
EXAMPLE 033: ACL SETUSER default on >pass allcommands
EXAMPLE 034: XREAD COUNT 2 STREAMS stream 0-0
EXAMPLE 035: GEORADIUS geo 15 37 200 km WITHDIST WITHCOORD COUNT 5
EXAMPLE 036: BITOP AND out bits bits2
EXAMPLE 037: MSET k1 v1 k2 v2 k3 v3
EXAMPLE 038: MGET k1 k2 k3
EXAMPLE 039: RENAME k1 k1_new
EXAMPLE 040: RENAMENX k2 k2_new
EXAMPLE 041: SETNX kx vx
EXAMPLE 042: GETSET kx vy
EXAMPLE 043: STRLEN kx
EXAMPLE 044: APPEND kx zz
EXAMPLE 045: LINDEX list 0
EXAMPLE 046: LSET list 0 z
EXAMPLE 047: LTRIM list 0 3
EXAMPLE 048: SREM set a
EXAMPLE 049: SMOVE set set2 b
EXAMPLE 050: HDEL hash f2
EXAMPLE 051: HEXISTS hash f1
EXAMPLE 052: ZSCORE z a
EXAMPLE 053: ZREM z b
EXAMPLE 054: ZCARD z
EXAMPLE 055: GEOADD geo 12.1 31.2 c
EXAMPLE 056: GEOPOS geo a b c
EXAMPLE 057: XDEL stream 0-1
EXAMPLE 058: XLEN stream
EXAMPLE 059: EXPIREAT k1 9999999999
EXAMPLE 060: PEXPIREAT k1 9999999999
EXAMPLE 061: KEYS *
EXAMPLE 062: TYPE list
EXAMPLE 063: FLUSHDB
EXAMPLE 064: SAVE dump.rc
EXAMPLE 065: LOAD dump.rc
EXAMPLE 066: REWRITEAOF dump.aof
EXAMPLE 067: BGREWRITEAOF dump.aof
EXAMPLE 068: SET demo:key:1 value1
EXAMPLE 069: SET demo:key:2 value2
EXAMPLE 070: SET demo:key:3 value3
EXAMPLE 071: SET demo:key:4 value4
EXAMPLE 072: SET demo:key:5 value5
EXAMPLE 073: SET demo:key:6 value6
EXAMPLE 074: SET demo:key:7 value7
EXAMPLE 075: SET demo:key:8 value8
EXAMPLE 076: SET demo:key:9 value9
EXAMPLE 077: SET demo:key:10 value10
EXAMPLE 078: SET demo:key:11 value11
EXAMPLE 079: SET demo:key:12 value12
EXAMPLE 080: SET demo:key:13 value13
EXAMPLE 081: SET demo:key:14 value14
EXAMPLE 082: SET demo:key:15 value15
EXAMPLE 083: SET demo:key:16 value16
EXAMPLE 084: SET demo:key:17 value17
EXAMPLE 085: SET demo:key:18 value18
EXAMPLE 086: SET demo:key:19 value19
EXAMPLE 087: SET demo:key:20 value20
EXAMPLE 088: SET demo:key:21 value21
EXAMPLE 089: SET demo:key:22 value22
EXAMPLE 090: SET demo:key:23 value23
EXAMPLE 091: SET demo:key:24 value24
EXAMPLE 092: SET demo:key:25 value25
EXAMPLE 093: SET demo:key:26 value26
EXAMPLE 094: SET demo:key:27 value27
EXAMPLE 095: SET demo:key:28 value28
EXAMPLE 096: SET demo:key:29 value29
EXAMPLE 097: SET demo:key:30 value30
EXAMPLE 098: SET demo:key:31 value31
EXAMPLE 099: SET demo:key:32 value32
EXAMPLE 100: SET demo:key:33 value33
EXAMPLE 101: SET demo:key:34 value34
EXAMPLE 102: SET demo:key:35 value35
EXAMPLE 103: SET demo:key:36 value36
EXAMPLE 104: SET demo:key:37 value37
EXAMPLE 105: SET demo:key:38 value38
EXAMPLE 106: SET demo:key:39 value39
EXAMPLE 107: SET demo:key:40 value40
EXAMPLE 108: SET demo:key:41 value41
EXAMPLE 109: SET demo:key:42 value42
EXAMPLE 110: SET demo:key:43 value43
EXAMPLE 111: SET demo:key:44 value44
EXAMPLE 112: SET demo:key:45 value45
EXAMPLE 113: SET demo:key:46 value46
EXAMPLE 114: SET demo:key:47 value47
EXAMPLE 115: SET demo:key:48 value48
EXAMPLE 116: SET demo:key:49 value49
EXAMPLE 117: SET demo:key:50 value50
EXAMPLE 118: SET demo:key:51 value51
EXAMPLE 119: SET demo:key:52 value52
EXAMPLE 120: SET demo:key:53 value53
EXAMPLE 121: SET demo:key:54 value54
EXAMPLE 122: SET demo:key:55 value55
EXAMPLE 123: SET demo:key:56 value56
EXAMPLE 124: SET demo:key:57 value57
EXAMPLE 125: SET demo:key:58 value58
EXAMPLE 126: SET demo:key:59 value59
EXAMPLE 127: SET demo:key:60 value60
EXAMPLE 128: SET demo:key:61 value61
EXAMPLE 129: SET demo:key:62 value62
EXAMPLE 130: SET demo:key:63 value63
EXAMPLE 131: SET demo:key:64 value64
EXAMPLE 132: SET demo:key:65 value65
EXAMPLE 133: SET demo:key:66 value66
EXAMPLE 134: SET demo:key:67 value67
EXAMPLE 135: SET demo:key:68 value68
EXAMPLE 136: SET demo:key:69 value69
EXAMPLE 137: SET demo:key:70 value70
EXAMPLE 138: SET demo:key:71 value71
EXAMPLE 139: SET demo:key:72 value72
EXAMPLE 140: SET demo:key:73 value73
EXAMPLE 141: SET demo:key:74 value74
EXAMPLE 142: SET demo:key:75 value75
EXAMPLE 143: SET demo:key:76 value76
EXAMPLE 144: SET demo:key:77 value77
EXAMPLE 145: SET demo:key:78 value78
EXAMPLE 146: SET demo:key:79 value79
EXAMPLE 147: SET demo:key:80 value80
EXAMPLE 148: SET demo:key:81 value81
EXAMPLE 149: SET demo:key:82 value82
EXAMPLE 150: SET demo:key:83 value83
EXAMPLE 151: SET demo:key:84 value84
EXAMPLE 152: SET demo:key:85 value85
EXAMPLE 153: SET demo:key:86 value86
EXAMPLE 154: SET demo:key:87 value87
EXAMPLE 155: SET demo:key:88 value88
EXAMPLE 156: SET demo:key:89 value89
EXAMPLE 157: SET demo:key:90 value90
EXAMPLE 158: SET demo:key:91 value91
EXAMPLE 159: SET demo:key:92 value92
EXAMPLE 160: SET demo:key:93 value93
EXAMPLE 161: SET demo:key:94 value94
EXAMPLE 162: SET demo:key:95 value95
EXAMPLE 163: SET demo:key:96 value96
EXAMPLE 164: SET demo:key:97 value97
EXAMPLE 165: SET demo:key:98 value98
EXAMPLE 166: SET demo:key:99 value99
EXAMPLE 167: SET demo:key:100 value100
EXAMPLE 168: SET demo:key:101 value101
EXAMPLE 169: SET demo:key:102 value102
EXAMPLE 170: SET demo:key:103 value103
EXAMPLE 171: SET demo:key:104 value104
EXAMPLE 172: SET demo:key:105 value105
EXAMPLE 173: SET demo:key:106 value106
EXAMPLE 174: SET demo:key:107 value107
EXAMPLE 175: SET demo:key:108 value108
EXAMPLE 176: SET demo:key:109 value109
EXAMPLE 177: SET demo:key:110 value110
EXAMPLE 178: SET demo:key:111 value111
EXAMPLE 179: SET demo:key:112 value112
EXAMPLE 180: SET demo:key:113 value113
EXAMPLE 181: SET demo:key:114 value114
EXAMPLE 182: SET demo:key:115 value115
EXAMPLE 183: SET demo:key:116 value116
EXAMPLE 184: SET demo:key:117 value117
EXAMPLE 185: SET demo:key:118 value118
EXAMPLE 186: SET demo:key:119 value119
EXAMPLE 187: SET demo:key:120 value120
EXAMPLE 188: SET demo:key:121 value121
EXAMPLE 189: SET demo:key:122 value122
EXAMPLE 190: SET demo:key:123 value123
EXAMPLE 191: SET demo:key:124 value124
EXAMPLE 192: SET demo:key:125 value125
EXAMPLE 193: SET demo:key:126 value126
EXAMPLE 194: SET demo:key:127 value127
EXAMPLE 195: SET demo:key:128 value128
EXAMPLE 196: SET demo:key:129 value129
EXAMPLE 197: SET demo:key:130 value130
EXAMPLE 198: SET demo:key:131 value131
EXAMPLE 199: SET demo:key:132 value132
EXAMPLE 200: SET demo:key:133 value133
EXAMPLE 201: SET demo:key:134 value134
EXAMPLE 202: SET demo:key:135 value135
EXAMPLE 203: SET demo:key:136 value136
EXAMPLE 204: SET demo:key:137 value137
EXAMPLE 205: SET demo:key:138 value138
EXAMPLE 206: SET demo:key:139 value139
EXAMPLE 207: SET demo:key:140 value140
EXAMPLE 208: SET demo:key:141 value141
EXAMPLE 209: SET demo:key:142 value142
EXAMPLE 210: SET demo:key:143 value143
EXAMPLE 211: SET demo:key:144 value144
EXAMPLE 212: SET demo:key:145 value145
EXAMPLE 213: SET demo:key:146 value146
EXAMPLE 214: SET demo:key:147 value147
EXAMPLE 215: SET demo:key:148 value148
EXAMPLE 216: SET demo:key:149 value149
EXAMPLE 217: SET demo:key:150 value150
EXAMPLE 218: SET demo:key:151 value151
EXAMPLE 219: SET demo:key:152 value152
EXAMPLE 220: SET demo:key:153 value153
EXAMPLE 221: SET demo:key:154 value154
EXAMPLE 222: SET demo:key:155 value155
EXAMPLE 223: SET demo:key:156 value156
EXAMPLE 224: SET demo:key:157 value157
EXAMPLE 225: SET demo:key:158 value158
EXAMPLE 226: SET demo:key:159 value159
EXAMPLE 227: SET demo:key:160 value160
EXAMPLE 228: SET demo:key:161 value161
EXAMPLE 229: SET demo:key:162 value162
EXAMPLE 230: SET demo:key:163 value163
EXAMPLE 231: SET demo:key:164 value164
EXAMPLE 232: SET demo:key:165 value165
EXAMPLE 233: SET demo:key:166 value166
EXAMPLE 234: SET demo:key:167 value167
EXAMPLE 235: SET demo:key:168 value168
EXAMPLE 236: SET demo:key:169 value169
EXAMPLE 237: SET demo:key:170 value170
EXAMPLE 238: SET demo:key:171 value171
EXAMPLE 239: SET demo:key:172 value172
EXAMPLE 240: SET demo:key:173 value173
EXAMPLE 241: SET demo:key:174 value174
EXAMPLE 242: SET demo:key:175 value175
EXAMPLE 243: SET demo:key:176 value176
EXAMPLE 244: SET demo:key:177 value177
EXAMPLE 245: SET demo:key:178 value178
EXAMPLE 246: SET demo:key:179 value179
EXAMPLE 247: SET demo:key:180 value180
EXAMPLE 248: SET demo:key:181 value181
EXAMPLE 249: SET demo:key:182 value182
EXAMPLE 250: SET demo:key:183 value183
EXAMPLE 251: SET demo:key:184 value184
EXAMPLE 252: SET demo:key:185 value185
EXAMPLE 253: SET demo:key:186 value186
EXAMPLE 254: SET demo:key:187 value187
EXAMPLE 255: SET demo:key:188 value188
EXAMPLE 256: SET demo:key:189 value189
EXAMPLE 257: SET demo:key:190 value190
EXAMPLE 258: SET demo:key:191 value191
EXAMPLE 259: SET demo:key:192 value192
EXAMPLE 260: SET demo:key:193 value193
EXAMPLE 261: SET demo:key:194 value194
EXAMPLE 262: SET demo:key:195 value195
EXAMPLE 263: SET demo:key:196 value196
EXAMPLE 264: SET demo:key:197 value197
EXAMPLE 265: SET demo:key:198 value198
EXAMPLE 266: SET demo:key:199 value199
EXAMPLE 267: SET demo:key:200 value200
EXAMPLE 268: SET demo:key:201 value201
EXAMPLE 269: SET demo:key:202 value202
EXAMPLE 270: SET demo:key:203 value203
EXAMPLE 271: SET demo:key:204 value204
EXAMPLE 272: SET demo:key:205 value205
EXAMPLE 273: SET demo:key:206 value206
EXAMPLE 274: SET demo:key:207 value207
EXAMPLE 275: SET demo:key:208 value208
EXAMPLE 276: SET demo:key:209 value209
EXAMPLE 277: SET demo:key:210 value210
EXAMPLE 278: SET demo:key:211 value211
EXAMPLE 279: SET demo:key:212 value212
EXAMPLE 280: SET demo:key:213 value213
EXAMPLE 281: SET demo:key:214 value214
EXAMPLE 282: SET demo:key:215 value215
EXAMPLE 283: SET demo:key:216 value216
EXAMPLE 284: SET demo:key:217 value217
EXAMPLE 285: SET demo:key:218 value218
EXAMPLE 286: SET demo:key:219 value219
EXAMPLE 287: SET demo:key:220 value220
EXAMPLE 288: SET demo:key:221 value221
EXAMPLE 289: SET demo:key:222 value222
EXAMPLE 290: SET demo:key:223 value223
EXAMPLE 291: SET demo:key:224 value224
EXAMPLE 292: SET demo:key:225 value225
EXAMPLE 293: SET demo:key:226 value226
EXAMPLE 294: SET demo:key:227 value227
EXAMPLE 295: SET demo:key:228 value228
EXAMPLE 296: SET demo:key:229 value229
EXAMPLE 297: SET demo:key:230 value230
EXAMPLE 298: SET demo:key:231 value231
EXAMPLE 299: SET demo:key:232 value232
EXAMPLE 300: SET demo:key:233 value233
+)HELP";

const std::vector<std::string> kDemoScript = {
    "SET demo:kv:1 value1",
    "SET demo:kv:2 value2",
    "SET demo:kv:3 value3",
    "SET demo:kv:4 value4",
    "SET demo:kv:5 value5",
    "SET demo:kv:6 value6",
    "SET demo:kv:7 value7",
    "SET demo:kv:8 value8",
    "SET demo:kv:9 value9",
    "SET demo:kv:10 value10",
    "SET demo:kv:11 value11",
    "SET demo:kv:12 value12",
    "SET demo:kv:13 value13",
    "SET demo:kv:14 value14",
    "SET demo:kv:15 value15",
    "SET demo:kv:16 value16",
    "SET demo:kv:17 value17",
    "SET demo:kv:18 value18",
    "SET demo:kv:19 value19",
    "SET demo:kv:20 value20",
    "SET demo:kv:21 value21",
    "SET demo:kv:22 value22",
    "SET demo:kv:23 value23",
    "SET demo:kv:24 value24",
    "SET demo:kv:25 value25",
    "SET demo:kv:26 value26",
    "SET demo:kv:27 value27",
    "SET demo:kv:28 value28",
    "SET demo:kv:29 value29",
    "SET demo:kv:30 value30",
    "SET demo:kv:31 value31",
    "SET demo:kv:32 value32",
    "SET demo:kv:33 value33",
    "SET demo:kv:34 value34",
    "SET demo:kv:35 value35",
    "SET demo:kv:36 value36",
    "SET demo:kv:37 value37",
    "SET demo:kv:38 value38",
    "SET demo:kv:39 value39",
    "SET demo:kv:40 value40",
    "SET demo:kv:41 value41",
    "SET demo:kv:42 value42",
    "SET demo:kv:43 value43",
    "SET demo:kv:44 value44",
    "SET demo:kv:45 value45",
    "SET demo:kv:46 value46",
    "SET demo:kv:47 value47",
    "SET demo:kv:48 value48",
    "SET demo:kv:49 value49",
    "SET demo:kv:50 value50",
    "SET demo:kv:51 value51",
    "SET demo:kv:52 value52",
    "SET demo:kv:53 value53",
    "SET demo:kv:54 value54",
    "SET demo:kv:55 value55",
    "SET demo:kv:56 value56",
    "SET demo:kv:57 value57",
    "SET demo:kv:58 value58",
    "SET demo:kv:59 value59",
    "SET demo:kv:60 value60",
    "SET demo:kv:61 value61",
    "SET demo:kv:62 value62",
    "SET demo:kv:63 value63",
    "SET demo:kv:64 value64",
    "SET demo:kv:65 value65",
    "SET demo:kv:66 value66",
    "SET demo:kv:67 value67",
    "SET demo:kv:68 value68",
    "SET demo:kv:69 value69",
    "SET demo:kv:70 value70",
    "SET demo:kv:71 value71",
    "SET demo:kv:72 value72",
    "SET demo:kv:73 value73",
    "SET demo:kv:74 value74",
    "SET demo:kv:75 value75",
    "SET demo:kv:76 value76",
    "SET demo:kv:77 value77",
    "SET demo:kv:78 value78",
    "SET demo:kv:79 value79",
    "SET demo:kv:80 value80",
    "SET demo:kv:81 value81",
    "SET demo:kv:82 value82",
    "SET demo:kv:83 value83",
    "SET demo:kv:84 value84",
    "SET demo:kv:85 value85",
    "SET demo:kv:86 value86",
    "SET demo:kv:87 value87",
    "SET demo:kv:88 value88",
    "SET demo:kv:89 value89",
    "SET demo:kv:90 value90",
    "SET demo:kv:91 value91",
    "SET demo:kv:92 value92",
    "SET demo:kv:93 value93",
    "SET demo:kv:94 value94",
    "SET demo:kv:95 value95",
    "SET demo:kv:96 value96",
    "SET demo:kv:97 value97",
    "SET demo:kv:98 value98",
    "SET demo:kv:99 value99",
    "SET demo:kv:100 value100",
    "LPUSH demo:list a b c d e",
    "RPUSH demo:list f g h i j",
    "LLEN demo:list",
    "LRANGE demo:list 0 -1",
    "LTRIM demo:list 0 5",
    "LSET demo:list 0 z",
    "SADD demo:set a b c d e",
    "SADD demo:set f g h i j",
    "SREM demo:set j",
    "SISMEMBER demo:set a",
    "SMOVE demo:set demo:set2 a",
    "SUNION demo:set demo:set2",
    "HSET demo:hash f1 v1 f2 v2 f3 v3 f4 v4",
    "HDEL demo:hash f4",
    "HEXISTS demo:hash f1",
    "HGETALL demo:hash",
    "ZADD demo:z 1 a 2 b 3 c 4 d",
    "ZRANGE demo:z 0 -1 WITHSCORES",
    "ZREM demo:z d",
    "GEOADD demo:geo 13.361389 38.115556 a 15.087269 37.502669 b",
    "GEODIST demo:geo a b km",
    "GEORADIUS demo:geo 15 37 200 km WITHDIST COUNT 3",
    "XADD demo:stream * f1 v1 f2 v2",
    "XRANGE demo:stream - +",
    "SETBIT demo:bits 7 1",
    "GETBIT demo:bits 7",
    "BITCOUNT demo:bits",
    "BITOP NOT demo:bits2 demo:bits",
    "EXPIRE demo:kv:1 60",
    "TTL demo:kv:1",
    "SCAN 0 MATCH demo:* COUNT 10",
    "SET demo:bulk:101 value101",
    "SET demo:bulk:102 value102",
    "SET demo:bulk:103 value103",
    "SET demo:bulk:104 value104",
    "SET demo:bulk:105 value105",
    "SET demo:bulk:106 value106",
    "SET demo:bulk:107 value107",
    "SET demo:bulk:108 value108",
    "SET demo:bulk:109 value109",
    "SET demo:bulk:110 value110",
    "SET demo:bulk:111 value111",
    "SET demo:bulk:112 value112",
    "SET demo:bulk:113 value113",
    "SET demo:bulk:114 value114",
    "SET demo:bulk:115 value115",
    "SET demo:bulk:116 value116",
    "SET demo:bulk:117 value117",
    "SET demo:bulk:118 value118",
    "SET demo:bulk:119 value119",
    "SET demo:bulk:120 value120",
    "SET demo:bulk:121 value121",
    "SET demo:bulk:122 value122",
    "SET demo:bulk:123 value123",
    "SET demo:bulk:124 value124",
    "SET demo:bulk:125 value125",
    "SET demo:bulk:126 value126",
    "SET demo:bulk:127 value127",
    "SET demo:bulk:128 value128",
    "SET demo:bulk:129 value129",
    "SET demo:bulk:130 value130",
    "SET demo:bulk:131 value131",
    "SET demo:bulk:132 value132",
    "SET demo:bulk:133 value133",
    "SET demo:bulk:134 value134",
    "SET demo:bulk:135 value135",
    "SET demo:bulk:136 value136",
    "SET demo:bulk:137 value137",
    "SET demo:bulk:138 value138",
    "SET demo:bulk:139 value139",
    "SET demo:bulk:140 value140",
    "SET demo:bulk:141 value141",
    "SET demo:bulk:142 value142",
    "SET demo:bulk:143 value143",
    "SET demo:bulk:144 value144",
    "SET demo:bulk:145 value145",
    "SET demo:bulk:146 value146",
    "SET demo:bulk:147 value147",
    "SET demo:bulk:148 value148",
    "SET demo:bulk:149 value149",
    "SET demo:bulk:150 value150",
    "SET demo:bulk:151 value151",
    "SET demo:bulk:152 value152",
    "SET demo:bulk:153 value153",
    "SET demo:bulk:154 value154",
    "SET demo:bulk:155 value155",
    "SET demo:bulk:156 value156",
    "SET demo:bulk:157 value157",
    "SET demo:bulk:158 value158",
    "SET demo:bulk:159 value159",
    "SET demo:bulk:160 value160",
    "SET demo:bulk:161 value161",
    "SET demo:bulk:162 value162",
    "SET demo:bulk:163 value163",
    "SET demo:bulk:164 value164",
    "SET demo:bulk:165 value165",
    "SET demo:bulk:166 value166",
    "SET demo:bulk:167 value167",
    "SET demo:bulk:168 value168",
    "SET demo:bulk:169 value169",
    "SET demo:bulk:170 value170",
    "SET demo:bulk:171 value171",
    "SET demo:bulk:172 value172",
    "SET demo:bulk:173 value173",
    "SET demo:bulk:174 value174",
    "SET demo:bulk:175 value175",
    "SET demo:bulk:176 value176",
    "SET demo:bulk:177 value177",
    "SET demo:bulk:178 value178",
    "SET demo:bulk:179 value179",
    "SET demo:bulk:180 value180",
    "SET demo:bulk:181 value181",
    "SET demo:bulk:182 value182",
    "SET demo:bulk:183 value183",
    "SET demo:bulk:184 value184",
    "SET demo:bulk:185 value185",
    "SET demo:bulk:186 value186",
    "SET demo:bulk:187 value187",
    "SET demo:bulk:188 value188",
    "SET demo:bulk:189 value189",
    "SET demo:bulk:190 value190",
    "SET demo:bulk:191 value191",
    "SET demo:bulk:192 value192",
    "SET demo:bulk:193 value193",
    "SET demo:bulk:194 value194",
    "SET demo:bulk:195 value195",
    "SET demo:bulk:196 value196",
    "SET demo:bulk:197 value197",
    "SET demo:bulk:198 value198",
    "SET demo:bulk:199 value199",
    "SET demo:bulk:200 value200",
    "SET demo:bulk:201 value201",
    "SET demo:bulk:202 value202",
    "SET demo:bulk:203 value203",
    "SET demo:bulk:204 value204",
    "SET demo:bulk:205 value205",
    "SET demo:bulk:206 value206",
    "SET demo:bulk:207 value207",
    "SET demo:bulk:208 value208",
    "SET demo:bulk:209 value209",
    "SET demo:bulk:210 value210",
    "SET demo:bulk:211 value211",
    "SET demo:bulk:212 value212",
    "SET demo:bulk:213 value213",
    "SET demo:bulk:214 value214",
    "SET demo:bulk:215 value215",
    "SET demo:bulk:216 value216",
    "SET demo:bulk:217 value217",
    "SET demo:bulk:218 value218",
    "SET demo:bulk:219 value219",
    "SET demo:bulk:220 value220",
    "SET demo:bulk:221 value221",
    "SET demo:bulk:222 value222",
    "SET demo:bulk:223 value223",
    "SET demo:bulk:224 value224",
    "SET demo:bulk:225 value225",
    "SET demo:bulk:226 value226",
    "SET demo:bulk:227 value227",
    "SET demo:bulk:228 value228",
    "SET demo:bulk:229 value229",
    "SET demo:bulk:230 value230",
    "SET demo:bulk:231 value231",
    "SET demo:bulk:232 value232",
    "SET demo:bulk:233 value233",
    "SET demo:bulk:234 value234",
    "SET demo:bulk:235 value235",
    "SET demo:bulk:236 value236",
    "SET demo:bulk:237 value237",
    "SET demo:bulk:238 value238",
    "SET demo:bulk:239 value239",
    "SET demo:bulk:240 value240",
    "SET demo:bulk:241 value241",
    "SET demo:bulk:242 value242",
    "SET demo:bulk:243 value243",
    "SET demo:bulk:244 value244",
    "SET demo:bulk:245 value245",
    "SET demo:bulk:246 value246",
    "SET demo:bulk:247 value247",
    "SET demo:bulk:248 value248",
    "SET demo:bulk:249 value249",
    "SET demo:bulk:250 value250",
    "SET demo:bulk:251 value251",
    "SET demo:bulk:252 value252",
    "SET demo:bulk:253 value253",
    "SET demo:bulk:254 value254",
    "SET demo:bulk:255 value255",
    "SET demo:bulk:256 value256",
    "SET demo:bulk:257 value257",
    "SET demo:bulk:258 value258",
    "SET demo:bulk:259 value259",
    "SET demo:bulk:260 value260",
    "SET demo:bulk:261 value261",
    "SET demo:bulk:262 value262",
    "SET demo:bulk:263 value263",
    "SET demo:bulk:264 value264",
    "SET demo:bulk:265 value265",
    "SET demo:bulk:266 value266",
    "SET demo:bulk:267 value267",
    "SET demo:bulk:268 value268",
    "SET demo:bulk:269 value269",
    "SET demo:bulk:270 value270",
    "SET demo:bulk:271 value271",
    "SET demo:bulk:272 value272",
    "SET demo:bulk:273 value273",
    "SET demo:bulk:274 value274",
    "SET demo:bulk:275 value275",
    "SET demo:bulk:276 value276",
    "SET demo:bulk:277 value277",
    "SET demo:bulk:278 value278",
    "SET demo:bulk:279 value279",
    "SET demo:bulk:280 value280",
    "SET demo:bulk:281 value281",
    "SET demo:bulk:282 value282",
    "SET demo:bulk:283 value283",
    "SET demo:bulk:284 value284",
    "SET demo:bulk:285 value285",
    "SET demo:bulk:286 value286",
    "SET demo:bulk:287 value287",
    "SET demo:bulk:288 value288",
    "SET demo:bulk:289 value289",
    "SET demo:bulk:290 value290",
    "SET demo:bulk:291 value291",
    "SET demo:bulk:292 value292",
    "SET demo:bulk:293 value293",
    "SET demo:bulk:294 value294",
    "SET demo:bulk:295 value295",
    "SET demo:bulk:296 value296",
    "SET demo:bulk:297 value297",
    "SET demo:bulk:298 value298",
    "SET demo:bulk:299 value299",
    "SET demo:bulk:300 value300",
    "SET demo:bulk:301 value301",
    "SET demo:bulk:302 value302",
    "SET demo:bulk:303 value303",
    "SET demo:bulk:304 value304",
    "SET demo:bulk:305 value305",
    "SET demo:bulk:306 value306",
    "SET demo:bulk:307 value307",
    "SET demo:bulk:308 value308",
    "SET demo:bulk:309 value309",
    "SET demo:bulk:310 value310",
    "SET demo:bulk:311 value311",
    "SET demo:bulk:312 value312",
    "SET demo:bulk:313 value313",
    "SET demo:bulk:314 value314",
    "SET demo:bulk:315 value315",
    "SET demo:bulk:316 value316",
    "SET demo:bulk:317 value317",
    "SET demo:bulk:318 value318",
    "SET demo:bulk:319 value319",
    "SET demo:bulk:320 value320",
    "SET demo:bulk:321 value321",
    "SET demo:bulk:322 value322",
    "SET demo:bulk:323 value323",
    "SET demo:bulk:324 value324",
    "SET demo:bulk:325 value325",
    "SET demo:bulk:326 value326",
    "SET demo:bulk:327 value327",
    "SET demo:bulk:328 value328",
    "SET demo:bulk:329 value329",
    "SET demo:bulk:330 value330",
    "SET demo:bulk:331 value331",
    "SET demo:bulk:332 value332",
    "SET demo:bulk:333 value333",
    "SET demo:bulk:334 value334",
    "SET demo:bulk:335 value335",
    "SET demo:bulk:336 value336",
    "SET demo:bulk:337 value337",
    "SET demo:bulk:338 value338",
    "SET demo:bulk:339 value339",
    "SET demo:bulk:340 value340",
    "SET demo:bulk:341 value341",
    "SET demo:bulk:342 value342",
    "SET demo:bulk:343 value343",
    "SET demo:bulk:344 value344",
    "SET demo:bulk:345 value345",
    "SET demo:bulk:346 value346",
    "SET demo:bulk:347 value347",
    "SET demo:bulk:348 value348",
    "SET demo:bulk:349 value349",
    "SET demo:bulk:350 value350",
    "SET demo:bulk:351 value351",
    "SET demo:bulk:352 value352",
    "SET demo:bulk:353 value353",
    "SET demo:bulk:354 value354",
    "SET demo:bulk:355 value355",
    "SET demo:bulk:356 value356",
    "SET demo:bulk:357 value357",
    "SET demo:bulk:358 value358",
    "SET demo:bulk:359 value359",
    "SET demo:bulk:360 value360",
    "SET demo:bulk:361 value361",
    "SET demo:bulk:362 value362",
    "SET demo:bulk:363 value363",
    "SET demo:bulk:364 value364",
    "SET demo:bulk:365 value365",
    "SET demo:bulk:366 value366",
    "SET demo:bulk:367 value367",
    "SET demo:bulk:368 value368",
    "SET demo:bulk:369 value369",
    "SET demo:bulk:370 value370",
    "SET demo:bulk:371 value371",
    "SET demo:bulk:372 value372",
    "SET demo:bulk:373 value373",
    "SET demo:bulk:374 value374",
    "SET demo:bulk:375 value375",
    "SET demo:bulk:376 value376",
    "SET demo:bulk:377 value377",
    "SET demo:bulk:378 value378",
    "SET demo:bulk:379 value379",
    "SET demo:bulk:380 value380",
    "SET demo:bulk:381 value381",
    "SET demo:bulk:382 value382",
    "SET demo:bulk:383 value383",
    "SET demo:bulk:384 value384",
    "SET demo:bulk:385 value385",
    "SET demo:bulk:386 value386",
    "SET demo:bulk:387 value387",
    "SET demo:bulk:388 value388",
    "SET demo:bulk:389 value389",
    "SET demo:bulk:390 value390",
    "SET demo:bulk:391 value391",
    "SET demo:bulk:392 value392",
    "SET demo:bulk:393 value393",
    "SET demo:bulk:394 value394",
    "SET demo:bulk:395 value395",
    "SET demo:bulk:396 value396",
    "SET demo:bulk:397 value397",
    "SET demo:bulk:398 value398",
    "SET demo:bulk:399 value399",
    "SET demo:bulk:400 value400",
    "LPUSH demo:list2 v1",
    "LPUSH demo:list2 v2",
    "LPUSH demo:list2 v3",
    "LPUSH demo:list2 v4",
    "LPUSH demo:list2 v5",
    "LPUSH demo:list2 v6",
    "LPUSH demo:list2 v7",
    "LPUSH demo:list2 v8",
    "LPUSH demo:list2 v9",
    "LPUSH demo:list2 v10",
    "LPUSH demo:list2 v11",
    "LPUSH demo:list2 v12",
    "LPUSH demo:list2 v13",
    "LPUSH demo:list2 v14",
    "LPUSH demo:list2 v15",
    "LPUSH demo:list2 v16",
    "LPUSH demo:list2 v17",
    "LPUSH demo:list2 v18",
    "LPUSH demo:list2 v19",
    "LPUSH demo:list2 v20",
    "LPUSH demo:list2 v21",
    "LPUSH demo:list2 v22",
    "LPUSH demo:list2 v23",
    "LPUSH demo:list2 v24",
    "LPUSH demo:list2 v25",
    "LPUSH demo:list2 v26",
    "LPUSH demo:list2 v27",
    "LPUSH demo:list2 v28",
    "LPUSH demo:list2 v29",
    "LPUSH demo:list2 v30",
    "LPUSH demo:list2 v31",
    "LPUSH demo:list2 v32",
    "LPUSH demo:list2 v33",
    "LPUSH demo:list2 v34",
    "LPUSH demo:list2 v35",
    "LPUSH demo:list2 v36",
    "LPUSH demo:list2 v37",
    "LPUSH demo:list2 v38",
    "LPUSH demo:list2 v39",
    "LPUSH demo:list2 v40",
    "LPUSH demo:list2 v41",
    "LPUSH demo:list2 v42",
    "LPUSH demo:list2 v43",
    "LPUSH demo:list2 v44",
    "LPUSH demo:list2 v45",
    "LPUSH demo:list2 v46",
    "LPUSH demo:list2 v47",
    "LPUSH demo:list2 v48",
    "LPUSH demo:list2 v49",
    "LPUSH demo:list2 v50",
    "LPUSH demo:list2 v51",
    "LPUSH demo:list2 v52",
    "LPUSH demo:list2 v53",
    "LPUSH demo:list2 v54",
    "LPUSH demo:list2 v55",
    "LPUSH demo:list2 v56",
    "LPUSH demo:list2 v57",
    "LPUSH demo:list2 v58",
    "LPUSH demo:list2 v59",
    "LPUSH demo:list2 v60",
    "LPUSH demo:list2 v61",
    "LPUSH demo:list2 v62",
    "LPUSH demo:list2 v63",
    "LPUSH demo:list2 v64",
    "LPUSH demo:list2 v65",
    "LPUSH demo:list2 v66",
    "LPUSH demo:list2 v67",
    "LPUSH demo:list2 v68",
    "LPUSH demo:list2 v69",
    "LPUSH demo:list2 v70",
    "LPUSH demo:list2 v71",
    "LPUSH demo:list2 v72",
    "LPUSH demo:list2 v73",
    "LPUSH demo:list2 v74",
    "LPUSH demo:list2 v75",
    "LPUSH demo:list2 v76",
    "LPUSH demo:list2 v77",
    "LPUSH demo:list2 v78",
    "LPUSH demo:list2 v79",
    "LPUSH demo:list2 v80",
    "LPUSH demo:list2 v81",
    "LPUSH demo:list2 v82",
    "LPUSH demo:list2 v83",
    "LPUSH demo:list2 v84",
    "LPUSH demo:list2 v85",
    "LPUSH demo:list2 v86",
    "LPUSH demo:list2 v87",
    "LPUSH demo:list2 v88",
    "LPUSH demo:list2 v89",
    "LPUSH demo:list2 v90",
    "LPUSH demo:list2 v91",
    "LPUSH demo:list2 v92",
    "LPUSH demo:list2 v93",
    "LPUSH demo:list2 v94",
    "LPUSH demo:list2 v95",
    "LPUSH demo:list2 v96",
    "LPUSH demo:list2 v97",
    "LPUSH demo:list2 v98",
    "LPUSH demo:list2 v99",
    "LPUSH demo:list2 v100",
    "LPUSH demo:list2 v101",
    "LPUSH demo:list2 v102",
    "LPUSH demo:list2 v103",
    "LPUSH demo:list2 v104",
    "LPUSH demo:list2 v105",
    "LPUSH demo:list2 v106",
    "LPUSH demo:list2 v107",
    "LPUSH demo:list2 v108",
    "LPUSH demo:list2 v109",
    "LPUSH demo:list2 v110",
    "LPUSH demo:list2 v111",
    "LPUSH demo:list2 v112",
    "LPUSH demo:list2 v113",
    "LPUSH demo:list2 v114",
    "LPUSH demo:list2 v115",
    "LPUSH demo:list2 v116",
    "LPUSH demo:list2 v117",
    "LPUSH demo:list2 v118",
    "LPUSH demo:list2 v119",
    "LPUSH demo:list2 v120",
    "LPUSH demo:list2 v121",
    "LPUSH demo:list2 v122",
    "LPUSH demo:list2 v123",
    "LPUSH demo:list2 v124",
    "LPUSH demo:list2 v125",
    "LPUSH demo:list2 v126",
    "LPUSH demo:list2 v127",
    "LPUSH demo:list2 v128",
    "LPUSH demo:list2 v129",
    "LPUSH demo:list2 v130",
    "LPUSH demo:list2 v131",
    "LPUSH demo:list2 v132",
    "LPUSH demo:list2 v133",
    "LPUSH demo:list2 v134",
    "LPUSH demo:list2 v135",
    "LPUSH demo:list2 v136",
    "LPUSH demo:list2 v137",
    "LPUSH demo:list2 v138",
    "LPUSH demo:list2 v139",
    "LPUSH demo:list2 v140",
    "LPUSH demo:list2 v141",
    "LPUSH demo:list2 v142",
    "LPUSH demo:list2 v143",
    "LPUSH demo:list2 v144",
    "LPUSH demo:list2 v145",
    "LPUSH demo:list2 v146",
    "LPUSH demo:list2 v147",
    "LPUSH demo:list2 v148",
    "LPUSH demo:list2 v149",
    "LPUSH demo:list2 v150",
    "SADD demo:set2 s1",
    "SADD demo:set2 s2",
    "SADD demo:set2 s3",
    "SADD demo:set2 s4",
    "SADD demo:set2 s5",
    "SADD demo:set2 s6",
    "SADD demo:set2 s7",
    "SADD demo:set2 s8",
    "SADD demo:set2 s9",
    "SADD demo:set2 s10",
    "SADD demo:set2 s11",
    "SADD demo:set2 s12",
    "SADD demo:set2 s13",
    "SADD demo:set2 s14",
    "SADD demo:set2 s15",
    "SADD demo:set2 s16",
    "SADD demo:set2 s17",
    "SADD demo:set2 s18",
    "SADD demo:set2 s19",
    "SADD demo:set2 s20",
    "SADD demo:set2 s21",
    "SADD demo:set2 s22",
    "SADD demo:set2 s23",
    "SADD demo:set2 s24",
    "SADD demo:set2 s25",
    "SADD demo:set2 s26",
    "SADD demo:set2 s27",
    "SADD demo:set2 s28",
    "SADD demo:set2 s29",
    "SADD demo:set2 s30",
    "SADD demo:set2 s31",
    "SADD demo:set2 s32",
    "SADD demo:set2 s33",
    "SADD demo:set2 s34",
    "SADD demo:set2 s35",
    "SADD demo:set2 s36",
    "SADD demo:set2 s37",
    "SADD demo:set2 s38",
    "SADD demo:set2 s39",
    "SADD demo:set2 s40",
    "SADD demo:set2 s41",
    "SADD demo:set2 s42",
    "SADD demo:set2 s43",
    "SADD demo:set2 s44",
    "SADD demo:set2 s45",
    "SADD demo:set2 s46",
    "SADD demo:set2 s47",
    "SADD demo:set2 s48",
    "SADD demo:set2 s49",
    "SADD demo:set2 s50",
    "SADD demo:set2 s51",
    "SADD demo:set2 s52",
    "SADD demo:set2 s53",
    "SADD demo:set2 s54",
    "SADD demo:set2 s55",
    "SADD demo:set2 s56",
    "SADD demo:set2 s57",
    "SADD demo:set2 s58",
    "SADD demo:set2 s59",
    "SADD demo:set2 s60",
    "SADD demo:set2 s61",
    "SADD demo:set2 s62",
    "SADD demo:set2 s63",
    "SADD demo:set2 s64",
    "SADD demo:set2 s65",
    "SADD demo:set2 s66",
    "SADD demo:set2 s67",
    "SADD demo:set2 s68",
    "SADD demo:set2 s69",
    "SADD demo:set2 s70",
    "SADD demo:set2 s71",
    "SADD demo:set2 s72",
    "SADD demo:set2 s73",
    "SADD demo:set2 s74",
    "SADD demo:set2 s75",
    "SADD demo:set2 s76",
    "SADD demo:set2 s77",
    "SADD demo:set2 s78",
    "SADD demo:set2 s79",
    "SADD demo:set2 s80",
    "SADD demo:set2 s81",
    "SADD demo:set2 s82",
    "SADD demo:set2 s83",
    "SADD demo:set2 s84",
    "SADD demo:set2 s85",
    "SADD demo:set2 s86",
    "SADD demo:set2 s87",
    "SADD demo:set2 s88",
    "SADD demo:set2 s89",
    "SADD demo:set2 s90",
    "SADD demo:set2 s91",
    "SADD demo:set2 s92",
    "SADD demo:set2 s93",
    "SADD demo:set2 s94",
    "SADD demo:set2 s95",
    "SADD demo:set2 s96",
    "SADD demo:set2 s97",
    "SADD demo:set2 s98",
    "SADD demo:set2 s99",
    "SADD demo:set2 s100",
    "HSET demo:hash2 f1 v1",
    "HSET demo:hash2 f2 v2",
    "HSET demo:hash2 f3 v3",
    "HSET demo:hash2 f4 v4",
    "HSET demo:hash2 f5 v5",
    "HSET demo:hash2 f6 v6",
    "HSET demo:hash2 f7 v7",
    "HSET demo:hash2 f8 v8",
    "HSET demo:hash2 f9 v9",
    "HSET demo:hash2 f10 v10",
    "HSET demo:hash2 f11 v11",
    "HSET demo:hash2 f12 v12",
    "HSET demo:hash2 f13 v13",
    "HSET demo:hash2 f14 v14",
    "HSET demo:hash2 f15 v15",
    "HSET demo:hash2 f16 v16",
    "HSET demo:hash2 f17 v17",
    "HSET demo:hash2 f18 v18",
    "HSET demo:hash2 f19 v19",
    "HSET demo:hash2 f20 v20",
    "HSET demo:hash2 f21 v21",
    "HSET demo:hash2 f22 v22",
    "HSET demo:hash2 f23 v23",
    "HSET demo:hash2 f24 v24",
    "HSET demo:hash2 f25 v25",
    "HSET demo:hash2 f26 v26",
    "HSET demo:hash2 f27 v27",
    "HSET demo:hash2 f28 v28",
    "HSET demo:hash2 f29 v29",
    "HSET demo:hash2 f30 v30",
    "HSET demo:hash2 f31 v31",
    "HSET demo:hash2 f32 v32",
    "HSET demo:hash2 f33 v33",
    "HSET demo:hash2 f34 v34",
    "HSET demo:hash2 f35 v35",
    "HSET demo:hash2 f36 v36",
    "HSET demo:hash2 f37 v37",
    "HSET demo:hash2 f38 v38",
    "HSET demo:hash2 f39 v39",
    "HSET demo:hash2 f40 v40",
    "HSET demo:hash2 f41 v41",
    "HSET demo:hash2 f42 v42",
    "HSET demo:hash2 f43 v43",
    "HSET demo:hash2 f44 v44",
    "HSET demo:hash2 f45 v45",
    "HSET demo:hash2 f46 v46",
    "HSET demo:hash2 f47 v47",
    "HSET demo:hash2 f48 v48",
    "HSET demo:hash2 f49 v49",
    "HSET demo:hash2 f50 v50"
};

} // namespace redisclone

static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.rfind(prefix, 0) == 0;
}

struct TestCase {
    std::vector<std::string> cmd;
    std::string expect_prefix;
    std::string expect_exact;
    std::string name;
};

static int run_demo_script() {
    redisclone::RedisClone db;
    std::mutex mu;
    size_t ok = 0;
    for (const auto &line : redisclone::kDemoScript) {
        auto args = redisclone::split_args(line);
        if (args.empty()) {
            continue;
        }
        std::string resp;
        {
            std::lock_guard<std::mutex> lock(mu);
            resp = db.exec(args);
        }
        if (!resp.empty() && resp[0] != '-') {
            ++ok;
        } else {
            std::cerr << "DEMO FAIL: " << line << " -> " << resp << std::endl;
        }
    }
    std::cout << "DEMO OK (" << ok << "/" << redisclone::kDemoScript.size() << ")" << std::endl;
    return 0;
}

static int run_selftest() {
    redisclone::RedisClone db;
    std::mutex mu;
    std::vector<TestCase> tests;

    tests.push_back(TestCase{{"PING"}, "+PONG", "+PONG\r\n", "ping"});
    tests.push_back(TestCase{{"SET", "a", "1"}, "+OK", "+OK\r\n", "set a"});
    tests.push_back(TestCase{{"GET", "a"}, "$", "", "get a"});
    tests.push_back(TestCase{{"INCR", "a"}, ":", "", "incr a"});
    tests.push_back(TestCase{{"DECR", "a"}, ":", "", "decr a"});
    tests.push_back(TestCase{{"INCRBY", "a", "5"}, ":", "", "incrby a"});
    tests.push_back(TestCase{{"DECRBY", "a", "2"}, ":", "", "decrby a"});
    tests.push_back(TestCase{{"APPEND", "a", "xyz"}, ":", "", "append a"});
    tests.push_back(TestCase{{"STRLEN", "a"}, ":", "", "strlen a"});
    tests.push_back(TestCase{{"GETSET", "a", "2"}, "$", "", "getset"});
    tests.push_back(TestCase{{"SETNX", "b", "v"}, ":", "", "setnx"});
    tests.push_back(TestCase{{"SETNX", "b", "v2"}, ":", "", "setnx fail"});
    tests.push_back(TestCase{{"MSET", "k1", "v1", "k2", "v2"}, "+OK", "+OK\r\n", "mset"});
    tests.push_back(TestCase{{"MGET", "k1", "k2", "k3"}, "*", "", "mget"});
    tests.push_back(TestCase{{"DEL", "k1"}, ":", "", "del"});
    tests.push_back(TestCase{{"EXISTS", "k1", "k2"}, ":", "", "exists"});
    tests.push_back(TestCase{{"EXPIRE", "k2", "1"}, ":", "", "expire"});
    tests.push_back(TestCase{{"TTL", "k2"}, ":", "", "ttl"});
    tests.push_back(TestCase{{"PERSIST", "k2"}, ":", "", "persist"});
    tests.push_back(TestCase{{"RENAME", "k2", "k2b"}, "+OK", "+OK\r\n", "rename"});
    tests.push_back(TestCase{{"RENAMENX", "k2b", "k2c"}, ":", "", "renamenx"});
    tests.push_back(TestCase{{"TYPE", "k2c"}, "+", "", "type"});
    tests.push_back(TestCase{{"KEYS", "*"}, "*", "", "keys"});
    tests.push_back(TestCase{{"SCAN", "0"}, "*", "", "scan"});

    tests.push_back(TestCase{{"LPUSH", "list", "a", "b", "c"}, ":", "", "lpush"});
    tests.push_back(TestCase{{"RPUSH", "list", "d", "e"}, ":", "", "rpush"});
    tests.push_back(TestCase{{"LLEN", "list"}, ":", "", "llen"});
    tests.push_back(TestCase{{"LRANGE", "list", "0", "-1"}, "*", "", "lrange"});
    tests.push_back(TestCase{{"LINDEX", "list", "0"}, "$", "", "lindex"});
    tests.push_back(TestCase{{"LSET", "list", "0", "z"}, "+OK", "+OK\r\n", "lset"});
    tests.push_back(TestCase{{"LTRIM", "list", "0", "2"}, "+OK", "+OK\r\n", "ltrim"});
    tests.push_back(TestCase{{"LPOP", "list"}, "$", "", "lpop"});
    tests.push_back(TestCase{{"RPOP", "list"}, "$", "", "rpop"});

    tests.push_back(TestCase{{"SADD", "set", "a", "b", "c"}, ":", "", "sadd"});
    tests.push_back(TestCase{{"SREM", "set", "c"}, ":", "", "srem"});
    tests.push_back(TestCase{{"SISMEMBER", "set", "a"}, ":", "", "sismember"});
    tests.push_back(TestCase{{"SMOVE", "set", "set2", "a"}, ":", "", "smove"});
    tests.push_back(TestCase{{"SUNION", "set", "set2"}, "*", "", "sunion"});
    tests.push_back(TestCase{{"SINTER", "set", "set2"}, "*", "", "sinter"});
    tests.push_back(TestCase{{"SSCAN", "set", "0"}, "*", "", "sscan"});

    tests.push_back(TestCase{{"HSET", "hash", "f1", "v1", "f2", "v2"}, ":", "", "hset"});
    tests.push_back(TestCase{{"HMSET", "hash", "f3", "v3"}, "+OK", "+OK\r\n", "hmset"});
    tests.push_back(TestCase{{"HGET", "hash", "f1"}, "$", "", "hget"});
    tests.push_back(TestCase{{"HMGET", "hash", "f1", "f2", "fx"}, "*", "", "hmget"});
    tests.push_back(TestCase{{"HLEN", "hash"}, ":", "", "hlen"});
    tests.push_back(TestCase{{"HEXISTS", "hash", "f2"}, ":", "", "hexists"});
    tests.push_back(TestCase{{"HDEL", "hash", "f2"}, ":", "", "hdel"});
    tests.push_back(TestCase{{"HGETALL", "hash"}, "*", "", "hgetall"});
    tests.push_back(TestCase{{"HSCAN", "hash", "0"}, "*", "", "hscan"});

    tests.push_back(TestCase{{"ZADD", "z", "1", "a", "2", "b"}, ":", "", "zadd"});
    tests.push_back(TestCase{{"ZRANGE", "z", "0", "-1", "WITHSCORES"}, "*", "", "zrange"});
    tests.push_back(TestCase{{"ZSCORE", "z", "a"}, "$", "", "zscore"});
    tests.push_back(TestCase{{"ZREM", "z", "b"}, ":", "", "zrem"});
    tests.push_back(TestCase{{"ZCARD", "z"}, ":", "", "zcard"});
    tests.push_back(TestCase{{"ZSCAN", "z", "0"}, "*", "", "zscan"});

    tests.push_back(TestCase{{"GEOADD", "geo", "13.361389", "38.115556", "a", "15.087269", "37.502669", "b"}, ":", "", "geoadd"});
    tests.push_back(TestCase{{"GEOPOS", "geo", "a", "b", "c"}, "*", "", "geopos"});
    tests.push_back(TestCase{{"GEODIST", "geo", "a", "b", "km"}, "$", "", "geodist"});
    tests.push_back(TestCase{{"GEORADIUS", "geo", "15", "37", "200", "km", "WITHDIST", "COUNT", "1"}, "*", "", "georadius"});

    tests.push_back(TestCase{{"XADD", "stream", "*", "f1", "v1"}, "$", "", "xadd"});
    tests.push_back(TestCase{{"XLEN", "stream"}, ":", "", "xlen"});
    tests.push_back(TestCase{{"XRANGE", "stream", "-", "+"}, "*", "", "xrange"});
    tests.push_back(TestCase{{"XREAD", "COUNT", "1", "STREAMS", "stream", "0-0"}, "*", "", "xread"});

    tests.push_back(TestCase{{"SETBIT", "bits", "7", "1"}, ":", "", "setbit"});
    tests.push_back(TestCase{{"GETBIT", "bits", "7"}, ":", "", "getbit"});
    tests.push_back(TestCase{{"BITCOUNT", "bits"}, ":", "", "bitcount"});
    tests.push_back(TestCase{{"BITOP", "NOT", "bits2", "bits"}, ":", "", "bitop"});

    tests.push_back(TestCase{{"INFO"}, "$", "", "info"});
    tests.push_back(TestCase{{"SLOWLOG", "LEN"}, ":", "", "slowlog len"});
    tests.push_back(TestCase{{"SLOWLOG", "GET", "5"}, "*", "", "slowlog get"});
    tests.push_back(TestCase{{"SLOWLOG", "RESET"}, "+OK", "+OK\r\n", "slowlog reset"});

    for (int i = 0; i < 200; ++i) {
        tests.push_back(TestCase{{"SET", "bulk" + std::to_string(i), std::to_string(i)}, "+OK", "+OK\r\n", "bulk set"});
    }
    for (int i = 0; i < 200; ++i) {
        tests.push_back(TestCase{{"GET", "bulk" + std::to_string(i)}, "$", "", "bulk get"});
    }
    for (int i = 0; i < 100; ++i) {
        tests.push_back(TestCase{{"LPUSH", "lbulk", std::to_string(i)}, ":", "", "bulk lpush"});
    }
    for (int i = 0; i < 50; ++i) {
        tests.push_back(TestCase{{"SADD", "sbulk", "v" + std::to_string(i)}, ":", "", "bulk sadd"});
    }
    for (int i = 0; i < 50; ++i) {
        tests.push_back(TestCase{{"HSET", "hbulk", "f" + std::to_string(i), "v" + std::to_string(i)}, ":", "", "bulk hset"});
    }

    int failed = 0;
    for (const auto &tc : tests) {
        std::string resp;
        {
            std::lock_guard<std::mutex> lock(mu);
            resp = db.exec(tc.cmd);
        }
        bool ok = true;
        if (!tc.expect_exact.empty()) {
            ok = (resp == tc.expect_exact);
        } else if (!tc.expect_prefix.empty()) {
            ok = starts_with(resp, tc.expect_prefix);
        }
        if (!ok) {
            ++failed;
            std::cerr << "FAIL: " << tc.name << " -> " << resp << std::endl;
        }
    }
    if (failed == 0) {
        std::cout << "SELFTEST OK (" << tests.size() << " cases)" << std::endl;
        return 0;
    }
    std::cerr << "SELFTEST FAILED: " << failed << " cases" << std::endl;
    return 1;
}

int main(int argc, char **argv) {
    bool enable_tcp = true;
    bool enable_cli = true;
    bool enable_notify = true;
    int port = 6379;
    std::string persist_path;
    std::string aof_path;
    bool load_on_start = false;
    bool aof_load_on_start = false;
    bool run_tests = false;
    bool run_demo = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--tcp") {
            enable_tcp = true;
        } else if (arg == "--no-tcp") {
            enable_tcp = false;
        } else if (arg == "--cli") {
            enable_cli = true;
        } else if (arg == "--no-cli") {
            enable_cli = false;
        } else if (arg == "--save" && i + 1 < argc) {
            persist_path = argv[++i];
        } else if (arg == "--load") {
            load_on_start = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                persist_path = argv[++i];
            }
        } else if (arg == "--aof" && i + 1 < argc) {
            aof_path = argv[++i];
        } else if (arg == "--aof-load") {
            aof_load_on_start = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                aof_path = argv[++i];
            }
        } else if (arg == "--no-notify") {
            enable_notify = false;
        } else if (arg == "--selftest") {
            run_tests = true;
        } else if (arg == "--demo") {
            run_demo = true;
        } else if (arg == "--help") {
            std::cout << "Usage: redis_clone [--port N] [--save file] [--load [file]] [--aof file] [--aof-load [file]] [--no-tcp] [--no-cli] [--no-notify] [--selftest] [--demo]" << std::endl;
            return 0;
        }
    }

    if (run_tests) {
        return run_selftest();
    }
    if (run_demo) {
        return run_demo_script();
    }

    if (!enable_tcp && !enable_cli) {
        enable_cli = true;
    }

    redisclone::RedisClone db;
    if (!persist_path.empty()) {
        db.set_persistence_file(persist_path);
    }
    if (!aof_path.empty()) {
        db.enable_aof(aof_path);
    }
    if (load_on_start && !persist_path.empty()) {
        std::string err;
        if (!db.load_from_file(persist_path, err)) {
            std::cerr << "Load failed: " << err << std::endl;
        }
    }
    if (aof_load_on_start && !aof_path.empty()) {
        std::string err;
        if (!db.load_aof(aof_path, err)) {
            std::cerr << "AOF load failed: " << err << std::endl;
        }
    }

    std::mutex db_mu;
    std::atomic<bool> running(true);

    if (enable_tcp && enable_cli) {
        std::thread server_thread([&]() {
            redisclone::run_server(db, db_mu, port, running, enable_notify);
        });
        redisclone::run_cli(db, db_mu, running);
        server_thread.join();
    } else if (enable_tcp) {
        redisclone::run_server(db, db_mu, port, running, enable_notify);
    } else {
        redisclone::run_cli(db, db_mu, running);
    }

    if (!persist_path.empty()) {
        std::string err;
        if (!db.save_to_file(persist_path, err)) {
            std::cerr << "Save failed: " << err << std::endl;
        }
    }
    return 0;
}
