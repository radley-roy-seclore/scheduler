#include <iostream>
#include <string>
#include <string_view>

auto getRandomName() {
    return std::string_view("Vamsi R");
}

struct Player {
    const uint64_t mId;
    std::string mName;

    Player(uint64_t pId) : mId(pId), mName(getRandomName()) {}
    Player(std::string_view pName) : mId(std::hash<std::string_view>{}(pName)), mName(pName) {}
};

int main(int argc, char* argv[]) {

    const bool lbRemote = argc > 1;
    alignas(Player) std::byte storage[sizeof(Player)];

    if (lbRemote) {
        new (&storage) Player{4564564ULL};
    } else {
        new (&storage) Player{"local guest"};
    }

    std::cout << reinterpret_cast<Player*>(&storage)->mId << std::endl;
    return 0;
}
