#include "JobQueue.h"

void JobQueue::push(Job* job)
{
    std::lock_guard<std::mutex> lock(mtx);
    jobs.push(job);
}

Job* JobQueue::pop()
{
    std::lock_guard<std::mutex> lock(mtx);

    if (jobs.empty())
        return nullptr;

    Job* job = jobs.front();
    jobs.pop();

    return job;
}

bool JobQueue::empty()
{
    return jobs.empty();
}
