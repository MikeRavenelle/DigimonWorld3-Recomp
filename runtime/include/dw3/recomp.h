/* Interface between generated (recompiled) C code and the runtime.
 * Plain C so generated translation units compile fast and stay ABI-stable. */
#ifndef DW3_RECOMP_H
#define DW3_RECOMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompContext {
    uint32_t r[32]; /* GPRs; r[0] is forced to zero by generated code */
    uint32_t hi, lo;
    uint32_t cop0[32];
    uint8_t* ram;        /* 2 MiB main RAM (host pointer) */
    uint8_t* scratchpad; /* 1 KiB D-cache scratchpad */
    int64_t cycles;      /* emulated CPU cycles elapsed */
    int64_t next_event;  /* runtime services events when cycles >= next_event */
    void* runtime;       /* opaque dw3::Runtime* */
} RecompContext;

typedef void (*RecompFunc)(RecompContext* ctx);

/* Slow-path memory access for anything outside RAM/scratchpad (I/O, BIOS, expansion). */
uint32_t dw3_mem_read(RecompContext* ctx, uint32_t addr, int size);
void dw3_mem_write(RecompContext* ctx, uint32_t addr, uint32_t value, int size);

/* Resolves an indirect jump/call target given the currently loaded overlays. Aborts with a
 * diagnostic if the target was not recompiled (pure static recomp: no fallback). */
RecompFunc dw3_lookup_func(RecompContext* ctx, uint32_t addr);

/* Called at block boundaries when ctx->cycles >= ctx->next_event (IRQs, VSync, DMA, CD). */
void dw3_service_events(RecompContext* ctx);

void dw3_syscall(RecompContext* ctx, uint32_t pc);
void dw3_break(RecompContext* ctx, uint32_t pc);

#define DW3_RAM_SIZE        0x200000u
#define DW3_SCRATCHPAD_BASE 0x1F800000u
#define DW3_SCRATCHPAD_SIZE 0x400u

/* Strip KSEG0/KSEG1 segment bits: 0x80xxxxxx / 0xA0xxxxxx -> physical. */
static inline uint32_t dw3_phys(uint32_t addr) {
    static const uint32_t seg_mask[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                         0x7FFFFFFFu, 0x1FFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    return addr & seg_mask[addr >> 29];
}

static inline uint32_t dw3_read32(RecompContext* ctx, uint32_t addr) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) { /* RAM mirrored 4x across 8 MiB */
        uint32_t v;
        __builtin_memcpy(&v, ctx->ram + (p & (DW3_RAM_SIZE - 1)), 4);
        return v;
    }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        uint32_t v;
        __builtin_memcpy(&v, ctx->scratchpad + (p & (DW3_SCRATCHPAD_SIZE - 1)), 4);
        return v;
    }
    return dw3_mem_read(ctx, addr, 4);
}

static inline uint16_t dw3_read16(RecompContext* ctx, uint32_t addr) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) {
        uint16_t v;
        __builtin_memcpy(&v, ctx->ram + (p & (DW3_RAM_SIZE - 1)), 2);
        return v;
    }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        uint16_t v;
        __builtin_memcpy(&v, ctx->scratchpad + (p & (DW3_SCRATCHPAD_SIZE - 1)), 2);
        return v;
    }
    return (uint16_t)dw3_mem_read(ctx, addr, 2);
}

static inline uint8_t dw3_read8(RecompContext* ctx, uint32_t addr) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) return ctx->ram[p & (DW3_RAM_SIZE - 1)];
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) return ctx->scratchpad[p & (DW3_SCRATCHPAD_SIZE - 1)];
    return (uint8_t)dw3_mem_read(ctx, addr, 1);
}

static inline void dw3_write32(RecompContext* ctx, uint32_t addr, uint32_t v) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) { __builtin_memcpy(ctx->ram + (p & (DW3_RAM_SIZE - 1)), &v, 4); return; }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        __builtin_memcpy(ctx->scratchpad + (p & (DW3_SCRATCHPAD_SIZE - 1)), &v, 4);
        return;
    }
    dw3_mem_write(ctx, addr, v, 4);
}

static inline void dw3_write16(RecompContext* ctx, uint32_t addr, uint16_t v) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) { __builtin_memcpy(ctx->ram + (p & (DW3_RAM_SIZE - 1)), &v, 2); return; }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        __builtin_memcpy(ctx->scratchpad + (p & (DW3_SCRATCHPAD_SIZE - 1)), &v, 2);
        return;
    }
    dw3_mem_write(ctx, addr, v, 2);
}

static inline void dw3_write8(RecompContext* ctx, uint32_t addr, uint8_t v) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) { ctx->ram[p & (DW3_RAM_SIZE - 1)] = v; return; }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        ctx->scratchpad[p & (DW3_SCRATCHPAD_SIZE - 1)] = v;
        return;
    }
    dw3_mem_write(ctx, addr, v, 1);
}

#ifdef __cplusplus
}
#endif

#endif /* DW3_RECOMP_H */
