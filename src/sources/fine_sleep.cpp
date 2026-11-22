#include "sources/fine_sleep.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

FineSleepResolution::FineSleepResolution()
    : held_(timeBeginPeriod(1) == TIMERR_NOERROR)
{
}

// Release a timer request only when Windows accepted it in the constructor.
FineSleepResolution::~FineSleepResolution()
{
    if (held_)
    {
        timeEndPeriod(1);
    }
}
