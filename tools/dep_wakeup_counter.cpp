//
// dep_wakeup_counter.cpp — Period-wakeup diagnostic tool.
//
// Attaches to a DEP shared-memory buffer and FUTEX_WAITs on period_count, the same
// way any FUTEX_WAIT client (this SDK, dep_sync_fanoutd's other consumers) does.
// Once a second it prints how many periods actually ticked, so you can confirm
// dep_sync_fanoutd is broadcasting FUTEX_WAKE at the expected rate rather than
// silently degrading clients to their poll-timeout fallback.
//
// Usage: dep_wakeup_counter <shm-name>
//   shm-name  Name of the DEP shared-memory region (e.g. "DanteEP")
//
// Does not link against libdep_audio.a: like dep_sync_fanoutd, it only needs the
// buffer_header_t layout (from DanteAudio.hpp) to map the header and read/wait on
// period_count directly — no live Buffers connection required.
//

#include <dante/DanteAudio.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Dante;

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

// Conservative upper bound for the header + timing subheader region, matching
// dep_sync_fanoutd's mapping — we only need the first ~512 bytes.
static constexpr size_t HEADER_MAP_SIZE = 4096;

int main(int argc, char * argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <shm-name>\n", argv[0]);
        return 1;
    }

    char shmPath[300];
    snprintf(shmPath, sizeof(shmPath), "/%s", argv[1]);

    int fd = shm_open(shmPath, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s): %s\n", shmPath, strerror(errno));
        return 1;
    }

    void * map = mmap(nullptr, HEADER_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return 1;
    }

    auto * hdr = static_cast<const volatile buffer_header_t *>(map);

    if (hdr->metadata.magic_marker != DANTE_BUFFERS_HDR_MAGIC) {
        fprintf(stderr, "bad magic marker — DEP not ready?\n");
        munmap(map, HEADER_MAP_SIZE);
        return 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    const unsigned sampleRate       = hdr->audio.sample_rate;
    const unsigned samplesPerPeriod = hdr->time.samples_per_period;
    const double   expectedPerSec   = samplesPerPeriod ? (double) sampleRate / samplesPerPeriod : 0.0;

    fprintf(stderr, "[dep_wakeup_counter] attached to %s (DEP period rate ~%.0f/sec)\n",
            shmPath, expectedPerSec);

    // period_count itself advances whenever DEP posts a period, regardless of whether
    // anyone is listening — it is NOT a measure of the wakeup path. What we actually
    // want is how many FUTEX_WAKE deliveries this process received, i.e. how many of
    // dep_sync_fanoutd's sem_wait -> FUTEX_WAKE relays actually reached us. That's only
    // true when the futex syscall unblocks us with a real wake (return 0) — EAGAIN
    // (value already moved before we blocked) and ETIMEDOUT/EINTR are NOT wakeups
    // delivered to us, so they're deliberately not counted.
    uint64_t seen = hdr->time.period_count;
    uint64_t wakeupsThisSecond = 0;
    auto nextPrint = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (g_running) {
        uint32_t * word = getPeriodCountFutexWord(hdr);
        struct timespec ts { 0, 200 * 1000000L };  // 200 ms

        long r = syscall(SYS_futex, word, FUTEX_WAIT, (uint32_t) seen, &ts, nullptr, 0);
        if (r == 0) {
            ++wakeupsThisSecond;  // genuinely woken by a FUTEX_WAKE
        }
        // EAGAIN:    period_count already moved before we blocked — a wake fired, but
        //            not one delivered to us while waiting; not counted.
        // ETIMEDOUT/EINTR: no wake within the timeout; not counted.

        seen = hdr->time.period_count;  // resync so the next wait targets the current value

        auto now = std::chrono::steady_clock::now();
        if (now >= nextPrint) {
            printf("wakeups: %llu\n", (unsigned long long) wakeupsThisSecond);
            fflush(stdout);
            wakeupsThisSecond = 0;
            nextPrint = now + std::chrono::seconds(1);
        }
    }

    fprintf(stderr, "[dep_wakeup_counter] shutting down\n");
    munmap(map, HEADER_MAP_SIZE);
    return 0;
}
