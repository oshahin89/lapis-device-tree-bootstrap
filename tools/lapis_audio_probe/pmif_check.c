/* Hardware-independent protocol checks: no device or microphone is accessed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_pmif.h"

struct fake {
    uint32_t states[8]; unsigned nstates, pos, delays;
    uint32_t regs[8], values[8]; unsigned writes;
    uint32_t data, next_data; unsigned data_reads; int use_next_data;
};
static uint32_t get(void *ctx, uint32_t off)
{
    struct fake *f = ctx;
    if (off == LAPIS_PMIF_RDATA) {
        unsigned n = f->data_reads++;
        return f->use_next_data && n ? f->next_data : f->data;
    }
    assert(off == LAPIS_PMIF_STA && f->nstates);
    unsigned i = f->pos < f->nstates ? f->pos++ : f->nstates - 1;
    return f->states[i];
}
static void put(void *ctx, uint32_t off, uint32_t val)
{
    struct fake *f = ctx; assert(f->writes < 8);
    f->regs[f->writes] = off; f->values[f->writes++] = val;
}
static void delay_one(void *ctx) { ((struct fake *)ctx)->delays++; }
static struct lapis_pmif_io ops(struct fake *f)
{
    return (struct lapis_pmif_io){ f, get, put, delay_one };
}
int main(void)
{
    struct fake f = { .states = { 0, 2, 12 }, .nstates = 3, .data = 0x169 };
    struct lapis_pmif_io io = ops(&f); uint8_t out = 0;
    assert(lapis_pmif_read_byte(&io, 5, 0x0b, 4, &out) == LAPIS_PMIF_OK);
    assert(out == 0x69 && f.writes == 2 && f.regs[0] == LAPIS_PMIF_ACC);
    assert(f.values[0] == ((3u << 30) | (5u << 24) | 0x0bu));
    assert(f.regs[1] == LAPIS_PMIF_CLR && f.values[1] == 1);

    f = (struct fake){ .states = { 2 }, .nstates = 1 }; io = ops(&f); out = 0xa5;
    assert(lapis_pmif_read_byte(&io, 5, 0x0b, 3, &out) == LAPIS_PMIF_IDLE_TIMEOUT);
    assert(out == 0xa5 && f.writes == 0 && f.delays == 3);

    f = (struct fake){ .states = { 12 }, .nstates = 1 }; io = ops(&f);
    assert(lapis_pmif_read_byte(&io, 5, 0x0b, 2, &out) == LAPIS_PMIF_IDLE_TIMEOUT);
    assert(f.writes == 1 && f.regs[0] == LAPIS_PMIF_CLR);

    f = (struct fake){ .states = { 0, 2 }, .nstates = 2 }; io = ops(&f); out = 0xa5;
    assert(lapis_pmif_read_byte(&io, 5, 0x0b, 2, &out) == LAPIS_PMIF_READ_TIMEOUT);
    assert(out == 0xa5 && f.writes == 1 && f.regs[0] == LAPIS_PMIF_ACC);

    f = (struct fake){ .states = { 0, 2, 0 }, .nstates = 3 }; io = ops(&f);
    assert(lapis_pmif_write_byte(&io, 5, 0x250a, 3, 0xc1) == LAPIS_PMIF_OK);
    assert(f.writes == 2 && f.regs[0] == LAPIS_PMIF_WDATA && f.values[0] == 0xc1);
    assert(f.regs[1] == LAPIS_PMIF_ACC && f.values[1] == ((3u << 30) | (1u << 29) | (5u << 24) | 0x250au));

    f = (struct fake){ .states = { 0, 2 }, .nstates = 2 }; io = ops(&f);
    assert(lapis_pmif_write_byte(&io, 5, 0x250a, 2, 0xc1) == LAPIS_PMIF_WRITE_TIMEOUT);
    assert(f.writes == 2);
    assert(lapis_pmif_write_byte(&io, 16, 0, 2, 0) == LAPIS_PMIF_INVALID);
    assert(lapis_pmif_read_byte(&io, 5, 0x10000, 2, &out) == LAPIS_PMIF_INVALID);
    assert(lapis_pmif_read_byte(&io, 5, 0, 0, &out) == LAPIS_PMIF_INVALID);
    f = (struct fake){ .states = { 0, 12, 0, 0, 0, 12 }, .nstates = 6,
        .data = 0x40, .next_data = 0x41, .use_next_data = 1 }; io = ops(&f);
    uint8_t before = 0;
    assert(lapis_pmif_update_byte(&io, 5, 0x250a, 3, 0x01, 0x01, &before) == LAPIS_PMIF_OK);
    assert(before == 0x40 && f.writes == 6 && f.regs[2] == LAPIS_PMIF_WDATA && f.values[2] == 0x41);

    f = (struct fake){ .states = { 0, 12 }, .nstates = 2, .data = 0x41 }; io = ops(&f);
    assert(lapis_pmif_update_byte(&io, 5, 0x250a, 3, 0x01, 0x01, &before) == LAPIS_PMIF_OK);
    assert(before == 0x41 && f.writes == 2); /* no unnecessary write */

    f = (struct fake){ .states = { 0, 12, 0, 0, 0, 12 }, .nstates = 6,
        .data = 0x40, .next_data = 0x40, .use_next_data = 1 }; io = ops(&f);
    assert(lapis_pmif_update_byte(&io, 5, 0x250a, 3, 0x01, 0x01, &before) == LAPIS_PMIF_VERIFY_FAILED);
    assert(before == 0x40 && f.writes == 6);

    f = (struct fake){ .states = { 0, 12, 0, 2 }, .nstates = 4, .data = 0x40 }; io = ops(&f);
    assert(lapis_pmif_update_byte(&io, 5, 0x250a, 2, 0x01, 0x01, &before) == LAPIS_PMIF_WRITE_TIMEOUT);
    assert(before == 0x40 && f.writes == 4); /* write outcome is unknown */
    puts("PMIF read, write, masked update, readback, timeouts and stale-valid handling passed");
    return 0;
}
