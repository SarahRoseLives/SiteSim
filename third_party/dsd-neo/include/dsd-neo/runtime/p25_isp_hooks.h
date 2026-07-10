// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * P25 ISP (Inbound Signaling Packet) TSBK hook interface.
 *
 * When opts->p25_isp_mode is set, dsd-neo interprets received TSBKs as ISP
 * messages (mobile → site) rather than OSP (site → mobile).  Consumers
 * install a callback here to receive decoded ISP events.
 *
 * Field layout notes (TIA-102.AABC-E):
 *   raw12[0]  = [lb(1)][protect(1)][opcode(6)]
 *   raw12[1]  = MFID
 *   raw12[2]  = options byte (opcode-specific)
 *   raw12[3..4] = varies by opcode (often reserved or SysID region)
 *   raw12[5..6] = 16-bit address (TGID or target for most opcodes)
 *   raw12[7..9] = 24-bit Source ID  (ISSI — present in most ISP TSBKs)
 *   raw12[10..11] = CRC-CCITT-16
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISP TSBK opcodes relevant to a P25 site simulator. */
#define P25_ISP_GRP_VCH_REQ     0x00  /* Group Voice Channel Request      */
#define P25_ISP_UU_VCH_REQ      0x03  /* Unit-to-Unit Voice Channel Req   */
#define P25_ISP_EMRG_ALRM_REQ   0x0F  /* Emergency Alarm Request          */
#define P25_ISP_ACK_RSP_MO      0x1F  /* Acknowledge Response (mobile)    */
#define P25_ISP_GRP_AFF_Q       0x28  /* Group Affiliation Request        */
#define P25_ISP_U_REG_REQ       0x2C  /* Unit Registration Request        */
#define P25_ISP_U_DE_REG_ACK    0x2F  /* Unit De-Registration Acknowledge */

/**
 * Callback signature for ISP TSBK events.
 *
 * @param opcode   6-bit ISP opcode (P25_ISP_* constants above)
 * @param raw12    Full 12-byte TSBK (first byte still has lb/protect bits)
 * @param user     Opaque pointer set in dsd_p25_isp_hooks.user
 */
typedef void (*dsd_p25_isp_tsbk_cb)(uint8_t opcode, const uint8_t raw12[12], void* user);

typedef struct {
    dsd_p25_isp_tsbk_cb on_isp_tsbk; /* called for every decoded ISP TSBK */
    void*               user;         /* opaque context forwarded to callback */
} dsd_p25_isp_hooks;

/** Install (or replace) ISP hooks.  Thread-safe: copy is atomic at struct level. */
void dsd_p25_isp_hooks_set(dsd_p25_isp_hooks hooks);

/** Internal — called from p25p1_tsbk_isp.c to fire the installed callback. */
void dsd_p25_isp_hook_on_tsbk(uint8_t opcode, const uint8_t raw12[12]);

#ifdef __cplusplus
}
#endif
