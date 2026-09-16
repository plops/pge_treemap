#pragma once

#include <chrono>
#include <thread>

// ----------------------------------------------------------------------------
// Linux / X11 Idle Fix:
// PGE3's Host_Linux_X11::StartSystem() spins XPending() at 100% CPU when idle.
// Intercepting XPending to sleep briefly when empty drops idle CPU to ~0%.
// We ensure the symbol is exported with default visibility and not inlined.
// ----------------------------------------------------------------------------
#if defined(__linux__)
extern "C" {
struct _XDisplay;
int XEventsQueued(struct _XDisplay* display, int mode);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
inline int XPending(struct _XDisplay* display)
{
    // Mode 2 = QueuedAfterFlush
    int count = XEventsQueued(display, 2);
    if (count == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // Mode 0 = QueuedAlready
        count = XEventsQueued(display, 0);
    }
    return count;
}
}
#endif
