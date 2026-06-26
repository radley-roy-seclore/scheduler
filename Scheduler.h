#pragma once

#include <vector>
#include "Worker.h"
#include "JobQueue.h"

class Scheduler
{
private:
    std::vector<Worker*> workers;
    JobQueue queue;

public:
    Scheduler(int workerCount);
    ~Scheduler();

    void submit(Job* job);

    void schedule();
};
