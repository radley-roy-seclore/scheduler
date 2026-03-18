/*******************************************************************************
 *  Command-Line Task Manager with Network Connectivity & Explorer
 *  ---------------------------------------------------------------
 *  A full-fledged CLI task runner implemented in C++17.
 
 *
 *  Build (macOS / Linux)
 *  ---------------------
 *      g++ -std=c++17 -O2 -pthread -o taskrunner cmdlinetaskrunner.cpp
 *
 *  Run
 *  ---
 *      ./taskrunner                     # interactive mode
 *      ./taskrunner add --title "X"     # single-shot mode
 *      ./taskrunner server --tcp 8080   # start TCP task server
 *
 ******************************************************************************/

// ─── Standard headers ───────────────────────────────────────────────────────
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>
#include <iomanip>
#include <regex>

// ─── POSIX networking / process headers ─────────────────────────────────────
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 1 – Utility helpers
// ═══════════════════════════════════════════════════════════════════════════════

namespace util {

// Trim whitespace from both ends
inline std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\n\r") - b + 1);
}

// Case-insensitive compare
inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Current ISO timestamp
inline std::string now_iso() {
    auto t = std::time(nullptr);
    std::ostringstream os;
    os << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

// Simple ID generator (monotonic per session, max stored in file)
inline int next_id(int& counter) { return ++counter; }

// Escape a string for JSON output
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

// Unescape JSON string
inline std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  out += '"';  ++i; break;
            case '\\': out += '\\'; ++i; break;
            case 'n':  out += '\n'; ++i; break;
            case 'r':  out += '\r'; ++i; break;
            case 't':  out += '\t'; ++i; break;
            default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Print a horizontal rule
inline void hr(char c = '-', int width = 60) {
    std::cout << std::string(width, c) << "\n";
}

// Colorful console helpers (ANSI)
namespace color {
    const std::string RESET  = "\033[0m";
    const std::string RED    = "\033[31m";
    const std::string GREEN  = "\033[32m";
    const std::string YELLOW = "\033[33m";
    const std::string BLUE   = "\033[34m";
    const std::string CYAN   = "\033[36m";
    const std::string BOLD   = "\033[1m";
    const std::string DIM    = "\033[2m";
} // namespace color

} // namespace util

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 2 – Minimal JSON reader / writer
//  (Handles the subset needed: objects, arrays, strings, ints, bools)
// ═══════════════════════════════════════════════════════════════════════════════

namespace json {

enum class Type { Null, String, Int, Bool, Array, Object };

struct Value {
    Type type = Type::Null;
    std::string str;
    int64_t num = 0;
    bool boolean = false;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj; // ordered

    Value() = default;
    explicit Value(const std::string& s) : type(Type::String), str(s) {}
    explicit Value(const char* s) : type(Type::String), str(s) {}
    explicit Value(int64_t n) : type(Type::Int), num(n) {}
    explicit Value(int n) : type(Type::Int), num(n) {}
    explicit Value(bool b) : type(Type::Bool), boolean(b) {}

    static Value make_array()  { Value v; v.type = Type::Array;  return v; }
    static Value make_object() { Value v; v.type = Type::Object; return v; }

    // Object helpers
    Value& operator[](const std::string& key) {
        for (auto& [k, v] : obj) if (k == key) return v;
        obj.emplace_back(key, Value{});
        return obj.back().second;
    }
    const Value* get(const std::string& key) const {
        for (auto& [k, v] : obj) if (k == key) return &v;
        return nullptr;
    }
    bool has(const std::string& key) const { return get(key) != nullptr; }

    // Array helpers
    void push_back(const Value& v) { arr.push_back(v); }

    // Serialise
    std::string to_json(int indent = 0) const {
        std::string pad(indent, ' ');
        std::string pad2(indent + 2, ' ');
        switch (type) {
        case Type::Null:   return "null";
        case Type::String: return "\"" + util::json_escape(str) + "\"";
        case Type::Int:    return std::to_string(num);
        case Type::Bool:   return boolean ? "true" : "false";
        case Type::Array: {
            if (arr.empty()) return "[]";
            std::string s = "[\n";
            for (size_t i = 0; i < arr.size(); ++i) {
                s += pad2 + arr[i].to_json(indent + 2);
                if (i + 1 < arr.size()) s += ",";
                s += "\n";
            }
            s += pad + "]";
            return s;
        }
        case Type::Object: {
            if (obj.empty()) return "{}";
            std::string s = "{\n";
            for (size_t i = 0; i < obj.size(); ++i) {
                s += pad2 + "\"" + util::json_escape(obj[i].first) +
                     "\": " + obj[i].second.to_json(indent + 2);
                if (i + 1 < obj.size()) s += ",";
                s += "\n";
            }
            s += pad + "}";
            return s;
        }
        }
        return "null";
    }
};

// ─── Minimalistic recursive-descent parser ──────────────────────────────────

class Parser {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])))
            ++pos;
    }
    char peek() { skip_ws(); return pos < src.size() ? src[pos] : '\0'; }
    char advance() { return pos < src.size() ? src[pos++] : '\0'; }

    std::string parse_string_raw() {
        // Assumes opening " already consumed
        std::string s;
        while (pos < src.size()) {
            char c = advance();
            if (c == '"') break;
            if (c == '\\' && pos < src.size()) {
                char e = advance();
                switch (e) {
                case '"':  s += '"';  break;
                case '\\': s += '\\'; break;
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                case '/':  s += '/';  break;
                default:   s += '\\'; s += e; break;
                }
            } else {
                s += c;
            }
        }
        return s;
    }

public:
    explicit Parser(const std::string& s) : src(s) {}

    Value parse() {
        skip_ws();
        char c = peek();
        if (c == '"') { advance(); return Value(parse_string_raw()); }
        if (c == '{') {
            advance(); // {
            Value v = Value::make_object();
            if (peek() == '}') { advance(); return v; }
            while (true) {
                skip_ws(); advance(); // opening "
                std::string key = parse_string_raw();
                skip_ws(); advance(); // :
                v.obj.emplace_back(key, parse());
                skip_ws();
                if (peek() == ',') { advance(); continue; }
                if (peek() == '}') { advance(); break; }
                break;
            }
            return v;
        }
        if (c == '[') {
            advance();
            Value v = Value::make_array();
            if (peek() == ']') { advance(); return v; }
            while (true) {
                v.arr.push_back(parse());
                skip_ws();
                if (peek() == ',') { advance(); continue; }
                if (peek() == ']') { advance(); break; }
                break;
            }
            return v;
        }
        if (c == 't') { pos += 4; return Value(true);  }
        if (c == 'f') { pos += 5; return Value(false); }
        if (c == 'n') { pos += 4; return Value();      }
        // number (int only)
        {
            skip_ws();
            bool neg = false;
            if (src[pos] == '-') { neg = true; ++pos; }
            int64_t n = 0;
            while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                n = n * 10 + (src[pos++] - '0');
            // skip fractional part if present
            if (pos < src.size() && src[pos] == '.') {
                ++pos;
                while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                    ++pos;
            }
            return Value(neg ? -n : n);
        }
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

} // namespace json

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 3 – Task model
// ═══════════════════════════════════════════════════════════════════════════════

enum class Priority { Low, Medium, High };
enum class Status   { Pending, Completed };

inline std::string priority_str(Priority p) {
    switch (p) {
    case Priority::Low:    return "low";
    case Priority::Medium: return "medium";
    case Priority::High:   return "high";
    }
    return "medium";
}
inline Priority priority_from(const std::string& s) {
    if (util::iequals(s, "high"))   return Priority::High;
    if (util::iequals(s, "low"))    return Priority::Low;
    return Priority::Medium;
}
inline std::string status_str(Status s) {
    return s == Status::Completed ? "completed" : "pending";
}
inline Status status_from(const std::string& s) {
    if (util::iequals(s, "completed")) return Status::Completed;
    return Status::Pending;
}

struct Task {
    int         id          = 0;
    std::string title;
    std::string description;
    Priority    priority    = Priority::Medium;
    Status      status      = Status::Pending;
    std::string due_date;           // YYYY-MM-DD or empty
    std::string created_at;
    std::string updated_at;
    std::vector<std::string> files; // attached file paths

    json::Value to_json() const {
        auto v = json::Value::make_object();
        v["id"]          = json::Value(id);
        v["title"]       = json::Value(title);
        v["description"] = json::Value(description);
        v["priority"]    = json::Value(priority_str(priority));
        v["status"]      = json::Value(status_str(status));
        v["due_date"]    = json::Value(due_date);
        v["created_at"]  = json::Value(created_at);
        v["updated_at"]  = json::Value(updated_at);
        auto fa = json::Value::make_array();
        for (auto& f : files) fa.push_back(json::Value(f));
        v["files"] = fa;
        return v;
    }

    static Task from_json(const json::Value& v) {
        Task t;
        if (auto* p = v.get("id"))          t.id          = static_cast<int>(p->num);
        if (auto* p = v.get("title"))       t.title       = p->str;
        if (auto* p = v.get("description")) t.description = p->str;
        if (auto* p = v.get("priority"))    t.priority    = priority_from(p->str);
        if (auto* p = v.get("status"))      t.status      = status_from(p->str);
        if (auto* p = v.get("due_date"))    t.due_date    = p->str;
        if (auto* p = v.get("created_at"))  t.created_at  = p->str;
        if (auto* p = v.get("updated_at"))  t.updated_at  = p->str;
        if (auto* p = v.get("files"))
            for (auto& f : p->arr) t.files.push_back(f.str);
        return t;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 4 – Storage layer (JSON file)
// ═══════════════════════════════════════════════════════════════════════════════

class Storage {
    std::string filepath_;
    std::mutex  mtx_;

public:
    explicit Storage(const std::string& path = "tasks.json") : filepath_(path) {}

    std::vector<Task> load() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ifstream ifs(filepath_);
        if (!ifs.is_open()) return {};
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        if (content.empty()) return {};
        auto root = json::parse(content);
        const auto* tasks_val = root.get("tasks");
        if (!tasks_val) return {};
        std::vector<Task> tasks;
        for (auto& jt : tasks_val->arr) tasks.push_back(Task::from_json(jt));
        return tasks;
    }

    void save(const std::vector<Task>& tasks, int next_id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto root = json::Value::make_object();
        root["next_id"] = json::Value(next_id);
        auto arr = json::Value::make_array();
        for (auto& t : tasks) arr.push_back(t.to_json());
        root["tasks"] = arr;

        // Write to temp then rename for crash safety
        std::string tmp = filepath_ + ".tmp";
        {
            std::ofstream ofs(tmp);
            ofs << root.to_json() << "\n";
        }
        fs::rename(tmp, filepath_);
    }

    int load_next_id() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ifstream ifs(filepath_);
        if (!ifs.is_open()) return 0;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        if (content.empty()) return 0;
        auto root = json::parse(content);
        const auto* nid = root.get("next_id");
        return nid ? static_cast<int>(nid->num) : 0;
    }

    const std::string& path() const { return filepath_; }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 5 – Task Manager (business logic)
// ═══════════════════════════════════════════════════════════════════════════════

class TaskManager {
    std::vector<Task> tasks_;
    Storage           storage_;
    int               id_counter_ = 0;

    void auto_save() { storage_.save(tasks_, id_counter_); }

public:
    explicit TaskManager(const std::string& file = "tasks.json")
        : storage_(file) {
        tasks_      = storage_.load();
        id_counter_ = storage_.load_next_id();
        // Ensure id_counter is at least as large as max existing id
        for (auto& t : tasks_)
            if (t.id > id_counter_) id_counter_ = t.id;
    }

    // ── CRUD ────────────────────────────────────────────────────────────────

    Task& add(const std::string& title,
              const std::string& desc     = "",
              Priority           prio     = Priority::Medium,
              const std::string& due      = "") {
        Task t;
        t.id          = util::next_id(id_counter_);
        t.title       = title;
        t.description = desc;
        t.priority    = prio;
        t.due_date    = due;
        t.created_at  = util::now_iso();
        t.updated_at  = t.created_at;
        tasks_.push_back(t);
        auto_save();
        return tasks_.back();
    }

    bool remove(int id) {
        auto it = std::find_if(tasks_.begin(), tasks_.end(),
                               [id](const Task& t) { return t.id == id; });
        if (it == tasks_.end()) return false;
        tasks_.erase(it);
        auto_save();
        return true;
    }

    Task* find(int id) {
        for (auto& t : tasks_) if (t.id == id) return &t;
        return nullptr;
    }

    bool update(int id, const std::map<std::string,std::string>& fields) {
        Task* t = find(id);
        if (!t) return false;
        for (auto& [k, v] : fields) {
            if (k == "title")       t->title       = v;
            if (k == "description") t->description = v;
            if (k == "priority")    t->priority    = priority_from(v);
            if (k == "status")      t->status      = status_from(v);
            if (k == "due_date")    t->due_date    = v;
        }
        t->updated_at = util::now_iso();
        auto_save();
        return true;
    }

    bool mark(int id, Status s) {
        Task* t = find(id);
        if (!t) return false;
        t->status     = s;
        t->updated_at = util::now_iso();
        auto_save();
        return true;
    }

    bool attach_file(int id, const std::string& path) {
        Task* t = find(id);
        if (!t) return false;
        t->files.push_back(path);
        t->updated_at = util::now_iso();
        auto_save();
        return true;
    }

    // ── Queries ─────────────────────────────────────────────────────────────

    std::vector<Task> list_all() const { return tasks_; }

    std::vector<Task> filter(const std::string& by_status,
                             const std::string& by_priority,
                             const std::string& by_date) const {
        std::vector<Task> result;
        for (auto& t : tasks_) {
            if (!by_status.empty() && !util::iequals(status_str(t.status), by_status))
                continue;
            if (!by_priority.empty() && !util::iequals(priority_str(t.priority), by_priority))
                continue;
            if (!by_date.empty() && t.due_date != by_date)
                continue;
            result.push_back(t);
        }
        return result;
    }

    // ── Sync helpers ────────────────────────────────────────────────────────

    void merge_remote(const std::vector<Task>& remote) {
        std::set<int> local_ids;
        for (auto& t : tasks_) local_ids.insert(t.id);
        for (auto& rt : remote) {
            if (local_ids.count(rt.id)) {
                // Conflict: latest wins
                Task* lt = find(rt.id);
                if (lt && rt.updated_at > lt->updated_at) *lt = rt;
            } else {
                tasks_.push_back(rt);
                if (rt.id > id_counter_) id_counter_ = rt.id;
            }
        }
        auto_save();
    }

    // Serialise all tasks to a single JSON string (for network transfer)
    std::string serialise_all() const {
        auto root = json::Value::make_object();
        root["next_id"] = json::Value(id_counter_);
        auto arr = json::Value::make_array();
        for (auto& t : tasks_) arr.push_back(t.to_json());
        root["tasks"] = arr;
        return root.to_json();
    }

    void deserialise_and_merge(const std::string& data) {
        auto root = json::parse(data);
        const auto* tasks_val = root.get("tasks");
        if (!tasks_val) return;
        std::vector<Task> remote;
        for (auto& jt : tasks_val->arr) remote.push_back(Task::from_json(jt));
        merge_remote(remote);
    }

    int count() const { return static_cast<int>(tasks_.size()); }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 6 – Pretty printer for tasks
// ═══════════════════════════════════════════════════════════════════════════════

namespace display {

inline void print_task(const Task& t) {
    using namespace util::color;
    std::string prio_col =
        t.priority == Priority::High ? RED :
        t.priority == Priority::Medium ? YELLOW : GREEN;
    std::string stat_col = t.status == Status::Completed ? GREEN : CYAN;

    std::cout << BOLD << "  #" << t.id << RESET
              << "  " << BOLD << t.title << RESET << "\n";
    if (!t.description.empty())
        std::cout << "      " << DIM << t.description << RESET << "\n";
    std::cout << "      Priority: " << prio_col << priority_str(t.priority) << RESET
              << "  |  Status: " << stat_col << status_str(t.status) << RESET;
    if (!t.due_date.empty())
        std::cout << "  |  Due: " << t.due_date;
    std::cout << "\n";
    if (!t.files.empty()) {
        std::cout << "      Files: ";
        for (size_t i = 0; i < t.files.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << t.files[i];
        }
        std::cout << "\n";
    }
    std::cout << "      Created: " << DIM << t.created_at << RESET
              << "  Updated: " << DIM << t.updated_at << RESET << "\n";
}

inline void print_task_list(const std::vector<Task>& tasks) {
    if (tasks.empty()) {
        std::cout << "  (no tasks)\n";
        return;
    }
    for (auto& t : tasks) { print_task(t); std::cout << "\n"; }
    std::cout << "  Total: " << tasks.size() << " task(s)\n";
}

} // namespace display

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 7 – Command parser
// ═══════════════════════════════════════════════════════════════════════════════

struct ParsedCommand {
    std::string              command;            // e.g. "add"
    std::map<std::string, std::string> flags;   // --key value
    std::vector<std::string> positional;         // non-flag args
};

inline ParsedCommand parse_command(const std::vector<std::string>& tokens) {
    ParsedCommand pc;
    if (tokens.empty()) return pc;
    pc.command = tokens[0];
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].rfind("--", 0) == 0) {
            std::string key = tokens[i].substr(2);
            if (i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0) {
                pc.flags[key] = tokens[++i];
            } else {
                pc.flags[key] = "true";
            }
        } else {
            pc.positional.push_back(tokens[i]);
        }
    }
    return pc;
}

// Tokenise respecting double-quoted strings
inline std::vector<std::string> tokenise(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') { in_quote = !in_quote; continue; }
        if (!in_quote && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 8 – File explorer module
// ═══════════════════════════════════════════════════════════════════════════════

class Explorer {
    fs::path cwd_;

public:
    Explorer() : cwd_(fs::current_path()) {}

    void run() {
        using namespace util::color;
        std::cout << BOLD << "\n=== File Explorer ===" << RESET
                  << " (type 'back' to return)\n\n";

        while (true) {
            std::cout << CYAN << "[explorer " << cwd_.string() << "]$ " << RESET;
            std::string line;
            if (!std::getline(std::cin, line)) break;
            auto tokens = tokenise(line);
            if (tokens.empty()) continue;
            auto cmd = tokens[0];

            if (cmd == "back" || cmd == "exit" || cmd == "quit") break;

            if (cmd == "pwd") {
                std::cout << cwd_.string() << "\n";
            } else if (cmd == "ls") {
                try {
                    fs::path target = tokens.size() > 1 ? cwd_ / tokens[1] : cwd_;
                    for (auto& entry : fs::directory_iterator(target)) {
                        bool is_dir = entry.is_directory();
                        std::cout << (is_dir ? BLUE + BOLD : "")
                                  << entry.path().filename().string()
                                  << (is_dir ? "/" : "")
                                  << RESET << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << RED << "Error: " << e.what() << RESET << "\n";
                }
            } else if (cmd == "cd") {
                if (tokens.size() < 2) { std::cout << cwd_.string() << "\n"; continue; }
                fs::path target = tokens[1];
                if (target.is_relative()) target = cwd_ / target;
                target = fs::weakly_canonical(target);
                if (fs::is_directory(target)) {
                    cwd_ = target;
                } else {
                    std::cerr << RED << "Not a directory: " << target.string() << RESET << "\n";
                }
            } else if (cmd == "mkdir") {
                if (tokens.size() < 2) { std::cerr << "Usage: mkdir <name>\n"; continue; }
                try {
                    fs::create_directories(cwd_ / tokens[1]);
                    std::cout << "Created: " << tokens[1] << "\n";
                } catch (const std::exception& e) {
                    std::cerr << RED << e.what() << RESET << "\n";
                }
            } else if (cmd == "rm") {
                if (tokens.size() < 2) { std::cerr << "Usage: rm <path>\n"; continue; }
                try {
                    auto cnt = fs::remove_all(cwd_ / tokens[1]);
                    std::cout << "Removed " << cnt << " item(s).\n";
                } catch (const std::exception& e) {
                    std::cerr << RED << e.what() << RESET << "\n";
                }
            } else if (cmd == "rename" || cmd == "mv") {
                if (tokens.size() < 3) {
                    std::cerr << "Usage: rename <old> <new>\n"; continue;
                }
                try {
                    fs::rename(cwd_ / tokens[1], cwd_ / tokens[2]);
                    std::cout << "Renamed.\n";
                } catch (const std::exception& e) {
                    std::cerr << RED << e.what() << RESET << "\n";
                }
            } else if (cmd == "cat") {
                if (tokens.size() < 2) { std::cerr << "Usage: cat <file>\n"; continue; }
                std::ifstream ifs(cwd_ / tokens[1]);
                if (!ifs) { std::cerr << RED << "Cannot open file." << RESET << "\n"; continue; }
                std::cout << ifs.rdbuf() << "\n";
            } else if (cmd == "touch") {
                if (tokens.size() < 2) { std::cerr << "Usage: touch <file>\n"; continue; }
                std::ofstream ofs(cwd_ / tokens[1], std::ios::app);
                std::cout << "Touched: " << tokens[1] << "\n";
            } else if (cmd == "help") {
                std::cout << "Commands: ls [dir], cd <dir>, pwd, mkdir <dir>, rm <path>,\n"
                          << "          rename <old> <new>, cat <file>, touch <file>,\n"
                          << "          back/exit\n";
            } else {
                std::cerr << "Unknown explorer command. Type 'help'.\n";
            }
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 9 – Networking layer
// ═══════════════════════════════════════════════════════════════════════════════

namespace net {

// ─── Socket RAII helper ─────────────────────────────────────────────────────

class Socket {
    int fd_ = -1;
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(int domain, int type, int proto) {
        fd_ = ::socket(domain, type, proto);
    }
    ~Socket() { if (fd_ >= 0) ::close(fd_); }
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Socket& operator=(Socket&& o) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.fd_; o.fd_ = -1; return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
    bool valid() const { return fd_ >= 0; }
};

// ─── Send / recv helpers (length-prefixed) ──────────────────────────────────

inline bool send_message(int fd, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    if (::send(fd, &len, 4, 0) != 4) return false;
    size_t sent = 0;
    while (sent < msg.size()) {
        auto n = ::send(fd, msg.data() + sent, msg.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline std::string recv_message(int fd) {
    uint32_t len_n = 0;
    if (::recv(fd, &len_n, 4, MSG_WAITALL) != 4) return "";
    uint32_t len = ntohl(len_n);
    if (len > 10'000'000) return ""; // sanity
    std::string buf(len, '\0');
    size_t got = 0;
    while (got < len) {
        auto n = ::recv(fd, buf.data() + got, len - got, 0);
        if (n <= 0) return "";
        got += static_cast<size_t>(n);
    }
    return buf;
}

// ═════════════════════════════════════════════════════════════════════════════
//  9.1 – TCP Server
// ═════════════════════════════════════════════════════════════════════════════

class TCPServer {
    int port_;
    std::atomic<bool> running_{false};
    TaskManager& mgr_;

    void handle_client(int client_fd) {
        while (true) {
            auto req = recv_message(client_fd);
            if (req.empty()) break;

            // Protocol: first line is the command, rest is payload
            auto nl = req.find('\n');
            std::string cmd  = (nl == std::string::npos) ? req : req.substr(0, nl);
            std::string body = (nl == std::string::npos) ? ""  : req.substr(nl + 1);

            std::string response;
            if (cmd == "LIST") {
                response = mgr_.serialise_all();
            } else if (cmd == "SYNC") {
                mgr_.deserialise_and_merge(body);
                response = mgr_.serialise_all();
            } else if (cmd == "PING") {
                response = "PONG";
            } else {
                response = "ERROR: unknown command";
            }
            if (!send_message(client_fd, response)) break;
        }
        ::close(client_fd);
    }

public:
    TCPServer(int port, TaskManager& mgr) : port_(port), mgr_(mgr) {}

    void start() {
        Socket srv(AF_INET, SOCK_STREAM, 0);
        if (!srv.valid()) {
            std::cerr << "Failed to create TCP socket.\n"; return;
        }
        int opt = 1;
        setsockopt(srv.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));

        if (::bind(srv.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "TCP bind failed on port " << port_ << ".\n"; return;
        }
        if (::listen(srv.fd(), 16) < 0) {
            std::cerr << "TCP listen failed.\n"; return;
        }

        running_ = true;
        std::cout << util::color::GREEN << "TCP server listening on port "
                  << port_ << util::color::RESET << "\n";

        while (running_) {
            sockaddr_in client_addr{};
            socklen_t cl = sizeof(client_addr);
            int cfd = ::accept(srv.fd(), reinterpret_cast<sockaddr*>(&client_addr), &cl);
            if (cfd < 0) { if (running_) std::cerr << "Accept error.\n"; break; }
            std::cout << "Client connected: "
                      << inet_ntoa(client_addr.sin_addr) << "\n";
            std::thread(&TCPServer::handle_client, this, cfd).detach();
        }
    }
    void stop() { running_ = false; }
};

// ═════════════════════════════════════════════════════════════════════════════
//  9.2 – TCP Client
// ═════════════════════════════════════════════════════════════════════════════

class TCPClient {
    std::string host_;
    int port_;
    Socket sock_;

public:
    TCPClient(const std::string& host, int port) : host_(host), port_(port) {}

    bool connect_to_server() {
        sock_ = Socket(AF_INET, SOCK_STREAM, 0);
        if (!sock_.valid()) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port_));

        struct hostent* he = gethostbyname(host_.c_str());
        if (!he) return false;
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], static_cast<size_t>(he->h_length));

        if (::connect(sock_.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            return false;
        return true;
    }

    std::string send_command(const std::string& cmd, const std::string& body = "") {
        std::string msg = body.empty() ? cmd : cmd + "\n" + body;
        if (!send_message(sock_.fd(), msg)) return "";
        return recv_message(sock_.fd());
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  9.4 – SSH helper (wraps system ssh binary)
// ═════════════════════════════════════════════════════════════════════════════

class SSHClient {
    std::string host_;
    std::string user_;
    int         port_;

public:
    SSHClient(const std::string& host, const std::string& user, int port = 22)
        : host_(host), user_(user), port_(port) {}

    int execute(const std::string& remote_cmd) const {
        std::ostringstream cmd;
        cmd << "ssh -o StrictHostKeyChecking=no -p " << port_
            << " " << user_ << "@" << host_ << " "
            << "'" << remote_cmd << "'";
        std::cout << util::color::DIM << "$ " << cmd.str() << util::color::RESET << "\n";
        return std::system(cmd.str().c_str());
    }

    std::string execute_capture(const std::string& remote_cmd) const {
        std::ostringstream cmd;
        cmd << "ssh -o StrictHostKeyChecking=no -p " << port_
            << " " << user_ << "@" << host_ << " "
            << "'" << remote_cmd << "' 2>&1";
        FILE* fp = popen(cmd.str().c_str(), "r");
        if (!fp) return "";
        std::string out;
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) out += buf;
        pclose(fp);
        return out;
    }

    // Copy local file to remote via scp
    int scp_to(const std::string& local_path, const std::string& remote_path) const {
        std::ostringstream cmd;
        cmd << "scp -P " << port_ << " " << local_path << " "
            << user_ << "@" << host_ << ":" << remote_path;
        std::cout << util::color::DIM << "$ " << cmd.str() << util::color::RESET << "\n";
        return std::system(cmd.str().c_str());
    }

    // Copy remote file to local via scp
    int scp_from(const std::string& remote_path, const std::string& local_path) const {
        std::ostringstream cmd;
        cmd << "scp -P " << port_ << " "
            << user_ << "@" << host_ << ":" << remote_path
            << " " << local_path;
        std::cout << util::color::DIM << "$ " << cmd.str() << util::color::RESET << "\n";
        return std::system(cmd.str().c_str());
    }
};

} // namespace net

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 10 – Help system
// ═══════════════════════════════════════════════════════════════════════════════

namespace help {

inline void print_usage() {
    using namespace util::color;
    std::cout << "\n" << BOLD << "Command-Line Task Manager" << RESET
              << " — help\n";
    util::hr('=');
    std::cout
        << BOLD << "Task Commands:" << RESET << "\n"
        << "  add     --title <t> [--desc <d>] [--priority low|medium|high] [--due YYYY-MM-DD]\n"
        << "  list    [--status pending|completed] [--priority low|medium|high] [--due YYYY-MM-DD]\n"
        << "  view    --id <n>\n"
        << "  update  --id <n> [--title <t>] [--desc <d>] [--priority <p>] [--status <s>]\n"
        << "  delete  --id <n>\n"
        << "  done    --id <n>         Mark task as completed\n"
        << "  pending --id <n>         Mark task as pending\n"
        << "  attach  --id <n> --file <path>\n"
        << "\n"
        << BOLD << "Explorer:" << RESET << "\n"
        << "  explorer                 Enter interactive file explorer\n"
        << "\n"
        << BOLD << "Networking:" << RESET << "\n"
        << "  server  --tcp <port>     Start TCP task server\n"
        << "  connect --tcp <host>:<port>     Connect to TCP task server\n"
        << "  sync    --tcp <host>:<port>     Sync tasks with remote server\n"
        << "  ssh     --host <h> --user <u> [--port <p>] --cmd <c>\n"
        << "  scp-to  --host <h> --user <u> --local <l> --remote <r>\n"
        << "  scp-from --host <h> --user <u> --remote <r> --local <l>\n"
        << "\n"
        << BOLD << "General:" << RESET << "\n"
        << "  help | --help            Show this help\n"
        << "  clear                    Clear screen\n"
        << "  exit | quit              Exit the program\n"
        << "\n";
}

} // namespace help

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 11 – Main CLI dispatcher
// ═══════════════════════════════════════════════════════════════════════════════

class CLI {
    TaskManager mgr_;
    Explorer    explorer_;
    bool        interactive_ = true;

    // Parse host:port
    static std::pair<std::string,int> parse_hostport(const std::string& s) {
        auto pos = s.rfind(':');
        if (pos == std::string::npos) return {s, 8080};
        return {s.substr(0, pos), std::stoi(s.substr(pos + 1))};
    }

    void dispatch(const ParsedCommand& pc) {
        using namespace util::color;
        const auto& cmd   = pc.command;
        const auto& flags = pc.flags;

        // ── help ────────────────────────────────────────────────────────────
        if (cmd == "help" || cmd == "--help" || cmd == "-h") {
            help::print_usage();
            return;
        }

        // ── add ─────────────────────────────────────────────────────────────
        if (cmd == "add") {
            auto it = flags.find("title");
            if (it == flags.end()) {
                std::cerr << RED << "Error: --title is required.\n" << RESET;
                return;
            }
            std::string desc;
            if (auto f = flags.find("desc"); f != flags.end()) desc = f->second;
            Priority prio = Priority::Medium;
            if (auto f = flags.find("priority"); f != flags.end())
                prio = priority_from(f->second);
            std::string due;
            if (auto f = flags.find("due"); f != flags.end()) due = f->second;

            auto& t = mgr_.add(it->second, desc, prio, due);
            std::cout << GREEN << "Task #" << t.id << " created." << RESET << "\n";
            return;
        }

        // ── list ────────────────────────────────────────────────────────────
        if (cmd == "list" || cmd == "ls-tasks") {
            std::string by_status, by_prio, by_date;
            if (auto f = flags.find("status");   f != flags.end()) by_status = f->second;
            if (auto f = flags.find("priority"); f != flags.end()) by_prio   = f->second;
            if (auto f = flags.find("due");      f != flags.end()) by_date   = f->second;

            auto tasks = (by_status.empty() && by_prio.empty() && by_date.empty())
                         ? mgr_.list_all()
                         : mgr_.filter(by_status, by_prio, by_date);
            std::cout << "\n";
            util::hr('-');
            display::print_task_list(tasks);
            util::hr('-');
            return;
        }

        // ── view ────────────────────────────────────────────────────────────
        if (cmd == "view") {
            auto it = flags.find("id");
            if (it == flags.end()) {
                std::cerr << RED << "Error: --id is required.\n" << RESET; return;
            }
            int id = std::stoi(it->second);
            Task* t = mgr_.find(id);
            if (!t) { std::cerr << RED << "Task #" << id << " not found.\n" << RESET; return; }
            std::cout << "\n";
            display::print_task(*t);
            return;
        }

        // ── update ──────────────────────────────────────────────────────────
        if (cmd == "update") {
            auto it = flags.find("id");
            if (it == flags.end()) {
                std::cerr << RED << "Error: --id is required.\n" << RESET; return;
            }
            int id = std::stoi(it->second);
            std::map<std::string,std::string> fields;
            for (auto& [k, v] : flags) {
                if (k != "id") fields[k == "desc" ? "description" : k] = v;
            }
            if (mgr_.update(id, fields))
                std::cout << GREEN << "Task #" << id << " updated." << RESET << "\n";
            else
                std::cerr << RED << "Task #" << id << " not found.\n" << RESET;
            return;
        }

        // ── delete ──────────────────────────────────────────────────────────
        if (cmd == "delete") {
            auto it = flags.find("id");
            if (it == flags.end()) {
                std::cerr << RED << "Error: --id is required.\n" << RESET; return;
            }
            int id = std::stoi(it->second);
            // Confirmation
            std::cout << YELLOW << "Delete task #" << id << "? (y/n): " << RESET;
            std::string ans;
            std::getline(std::cin, ans);
            if (util::trim(ans) != "y" && util::trim(ans) != "Y") {
                std::cout << "Cancelled.\n"; return;
            }
            if (mgr_.remove(id))
                std::cout << GREEN << "Task #" << id << " deleted." << RESET << "\n";
            else
                std::cerr << RED << "Task #" << id << " not found.\n" << RESET;
            return;
        }

        // ── done / pending ──────────────────────────────────────────────────
        if (cmd == "done" || cmd == "pending") {
            auto it = flags.find("id");
            if (it == flags.end()) {
                std::cerr << RED << "Error: --id is required.\n" << RESET; return;
            }
            int id = std::stoi(it->second);
            Status s = (cmd == "done") ? Status::Completed : Status::Pending;
            if (mgr_.mark(id, s))
                std::cout << GREEN << "Task #" << id << " marked " << status_str(s)
                          << "." << RESET << "\n";
            else
                std::cerr << RED << "Task #" << id << " not found.\n" << RESET;
            return;
        }

        // ── attach ──────────────────────────────────────────────────────────
        if (cmd == "attach") {
            auto it_id   = flags.find("id");
            auto it_file = flags.find("file");
            if (it_id == flags.end() || it_file == flags.end()) {
                std::cerr << RED << "Usage: attach --id <n> --file <path>\n" << RESET;
                return;
            }
            int id = std::stoi(it_id->second);
            if (mgr_.attach_file(id, it_file->second))
                std::cout << GREEN << "File attached to task #" << id << "." << RESET << "\n";
            else
                std::cerr << RED << "Task #" << id << " not found.\n" << RESET;
            return;
        }

        // ── explorer ────────────────────────────────────────────────────────
        if (cmd == "explorer") {
            explorer_.run();
            return;
        }

        // ── clear ───────────────────────────────────────────────────────────
        if (cmd == "clear") {
            std::cout << "\033[2J\033[1;1H";
            return;
        }

        // ─── TCP server ─────────────────────────────────────────────────────
        if (cmd == "server") {
            int port = 8080;
            if (auto f = flags.find("tcp"); f != flags.end())
                port = std::stoi(f->second);
            net::TCPServer server(port, mgr_);
            server.start(); // blocks
            return;
        }

        // ─── TCP connect (interactive remote session) ───────────────────────
        if (cmd == "connect") {
            auto it = flags.find("tcp");
            if (it == flags.end()) {
                std::cerr << RED << "Usage: connect --tcp <host>:<port>\n" << RESET; return;
            }
            auto [host, port] = parse_hostport(it->second);
            net::TCPClient client(host, port);
            if (!client.connect_to_server()) {
                std::cerr << RED << "Connection failed.\n" << RESET; return;
            }
            std::cout << GREEN << "Connected to " << host << ":" << port << RESET << "\n";
            // Quick ping
            auto pong = client.send_command("PING");
            std::cout << "Server says: " << pong << "\n";

            // Interactive remote loop
            std::cout << "Remote commands: LIST, SYNC, PING, quit\n";
            while (true) {
                std::cout << CYAN << "[remote]$ " << RESET;
                std::string line;
                if (!std::getline(std::cin, line)) break;
                line = util::trim(line);
                if (line.empty()) continue;
                if (line == "quit" || line == "exit") break;

                std::string resp;
                if (util::iequals(line, "SYNC")) {
                    resp = client.send_command("SYNC", mgr_.serialise_all());
                    if (!resp.empty()) {
                        mgr_.deserialise_and_merge(resp);
                        std::cout << GREEN << "Sync complete. "
                                  << mgr_.count() << " task(s) locally.\n" << RESET;
                    }
                } else {
                    resp = client.send_command(line);
                    std::cout << resp << "\n";
                }
            }
            return;
        }

        // ─── sync (one-shot) ────────────────────────────────────────────────
        if (cmd == "sync") {
            auto it = flags.find("tcp");
            if (it == flags.end()) {
                std::cerr << RED << "Usage: sync --tcp <host>:<port>\n" << RESET; return;
            }
            auto [host, port] = parse_hostport(it->second);
            net::TCPClient client(host, port);
            if (!client.connect_to_server()) {
                std::cerr << RED << "Connection failed.\n" << RESET; return;
            }
            auto resp = client.send_command("SYNC", mgr_.serialise_all());
            if (!resp.empty()) {
                mgr_.deserialise_and_merge(resp);
                std::cout << GREEN << "Sync complete. "
                          << mgr_.count() << " task(s) locally." << RESET << "\n";
            } else {
                std::cerr << RED << "Sync failed (empty response).\n" << RESET;
            }
            return;
        }

        // ─── SSH ────────────────────────────────────────────────────────────
        if (cmd == "ssh") {
            auto h = flags.find("host"), u = flags.find("user"), c = flags.find("cmd");
            if (h == flags.end() || u == flags.end() || c == flags.end()) {
                std::cerr << RED
                    << "Usage: ssh --host <h> --user <u> [--port <p>] --cmd <c>\n"
                    << RESET;
                return;
            }
            int port = 22;
            if (auto f = flags.find("port"); f != flags.end()) port = std::stoi(f->second);
            net::SSHClient ssh(h->second, u->second, port);
            ssh.execute(c->second);
            return;
        }

        // ─── SCP ────────────────────────────────────────────────────────────
        if (cmd == "scp-to" || cmd == "scp-from") {
            auto h = flags.find("host"), u = flags.find("user");
            auto l = flags.find("local"), r = flags.find("remote");
            if (h == flags.end() || u == flags.end() ||
                l == flags.end() || r == flags.end()) {
                std::cerr << RED
                    << "Usage: " << cmd << " --host <h> --user <u> --local <l> --remote <r>\n"
                    << RESET;
                return;
            }
            int port = 22;
            if (auto f = flags.find("port"); f != flags.end()) port = std::stoi(f->second);
            net::SSHClient ssh(h->second, u->second, port);
            if (cmd == "scp-to")
                ssh.scp_to(l->second, r->second);
            else
                ssh.scp_from(r->second, l->second);
            return;
        }

        // ─── unknown ────────────────────────────────────────────────────────
        std::cerr << RED << "Unknown command: " << cmd
                  << ". Type 'help' for usage.\n" << RESET;
    }

public:
    CLI() = default;

    // Run a single command from argv
    void run_once(int argc, char* argv[]) {
        interactive_ = false;
        std::vector<std::string> tokens;
        for (int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
        auto pc = parse_command(tokens);
        dispatch(pc);
    }

    // Interactive REPL
    void repl() {
        using namespace util::color;
        std::cout << "\n" << BOLD << "╔══════════════════════════════════════════╗\n"
                  << "║   Command-Line Task Manager v1.0         ║\n"
                  << "╚══════════════════════════════════════════╝" << RESET << "\n"
                  << DIM << "Type 'help' for commands, 'exit' to quit.\n"
                  << "Tasks stored in: " << fs::absolute("tasks.json").string()
                  << RESET << "\n\n";

        while (true) {
            std::cout << BOLD << BLUE << "taskrunner" << RESET
                      << BOLD << " > " << RESET;
            std::string line;
            if (!std::getline(std::cin, line)) break;
            line = util::trim(line);
            if (line.empty()) continue;
            if (line == "exit" || line == "quit") {
                std::cout << "Goodbye!\n";
                break;
            }

            auto tokens = tokenise(line);
            auto pc     = parse_command(tokens);
            try {
                dispatch(pc);
            } catch (const std::exception& e) {
                std::cerr << RED << "Error: " << e.what() << RESET << "\n";
            }
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Section 12 – Entry point
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE for network robustness
    std::signal(SIGPIPE, SIG_IGN);

    CLI cli;

    if (argc > 1) {
        // Single-shot mode: run command from arguments
        cli.run_once(argc, argv);
    } else {
        // Interactive REPL
        cli.repl();
    }

    return 0;
}
