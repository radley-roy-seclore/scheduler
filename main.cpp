#include "Scheduler.h"
#include "ComputeJob.h"
#include "Utils.h"

int main()
{
    Scheduler scheduler(3);

    for (int i = 0; i < 10; i++)
    {
        int complexity = Utils::randomInt(100, 500);

        scheduler.submit(new ComputeJob(i, complexity));
    }

    scheduler.schedule();

    return 0;
}
