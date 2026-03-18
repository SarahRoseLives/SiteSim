// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * P25 Phase 1 ISP TSBK decoder.
 *
 * Handles TSBKs received on the site input channel (mobile → site).
 * Field offsets follow TIA-102.AABC-E; positions held constant across opcodes:
 *   byte[2]        options / flags
 *   byte[5..6]     16-bit TGID (or 16 lsb of target address)
 *   byte[7..9]     24-bit Source ISSI
 */
#include "p25p1_tsbk_isp.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/p25_isp_hooks.h>

#include <stdio.h>
#include <stdint.h>

/* Convenience macros for extracting common ISP fields. */
#define ISP_OPTIONS(b)   ((b)[2])
#define ISP_TGID(b)      (((uint32_t)(b)[5] << 8) | (b)[6])
#define ISP_SRC_ID(b)    (((uint32_t)(b)[7] << 16) | ((uint32_t)(b)[8] << 8) | (b)[9])
#define ISP_TARGET24(b)  (((uint32_t)(b)[4] << 16) | ((uint32_t)(b)[5] << 8) | (b)[6])

void
processTSBK_ISP(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[12])
{
    (void)opts;
    (void)state;

    uint8_t opcode = tsbk_byte[0] & 0x3F;
    uint8_t options = ISP_OPTIONS(tsbk_byte);

    /* Fire the generic hook first — lets SiteSim do its own interpretation. */
    dsd_p25_isp_hook_on_tsbk(opcode, tsbk_byte);

    fprintf(stderr, "%s", KYEL);
    fprintf(stderr, "\n P25 ISP TSBK [opcode 0x%02X]", opcode);

    switch (opcode) {

    case P25_ISP_GRP_VCH_REQ: {
        /* Group Voice Channel Request — mobile wants a voice channel. */
        uint32_t tgid   = ISP_TGID(tsbk_byte);
        uint32_t src_id = ISP_SRC_ID(tsbk_byte);
        int emrg  = (options >> 7) & 1;
        int enc   = (options >> 6) & 1;
        int prio  =  options & 0x07;
        fprintf(stderr, " GRP_VCH_REQ\n");
        fprintf(stderr, "  TGID: %-5u  SRC: %-8u  EMRG: %d  ENC: %d  PRIO: %d\n",
                tgid, src_id, emrg, enc, prio);
        break;
    }

    case P25_ISP_UU_VCH_REQ: {
        /* Unit-to-Unit Voice Channel Request. */
        uint32_t target = ISP_TARGET24(tsbk_byte);
        uint32_t src_id = ISP_SRC_ID(tsbk_byte);
        int emrg = (options >> 7) & 1;
        fprintf(stderr, " UU_VCH_REQ\n");
        fprintf(stderr, "  TARGET: %-8u  SRC: %-8u  EMRG: %d\n", target, src_id, emrg);
        break;
    }

    case P25_ISP_EMRG_ALRM_REQ: {
        /* Emergency Alarm Request. */
        uint32_t tgid   = ISP_TGID(tsbk_byte);
        uint32_t src_id = ISP_SRC_ID(tsbk_byte);
        fprintf(stderr, " EMRG_ALRM_REQ *** EMERGENCY ***\n");
        fprintf(stderr, "  TGID: %-5u  SRC: %-8u\n", tgid, src_id);
        break;
    }

    case P25_ISP_ACK_RSP_MO: {
        /* Acknowledge Response (Mobile Originated). */
        uint32_t src_id    = ISP_SRC_ID(tsbk_byte);
        uint8_t  ack_opcode = tsbk_byte[3]; /* opcode being acknowledged */
        fprintf(stderr, " ACK_RSP_MO\n");
        fprintf(stderr, "  SRC: %-8u  ACK for opcode: 0x%02X\n", src_id, ack_opcode);
        break;
    }

    case P25_ISP_GRP_AFF_Q: {
        /* Group Affiliation Request — mobile wants to affiliate to a talkgroup. */
        uint32_t tgid   = ISP_TGID(tsbk_byte);
        uint32_t src_id = ISP_SRC_ID(tsbk_byte);
        int lg = (options >> 7) & 1; /* Local Group flag */
        fprintf(stderr, " GRP_AFF_REQ\n");
        fprintf(stderr, "  TGID: %-5u  SRC: %-8u  LG: %d\n", tgid, src_id, lg);
        break;
    }

    case P25_ISP_U_REG_REQ: {
        /* Unit Registration Request — mobile registering onto the site. */
        uint32_t src_id  = ISP_SRC_ID(tsbk_byte);
        /* SysID is 12 bits starting at bits 24-35 (bytes 3 high-nybble + byte 4) */
        uint16_t sys_id = ((uint16_t)(tsbk_byte[3] & 0x0F) << 8) | tsbk_byte[4];
        fprintf(stderr, " U_REG_REQ\n");
        fprintf(stderr, "  SRC: %-8u  SysID: 0x%03X\n", src_id, sys_id);
        break;
    }

    case P25_ISP_U_DE_REG_ACK: {
        /* Unit De-Registration Acknowledge — mobile deregistering. */
        uint32_t src_id = ISP_SRC_ID(tsbk_byte);
        fprintf(stderr, " U_DE_REG_ACK\n");
        fprintf(stderr, "  SRC: %-8u\n", src_id);
        break;
    }

    default:
        /* Unknown or unhandled ISP opcode — dump raw bytes for analysis. */
        fprintf(stderr, " (unhandled)\n");
        fprintf(stderr, "  raw: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                tsbk_byte[0], tsbk_byte[1], tsbk_byte[2], tsbk_byte[3],
                tsbk_byte[4], tsbk_byte[5], tsbk_byte[6], tsbk_byte[7],
                tsbk_byte[8], tsbk_byte[9]);
        break;
    }

    fprintf(stderr, "%s", KNRM);
}
