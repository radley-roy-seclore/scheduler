#pragma once
#include <string>

class TaskResult
{
public:
    bool success;
    std::string message;

    TaskResult(bool s, const std::string& msg)
        : success(s), message(msg) {}
};
