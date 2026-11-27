/* Optional aggregate-only microphone self-test report. The future device may
 * log this metadata; it must never contain PCM or user speech. */
#ifndef LAPIS_MIC_TEST_H
#define LAPIS_MIC_TEST_H
#include <stdint.h>
#if defined(__APPLE__) || defined(KERNEL)
#include <sys/ioccom.h>
#else
#include <sys/ioctl.h>
#endif
struct lapis_mic_selftest_report {
    uint64_t frames, nonzero, errors, odd_reads;
    uint32_t peak, reserved;
};
#define LAPIS_MIC_SELFTEST_REPORT _IOW('m', 1, struct lapis_mic_selftest_report)
#endif
