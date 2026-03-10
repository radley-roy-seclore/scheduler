#include "Job.h"

Job::Job(int id, JobType type)
    : id(id), type(type), status(JobStatus::Pending) {}

Job::~Job() {}

int Job::getId() const
{
    return id;
}

JobType Job::getType() const
{ 
    return type; 
}

JobStatus Job::getStatus() const
{
    return status;
}

void Job::setStatus(JobStatus s)
{
    status = s;
}
