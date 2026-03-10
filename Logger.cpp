#include "Logger.h"
#include <iostream>

void Logger::log(LogLevel level, const std::string& message)
{
    switch (level)
    {
        case LogLevel::Debug:
            std::cout << "[DEBUG] ";
            break;
        case LogLevel::Info:
            std::cout << "[INFO] ";
            break;
        case LogLevel::Warning:
            std::cout << "[WARNING] ";
            break;
        case LogLevel::Error:
            std::cout << "[ERROR] ";
            break;
    }

    std::cout << message << std::endl;
}
