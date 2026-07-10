#pragma once
#include <stdint.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode and dispatch an ISP TSBK received on the site input channel.
 * Called from processTSBK() when opts->p25_isp_mode != 0.
 */
void processTSBK_ISP(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[12]);

#ifdef __cplusplus
}
#endif
