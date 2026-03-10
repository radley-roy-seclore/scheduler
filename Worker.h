#pragma once

#include "Job.h"

class Worker
{
private:
    int workerId;
    bool busy;

public:
    Worker(int id);

    bool isBusy() const;

    TaskResult* runJob(Job* job);
};
