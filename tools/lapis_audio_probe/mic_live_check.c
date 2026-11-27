/* Optional iOS live check for a future /dev/hakios-mic implementation.
 * Reads mono S16LE into a short stack buffer; reports only aggregate counts
 * and peak. No PCM is written to disk or sent over a network. This opens the
 * device directly and therefore does not prove AVAudioSession app routing. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../../device/xnu-lapis/driver/lapis_mic_test.h"

#ifndef LAPIS_MIC_SELFTEST_DEVICE
#define LAPIS_MIC_SELFTEST_DEVICE "/dev/hakios-mic"
#endif
#ifndef LAPIS_MIC_SELFTEST_DELAY_SEC
#define LAPIS_MIC_SELFTEST_DELAY_SEC 60
#endif
#ifndef LAPIS_MIC_SELFTEST_DURATION_NS
#define LAPIS_MIC_SELFTEST_DURATION_NS 120000000000ULL
#endif

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

int main(void)
{
    /* Allow the user to reach the interface. The operator is told the window
     * before this optional executable is installed. */
    sleep(LAPIS_MIC_SELFTEST_DELAY_SEC);
    int fd = open(LAPIS_MIC_SELFTEST_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "LAPIS MIC SELFTEST: open failed errno=%d\n", errno);
        return 1;
    }
    uint64_t start = now_ns(), frames = 0, nonzero = 0, errors = 0, odd_reads = 0;
    unsigned peak = 0;
    if (!start) { close(fd); return 2; }
    for (;;) {
        uint64_t now = now_ns();
        if (!now || now < start || now - start >= LAPIS_MIC_SELFTEST_DURATION_NS) break;
        uint8_t bytes[1024];
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n > 0) {
            if (n & 1) odd_reads++;
            for (ssize_t i = 0; i + 1 < n; i += 2) {
                uint32_t u = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
                int32_t v = u & 0x8000u ? (int32_t)u - 0x10000 : (int32_t)u;
                unsigned mag = (unsigned)(v < 0 ? -v : v);
                if (mag) nonzero++;
                if (mag > peak) peak = mag;
                frames++;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            errors++;
            if (errors >= 10) break;
        }
        volatile uint8_t *wipe = bytes;
        for (unsigned i = 0; i < sizeof(bytes); i++) wipe[i] = 0;
        usleep(10000);
    }
    fprintf(stderr, "LAPIS MIC SELFTEST: frames=%llu nonzero=%llu peak=%u errors=%llu odd=%llu\n",
        (unsigned long long)frames, (unsigned long long)nonzero, peak,
        (unsigned long long)errors, (unsigned long long)odd_reads);
    struct lapis_mic_selftest_report report = { frames, nonzero, errors, odd_reads, peak, 0 };
    if (ioctl(fd, LAPIS_MIC_SELFTEST_REPORT, &report) != 0)
        fprintf(stderr, "LAPIS MIC SELFTEST: aggregate report ioctl unavailable errno=%d\n", errno);
    close(fd);
    return !frames || !nonzero || errors || odd_reads ? 3 : 0;
}
