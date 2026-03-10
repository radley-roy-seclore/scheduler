#pragma once

#include <string>
#include "Enums.h"

class Logger
{
public:
    static void log(LogLevel level, const std::string& message);
};
