#pragma once

#include <string>
#include "Enums.h"
#include "TaskResult.h"

class Job
{
private:
    int id;
    JobType type;
    JobStatus status;

public:
    Job(int id, JobType type);
    virtual ~Job();

    int getId() const;
    JobStatus getStatus() const;
    JobType getType() const;

    void setStatus(JobStatus s);

    virtual TaskResult* execute() = 0;
};
