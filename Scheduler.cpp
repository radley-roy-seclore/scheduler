#include "Scheduler.h"
#include "Logger.h"

Scheduler::Scheduler(int workerCount)
{
    for (int i = 0; i < workerCount; i++)
    {
        workers.push_back(new Worker(i));
    }
}

Scheduler::~Scheduler()
{
    for (auto w : workers)
    {
        delete w;
    }
}

void Scheduler::submit(Job* job)
{
    queue.push(job);
}

void Scheduler::schedule()
{
    while (!queue.empty())
    {
        for (auto worker : workers)
        {
            if (!worker->isBusy())
            {
                Job* job = queue.pop();

                if (!job)
                    break;

                worker->runJob(job);

                Logger::log(LogLevel::Info, "Job executed by worker");
            }
        }
    }
}