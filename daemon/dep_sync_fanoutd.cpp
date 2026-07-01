//
// Copyright © 2020-2026 Audinate Pty Ltd ACN 120 828 006 (Audinate). All rights reserved.
//
// See DanteAudio.hpp for licence terms.
//
// dep_sync_fanoutd.cpp — Period-sync relay daemon.
//
// The DEP server posts a POSIX named semaphore once per period. Because sem_post()
// unblocks only one waiter, multiple clients cannot safely share it. This daemon holds
// the semaphore exclusively and translates each post into a FUTEX_WAKE(INT_MAX) on the
// period_count word in shared memory, allowing any number of FUTEX_WAIT clients to wake
// simultaneously.
//
// Usage: dep_sync_fanoutd <shm-name>
//   shm-name  Name of the DEP shared-memory region (e.g. "DanteBuffers")
//

#include <dante/DanteAudio.hpp>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <linux/futex.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Dante;

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

// Conservative upper bound for the header + timing subheader region.
// The audio data starts much further in; we only need the first ~512 bytes.
static constexpr size_t HEADER_MAP_SIZE = 4096;

int main(int argc, char * argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <shm-name>\n", argv[0]);
        return 1;
    }

    // --- Open and map the DEP buffer header ---

    char shmPath[300];
    snprintf(shmPath, sizeof(shmPath), "/%s", argv[1]);

    int fd = shm_open(shmPath, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "[fanoutd] shm_open(%s): %s\n", shmPath, strerror(errno));
        return 1;
    }

    void * map = mmap(nullptr, HEADER_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "[fanoutd] mmap: %s\n", strerror(errno));
        return 1;
    }

    auto * hdr = static_cast<const volatile buffer_header_t *>(map);

    if (hdr->metadata.magic_marker != DANTE_BUFFERS_HDR_MAGIC) {
        fprintf(stderr, "[fanoutd] bad magic marker — DEP not ready?\n");
        munmap(map, HEADER_MAP_SIZE);
        return 1;
    }

    // --- Locate and validate the timing object subheader ---

    uint32_t toOff = hdr->metadata.timing_object_subheader_offset_bytes;
    if (toOff + sizeof(timing_object_subheader_t) > HEADER_MAP_SIZE) {
        fprintf(stderr, "[fanoutd] timing subheader offset %u exceeds map window\n", toOff);
        munmap(map, HEADER_MAP_SIZE);
        return 1;
    }

    auto * tsub = reinterpret_cast<const timing_object_subheader_t *>(
        static_cast<const char *>(map) + toOff);

    if (tsub->object_type != TIMING_OBJECT_TYPE__SIGNAL_EVENT) {
        fprintf(stderr, "[fanoutd] unexpected timing object type %u\n", tsub->object_type);
        munmap(map, HEADER_MAP_SIZE);
        return 1;
    }

    // object_name may or may not carry a leading '/'; sem_open requires one
    char semPath[TIMING_OBJECT_NAME_LENGTH + 2];
    if (tsub->object_name[0] == '/') {
        snprintf(semPath, sizeof(semPath), "%.*s",
                 TIMING_OBJECT_NAME_LENGTH, tsub->object_name);
    } else {
        snprintf(semPath, sizeof(semPath), "/%.*s",
                 TIMING_OBJECT_NAME_LENGTH, tsub->object_name);
    }

    // --- Open the pre-existing DEP semaphore ---

    sem_t * sem = sem_open(semPath, 0);
    if (sem == SEM_FAILED) {
        fprintf(stderr, "[fanoutd] sem_open(%s): %s\n", semPath, strerror(errno));
        munmap(map, HEADER_MAP_SIZE);
        return 1;
    }

    uint32_t * period_count_futex_word = getPeriodCountFutexWord(hdr);

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    fprintf(stderr, "[fanoutd] started — shm=%s sem=%s\n", shmPath, semPath);

    // --- Main loop: semaphore wait → futex broadcast ---

    while (g_running) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;  // 1 s timeout so SIGTERM is handled promptly

        int r = sem_timedwait(sem, &deadline);
        if (r == 0) {
            // Period fired — wake every FUTEX_WAIT caller on period_count
            syscall(SYS_futex, period_count_futex_word, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        } else if (errno == ETIMEDOUT || errno == EINTR) {
            continue;
        } else {
            fprintf(stderr, "[fanoutd] sem_timedwait: %s\n", strerror(errno));
            break;
        }
    }

    fprintf(stderr, "[fanoutd] shutting down\n");
    sem_close(sem);
    munmap(map, HEADER_MAP_SIZE);
    return 0;
}
