/* Single-byte PMIF channel transactions. Callers must serialize each complete
 * transaction, including any read/modify/write sequence. This hardware-neutral
 * core does not map hardware, create locks, or start threads. */
#ifndef LAPIS_PMIF_H
#define LAPIS_PMIF_H
#include <stdint.h>

#define LAPIS_PMIF_ACC    0x00u
#define LAPIS_PMIF_WDATA  0x04u
#define LAPIS_PMIF_RDATA  0x14u
#define LAPIS_PMIF_CLR    0x24u
#define LAPIS_PMIF_STA    0x28u
#define LAPIS_PMIF_FSM(s) (((s) >> 1) & 7u)
#define LAPIS_PMIF_IDLE   0u
#define LAPIS_PMIF_VALID  6u

struct lapis_pmif_io {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delay)(void *);
};

enum lapis_pmif_result {
    LAPIS_PMIF_OK = 0,
    LAPIS_PMIF_INVALID,
    LAPIS_PMIF_UNAVAILABLE,
    LAPIS_PMIF_IDLE_TIMEOUT,
    LAPIS_PMIF_READ_TIMEOUT,
    LAPIS_PMIF_WRITE_TIMEOUT
};

/* Kernel adapter: reports UNAVAILABLE until its mapping and lock are ready. */
enum lapis_pmif_result lapis_pmif_shared_read(uint32_t sid, uint32_t addr, uint8_t *out);

static inline int
lapis_pmif_valid(const struct lapis_pmif_io *io, uint32_t sid, uint32_t addr, unsigned polls)
{
    return io && io->read && io->write && io->delay && sid <= 15 && addr <= 0xffff && polls;
}

static inline int
lapis_pmif_wait(const struct lapis_pmif_io *io, unsigned wanted, unsigned polls, uint32_t *last)
{
    for (unsigned n = 0; n < polls; ++n) {
        *last = io->read(io->context, LAPIS_PMIF_STA);
        if (LAPIS_PMIF_FSM(*last) == wanted) return 1;
        io->delay(io->context);
    }
    return 0;
}

/* Mirrors the Android driver's stale-valid cleanup only when the last observed
 * state is VALID. It does not force-clear any other hardware state. */
static inline void
lapis_pmif_clear_stale_valid(const struct lapis_pmif_io *io, uint32_t last)
{
    if (LAPIS_PMIF_FSM(last) == LAPIS_PMIF_VALID)
        io->write(io->context, LAPIS_PMIF_CLR, 1);
}

static inline enum lapis_pmif_result
lapis_pmif_read_byte(const struct lapis_pmif_io *io, uint32_t sid, uint32_t addr,
                     unsigned polls, uint8_t *out)
{
    uint32_t status = 0;
    if (!out || !lapis_pmif_valid(io, sid, addr, polls)) return LAPIS_PMIF_INVALID;
    if (!lapis_pmif_wait(io, LAPIS_PMIF_IDLE, polls, &status)) {
        lapis_pmif_clear_stale_valid(io, status);
        return LAPIS_PMIF_IDLE_TIMEOUT;
    }
    io->write(io->context, LAPIS_PMIF_ACC, (3u << 30) | (sid << 24) | addr);
    if (!lapis_pmif_wait(io, LAPIS_PMIF_VALID, polls, &status)) {
        /* Outcome is unknown; do not consume RDATA or force-clear the channel. */
        return LAPIS_PMIF_READ_TIMEOUT;
    }
    uint8_t value = (uint8_t)io->read(io->context, LAPIS_PMIF_RDATA);
    io->write(io->context, LAPIS_PMIF_CLR, 1);
    *out = value;
    return LAPIS_PMIF_OK;
}

/* A returned idle FSM is not proof that the codec accepted a write. When safe,
 * higher-level code must read back stable configuration bits. On timeout the
 * result is unknown; callers must not assume the register remained unchanged. */
static inline enum lapis_pmif_result
lapis_pmif_write_byte(const struct lapis_pmif_io *io, uint32_t sid, uint32_t addr,
                      unsigned polls, uint8_t value)
{
    uint32_t status = 0;
    if (!lapis_pmif_valid(io, sid, addr, polls)) return LAPIS_PMIF_INVALID;
    if (!lapis_pmif_wait(io, LAPIS_PMIF_IDLE, polls, &status)) {
        lapis_pmif_clear_stale_valid(io, status);
        return LAPIS_PMIF_IDLE_TIMEOUT;
    }
    io->write(io->context, LAPIS_PMIF_WDATA, value);
    io->write(io->context, LAPIS_PMIF_ACC, (3u << 30) | (1u << 29) | (sid << 24) | addr);
    if (!lapis_pmif_wait(io, LAPIS_PMIF_IDLE, polls, &status)) return LAPIS_PMIF_WRITE_TIMEOUT;
    return LAPIS_PMIF_OK;
}
#endif
