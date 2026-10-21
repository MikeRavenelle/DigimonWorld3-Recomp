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
    void* bus;           /* opaque dw3::Bus* (memory slow path) */
    void* machine;       /* opaque dw3::Machine* (everything else); may be null in unit tests */

    /* Save states (see machine.h). cur_pc is the label the code is about to jump to, recorded at every event check:
     * the guest PC a state resumes at. resume is non-zero while a restored state is entered: the label address the
     * function being entered jumps to. */
    uint32_t cur_pc;
    uint32_t resume;
} RecompContext;

#if defined(__GNUC__) || defined(__clang__)
#define DW3_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define DW3_UNLIKELY(x) (x)
#endif

typedef void (*RecompFunc)(RecompContext* ctx);

/* Runtime services the recompiled code calls. Linked into the executable normally; a game module (DW3_MODULE) is a
 * separate library that receives them through a Dw3Api table at load, so it needs no import libraries and loads the
 * same way on every OS. */
#define DW3_API_VERSION 1u

typedef struct Dw3Api {
    uint32_t version;
    uint32_t (*mem_read)(RecompContext* ctx, uint32_t addr, int size);
    void (*mem_write)(RecompContext* ctx, uint32_t addr, uint32_t value, int size);
    RecompFunc (*lookup_func)(RecompContext* ctx, uint32_t addr);
    void (*service_events)(RecompContext* ctx);
    void (*syscall)(RecompContext* ctx, uint32_t pc);
    void (*brk)(RecompContext* ctx, uint32_t pc);
    void (*bad_jump)(RecompContext* ctx, uint32_t pc, uint32_t target);
    void (*bios_call)(RecompContext* ctx, uint32_t table, uint32_t fn);
    void (*bad_resume)(RecompContext* ctx, uint32_t fn);
    void (*mtc0)(RecompContext* ctx, uint32_t reg, uint32_t value);
    void (*rfe)(RecompContext* ctx);
    uint32_t (*gte_read_data)(RecompContext* ctx, uint32_t reg);
    void (*gte_write_data)(RecompContext* ctx, uint32_t reg, uint32_t value);
    uint32_t (*gte_read_ctrl)(RecompContext* ctx, uint32_t reg);
    void (*gte_write_ctrl)(RecompContext* ctx, uint32_t reg, uint32_t value);
    void (*gte_cmd)(RecompContext* ctx, uint32_t cmd);
} Dw3Api;

#ifdef DW3_MODULE
extern Dw3Api dw3_api;
#define dw3_mem_read (dw3_api.mem_read)
#define dw3_mem_write (dw3_api.mem_write)
#define dw3_lookup_func (dw3_api.lookup_func)
#define dw3_service_events (dw3_api.service_events)
#define dw3_syscall (dw3_api.syscall)
#define dw3_break (dw3_api.brk)
#define dw3_bad_jump (dw3_api.bad_jump)
#define dw3_bios_call (dw3_api.bios_call)
#define dw3_bad_resume (dw3_api.bad_resume)
#define dw3_mtc0 (dw3_api.mtc0)
#define dw3_rfe (dw3_api.rfe)
#define dw3_gte_read_data (dw3_api.gte_read_data)
#define dw3_gte_write_data (dw3_api.gte_write_data)
#define dw3_gte_read_ctrl (dw3_api.gte_read_ctrl)
#define dw3_gte_write_ctrl (dw3_api.gte_write_ctrl)
#define dw3_gte_cmd (dw3_api.gte_cmd)
#else
uint32_t dw3_mem_read(RecompContext* ctx, uint32_t addr, int size);
void dw3_mem_write(RecompContext* ctx, uint32_t addr, uint32_t value, int size);
RecompFunc dw3_lookup_func(RecompContext* ctx, uint32_t addr);
void dw3_service_events(RecompContext* ctx);
void dw3_syscall(RecompContext* ctx, uint32_t pc);
void dw3_break(RecompContext* ctx, uint32_t pc);
void dw3_bad_jump(RecompContext* ctx, uint32_t pc, uint32_t target);
void dw3_bios_call(RecompContext* ctx, uint32_t table, uint32_t fn);
void dw3_bad_resume(RecompContext* ctx, uint32_t fn);
void dw3_mtc0(RecompContext* ctx, uint32_t reg, uint32_t value);
void dw3_rfe(RecompContext* ctx);
uint32_t dw3_gte_read_data(RecompContext* ctx, uint32_t reg);
void dw3_gte_write_data(RecompContext* ctx, uint32_t reg, uint32_t value);
uint32_t dw3_gte_read_ctrl(RecompContext* ctx, uint32_t reg);
void dw3_gte_write_ctrl(RecompContext* ctx, uint32_t reg, uint32_t value);
void dw3_gte_cmd(RecompContext* ctx, uint32_t cmd);
#endif

/* Slow-path memory access for anything outside RAM/scratchpad (I/O, BIOS, expansion). */

/* Resolves an indirect jump/call target given the currently loaded overlays. Aborts with a
 * diagnostic if the target was not recompiled (pure static recomp: no fallback). */

/* Called at block boundaries when ctx->cycles >= ctx->next_event (IRQs, VSync, DMA, CD). */


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

/* ---- Control flow ---- */

/* Calls the function at `addr` through the lookup table (indirect calls, calls into overlay slots). */
static inline void dw3_call(RecompContext* ctx, uint32_t addr) { dw3_lookup_func(ctx, addr)(ctx); }

/* A switch() over a recovered jump table hit an unexpected value. Fatal with diagnostics. */

/* BIOS A0/B0/C0 dispatch (native HLE). `fn` is the function number from $t1. */

/* Event check on a backward jump to label `pc`. The label is where a save state resumes. */
#define DW3_CHECK_EVENTS(ctx, pc)                     \
    do {                                              \
        if ((ctx)->cycles >= (ctx)->next_event) {     \
            (ctx)->cur_pc = (pc);                     \
            dw3_service_events(ctx);                  \
        }                                             \
    } while (0)

/* Entered function found no label for the address a restored chain asked it to resume at. */

/* ---- Coprocessors ---- */

/* ---- Arithmetic with MIPS-exact edge cases ---- */
static inline void dw3_div(RecompContext* ctx, uint32_t rs, uint32_t rt) {
    int32_t n = (int32_t)rs, d = (int32_t)rt;
    if (d == 0) {
        ctx->lo = n >= 0 ? 0xFFFFFFFFu : 1u;
        ctx->hi = rs;
    } else if (rs == 0x80000000u && d == -1) {
        ctx->lo = 0x80000000u;
        ctx->hi = 0;
    } else {
        ctx->lo = (uint32_t)(n / d);
        ctx->hi = (uint32_t)(n % d);
    }
}

static inline void dw3_divu(RecompContext* ctx, uint32_t rs, uint32_t rt) {
    if (rt == 0) {
        ctx->lo = 0xFFFFFFFFu;
        ctx->hi = rs;
    } else {
        ctx->lo = rs / rt;
        ctx->hi = rs % rt;
    }
}

static inline void dw3_mult(RecompContext* ctx, uint32_t rs, uint32_t rt) {
    int64_t p = (int64_t)(int32_t)rs * (int64_t)(int32_t)rt;
    ctx->lo = (uint32_t)p;
    ctx->hi = (uint32_t)((uint64_t)p >> 32);
}

static inline void dw3_multu(RecompContext* ctx, uint32_t rs, uint32_t rt) {
    uint64_t p = (uint64_t)rs * (uint64_t)rt;
    ctx->lo = (uint32_t)p;
    ctx->hi = (uint32_t)(p >> 32);
}

/* ---- Unaligned loads/stores (little-endian R3000 semantics) ---- */
static inline uint32_t dw3_lwl(RecompContext* ctx, uint32_t addr, uint32_t old) {
    uint32_t w = dw3_read32(ctx, addr & ~3u);
    switch (addr & 3) {
        case 0: return (old & 0x00FFFFFFu) | (w << 24);
        case 1: return (old & 0x0000FFFFu) | (w << 16);
        case 2: return (old & 0x000000FFu) | (w << 8);
        default: return w;
    }
}

static inline uint32_t dw3_lwr(RecompContext* ctx, uint32_t addr, uint32_t old) {
    uint32_t w = dw3_read32(ctx, addr & ~3u);
    switch (addr & 3) {
        case 0: return w;
        case 1: return (old & 0xFF000000u) | (w >> 8);
        case 2: return (old & 0xFFFF0000u) | (w >> 16);
        default: return (old & 0xFFFFFF00u) | (w >> 24);
    }
}

static inline void dw3_swl(RecompContext* ctx, uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, m = dw3_read32(ctx, a);
    switch (addr & 3) {
        case 0: m = (m & 0xFFFFFF00u) | (v >> 24); break;
        case 1: m = (m & 0xFFFF0000u) | (v >> 16); break;
        case 2: m = (m & 0xFF000000u) | (v >> 8); break;
        default: m = v; break;
    }
    dw3_write32(ctx, a, m);
}

static inline void dw3_swr(RecompContext* ctx, uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, m = dw3_read32(ctx, a);
    switch (addr & 3) {
        case 0: m = v; break;
        case 1: m = (m & 0x000000FFu) | (v << 8); break;
        case 2: m = (m & 0x0000FFFFu) | (v << 16); break;
        default: m = (m & 0x00FFFFFFu) | (v << 24); break;
    }
    dw3_write32(ctx, a, m);
}

/* ---- Function tables emitted by psxrecomp ---- */
typedef struct RecompFuncEntry {
    uint32_t addr;
    RecompFunc func;
} RecompFuncEntry;

/* Code words sampled from a segment's image. Overlay slots are identified by comparing these
 * against RAM: code is never modified after load, unlike the overlay's data. */
typedef struct RecompProbe {
    uint32_t addr;
    uint32_t word;
} RecompProbe;

typedef struct RecompRange {
    uint32_t start, end; /* [start, end) of recompiled code */
} RecompRange;

/* A label a function can be entered at when a save state resumes: the return point of a call or the target of a
 * backward jump. Sorted by addr. */
typedef struct RecompResumeEntry {
    uint32_t addr;
    RecompFunc func;
} RecompResumeEntry;

typedef struct RecompSegment {
    const char* name;
    uint32_t base;
    uint32_t size;      /* bytes of the original image */
    uint32_t checksum;  /* FNV-1a of the original image */
    const RecompFuncEntry* funcs; /* sorted by addr */
    uint32_t func_count;
    const RecompProbe* probes;  /* words that tell this overlay apart from the others in its slot */
    uint32_t probe_count;
    const RecompRange* ranges;  /* recompiled code, for full verification of an overlay */
    uint32_t range_count;
    uint32_t code_hash;         /* FNV-1a over the bytes of `ranges` */
    const uint16_t* stale_if;   /* segment indices whose code in RAM shows this overlay is only a leftover tail */
    uint32_t stale_count;
    const RecompResumeEntry* resume; /* resume labels of this segment's functions, ended by an entry with addr 0 */
} RecompSegment;

/* Emitted by psxrecomp into dw3_tables.c. Segment 0 is the main executable. */
extern const RecompSegment dw3_segments[];
extern const uint32_t dw3_segment_count;

/* What a game module hands back from dw3_module_init(). `build_id` identifies the recompiler output the module was
 * built from; the host compares it with the one it expects. */
typedef struct RecompModuleInfo {
    uint32_t api_version;
    const char* build_id;
    const RecompSegment* segments;
    uint32_t segment_count;
} RecompModuleInfo;

typedef const RecompModuleInfo* (*RecompModuleInit)(const Dw3Api* api);
#define DW3_MODULE_INIT_SYMBOL "dw3_module_init"

#ifdef __cplusplus
}
#endif

#endif /* DW3_RECOMP_H */
