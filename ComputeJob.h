#pragma once

#include "Job.h"

class ComputeJob : public Job
{
private:
    int complexity;

public:
    ComputeJob(int id, int complexity);

    TaskResult* execute() override;
};
