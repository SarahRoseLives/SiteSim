// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/runtime/p25_isp_hooks.h>
#include <string.h>

static dsd_p25_isp_hooks g_p25_isp_hooks = {0};

void
dsd_p25_isp_hooks_set(dsd_p25_isp_hooks hooks)
{
    g_p25_isp_hooks = hooks;
}

void
dsd_p25_isp_hook_on_tsbk(uint8_t opcode, const uint8_t raw12[12])
{
    if (!g_p25_isp_hooks.on_isp_tsbk)
        return;
    g_p25_isp_hooks.on_isp_tsbk(opcode, raw12, g_p25_isp_hooks.user);
}
