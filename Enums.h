#pragma once

enum class JobStatus
{
    Pending,
    Running,
    Completed,
    Failed
};

enum class JobType
{
    IO,
    Compute,
    Network
};

enum class LogLevel
{
    Debug,
    Info,
    Warning,
    Error
};
