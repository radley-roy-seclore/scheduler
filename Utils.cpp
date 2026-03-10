#include "Utils.h"
#include <random>

std::string Utils::generateJobName(int id)
{
    return "job_" + std::to_string(id);
}

int Utils::randomInt(int min, int max)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    std::uniform_int_distribution<> dis(min, max);

    return dis(gen);
}
