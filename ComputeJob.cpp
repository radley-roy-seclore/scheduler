#include "ComputeJob.h"
#include <thread>
#include <chrono>

ComputeJob::ComputeJob(int id, int complexity)
    : Job(id, JobType::Compute), complexity(complexity) {}

TaskResult* ComputeJob::execute()
{
    setStatus(JobStatus::Running);

    std::this_thread::sleep_for(std::chrono::milliseconds(complexity));

    setStatus(JobStatus::Completed);

    return new TaskResult(true, "Compute job finished");
}
