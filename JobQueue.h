#pragma once

#include <queue>
#include <mutex>
#include "Job.h"

class JobQueue
{
private:
    std::queue<Job*> jobs;
    std::mutex mtx;

public:
    void push(Job* job);

    Job* pop();

    bool empty();
};
