/* Compiled into every game module (with DW3_MODULE defined) beside the recompiled sources. */
#include "dw3/recomp.h"

#if defined(_WIN32)
#define DW3_MODULE_EXPORT __declspec(dllexport)
#else
#define DW3_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/* Defined by a source file generated beside the recompiled code. */
extern const char dw3_module_build_id[];

Dw3Api dw3_api;

DW3_MODULE_EXPORT const RecompModuleInfo* dw3_module_init(const Dw3Api* api) {
    static const RecompModuleInfo info = {DW3_API_VERSION, dw3_module_build_id, dw3_segments, 0};
    static RecompModuleInfo filled;
    if (!api || api->version != DW3_API_VERSION) return 0;
    dw3_api = *api;
    filled = info;
    filled.segment_count = dw3_segment_count;
    return &filled;
}
