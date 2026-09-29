#ifndef MEP_FRAME_ACTIVITY_H
#define MEP_FRAME_ACTIVITY_H

// "Something happened that the screen may need to show": a job's output
// or exit, an agent-socket or JSON-RPC message, a queued synthetic input
// step. main()'s loop reads this to decide whether a frame is needed now
// or mep can sleep until the next event (see IdleWaitSeconds there) --
// without it an idle mep redrew every frame at the display's refresh
// rate. A counter, not a flag, so readers need no reset protocol.

#include <atomic>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <unistd.h>
#define MEP_WAKE_PIPE 1
#endif

namespace mep {

inline std::atomic<unsigned long> &ActivityEpoch() {
    static std::atomic<unsigned long> epoch{0};
    return epoch;
}

inline void NoteActivity() { ActivityEpoch().fetch_add(1, std::memory_order_relaxed); }

// A pipe the main loop's idle sleep also watches, so data arriving on a
// background thread (a job's output, an agent request) wakes it at once
// instead of on its next idle tick. WakeMainLoop is safe from any thread.
#if MEP_WAKE_PIPE
struct WakePipe {
    int fds[2] = {-1, -1};
    WakePipe() {
        if (pipe(fds) != 0) return;
        for (int fd : fds) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
};
inline WakePipe &MainLoopWakePipe() {
    static WakePipe pipe_fds;
    return pipe_fds;
}
inline int WakeFd() { return MainLoopWakePipe().fds[0]; }
inline void WakeMainLoop() {
    const int fd = MainLoopWakePipe().fds[1];
    if (fd < 0) return;
    const char byte = 1;
    ssize_t n = write(fd, &byte, 1);  // (full: the loop is already due to wake)
    (void)n;
}
// Empties the pipe after a wake.
inline void DrainWakes() {
    const int fd = WakeFd();
    if (fd < 0) return;
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}
#else
inline int WakeFd() { return -1; }
inline void WakeMainLoop() {}
inline void DrainWakes() {}
#endif

}  // namespace mep

#endif
