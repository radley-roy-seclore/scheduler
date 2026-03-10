#include "Worker.h"

Worker::Worker(int id)
    : workerId(id), busy(false) {}

bool Worker::isBusy() const
{
    return busy;
}

TaskResult* Worker::runJob(Job* job)
{
    busy = true;

    TaskResult* result = job->execute();

    busy = false;

    return result;
}
