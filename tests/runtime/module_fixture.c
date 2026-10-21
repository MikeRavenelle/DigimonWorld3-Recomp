/* A tiny game module for the loader tests: one segment with one function that goes through the runtime services. */
#include "dw3/recomp.h"

void f_FIX_80010000(RecompContext* ctx) {
    dw3_mem_write(ctx, 0x1F801074u, 5u, 4); /* I_MASK */
    ctx->r[2] = dw3_mem_read(ctx, 0x1F801074u, 4);
}

static const RecompFuncEntry kFuncs[] = {{0x80010000u, f_FIX_80010000}};

const RecompSegment dw3_segments[1] = {{"FIX", 0x80010000u, 0x100u, 0u, kFuncs, 1u, 0, 0u, 0, 0u, 0u, 0, 0u, 0}};
const uint32_t dw3_segment_count = 1;

const char dw3_module_build_id[] = "fixture";
