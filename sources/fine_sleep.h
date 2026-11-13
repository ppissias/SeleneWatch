#pragma once

// Raises the Windows timer resolution to 1 ms while it exists, so that
// sleep_until and timed waits wake on time for frame intervals of a few
// milliseconds. The setting is process-wide and counted by Windows, so
// holders may overlap. A source holds one on its worker thread while it runs.
class FineSleepResolution
{
public:
    FineSleepResolution();
    ~FineSleepResolution();

    FineSleepResolution(const FineSleepResolution&) = delete;
    FineSleepResolution& operator=(const FineSleepResolution&) = delete;

private:
    bool held_{false};
};
