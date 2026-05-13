// SPDX-License-Identifier: GPL-2.0-or-later
/* HMUARTLGW Frame Codec — DualCoPro-kompatibel
 *
 * Frame-Layout (auf-dem-Wire, vor Escape):
 *   [0xfd] [len_hi] [len_lo] [dst] [cnt] [payload...] [crc_hi] [crc_lo]
 *
 *   length      = (dst + cnt + payload) bytes, ohne CRC
 *   CRC16       = poly 0x8005, init 0xd77f, big-endian,
 *                 computed über [0xfd, len_hi, len_lo, dst, cnt, payload]
 *   Escape      = 0xfc und 0xfd im Body durch [0xfc, b XOR 0x80] ersetzt
 *                 (Magic-0xfd am Frame-Anfang ist NICHT escaped)
 *
 * dst-Werte (DualCoPro):
 *   0x00 OS / HMSYSTEM (Legacy-Pfad)
 *   0x01 APP / TRX
 *   0x02 HMIP   (DualCoPro)
 *   0x03 LLMAC  (DualCoPro BidCoS-MAC)
 *   0xfe COMMON (DualCoPro identify)
 *   0xff DUAL_ERR
 *
 * Lizenz: GPL-2.0-or-later (CUL32-HM-Projekt). Spec aus CULFW32 +
 * piVCCU GPLv2-Sourcen + empirisch verifizierte strace-Captures.
 */

#ifndef CUL32HM_FRAME_H
#define CUL32HM_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HMU_MAGIC          0xfd
#define HMU_ESCAPE         0xfc
#define HMU_ESCAPE_MASK    0x80

#define HMU_DST_OS         0x00
#define HMU_DST_APP        0x01
#define HMU_DST_HMIP       0x02
#define HMU_DST_LLMAC      0x03
#define HMU_DST_COMMON     0xfe
#define HMU_DST_DUAL_ERR   0xff

#define HMU_CRC_POLY       0x8005
#define HMU_CRC_INIT       0xd77f

/* 2048 = MAX_DATA_BYTES aus der Java-Spec FirmwareUpdateFrame.java
 * (das deckt 1042-byte chunks für HM-MOD-UART und 562-byte chunks für
 * DualCoPro-RFUSB/RPI-RF-MOD, plus Headroom). */
#define HMU_MAX_PAYLOAD    2048
#define HMU_MAX_FRAME      (1 + 2 + 2 + HMU_MAX_PAYLOAD + 2)
#define HMU_MAX_FRAME_ESC  (1 + 2 * (2 + 2 + HMU_MAX_PAYLOAD + 2))

/* CRC-Berechnung über roh-bytes, ohne Escape. */
uint16_t hmu_crc16(const uint8_t *data, size_t len);

/* Encode: payload → on-wire-Bytes (mit Magic, Length, CRC, Escape).
 * out muss mind. HMU_MAX_FRAME_ESC Bytes haben.
 * Returns: encoded length (>=8), oder -1 bei error. */
int hmu_frame_encode(uint8_t dst, uint8_t cnt,
                     const uint8_t *payload, size_t payload_len,
                     uint8_t *out, size_t out_cap);

/* Streaming-Decoder. Bytes werden incremental gefüttert; sobald ein
 * vollständiger Frame validiert ist, wird der Callback aufgerufen.
 * State-Machine handhabt Escape- und CRC-Mismatch (resync auf nächstes 0xfd). */
typedef void (*hmu_frame_cb)(void *ctx,
                             uint8_t dst, uint8_t cnt,
                             const uint8_t *payload, size_t payload_len);

typedef enum {
    HMU_DEC_HUNT_MAGIC,
    HMU_DEC_LEN_HI,
    HMU_DEC_LEN_LO,
    HMU_DEC_BODY,
    HMU_DEC_CRC_HI,
    HMU_DEC_CRC_LO,
} hmu_dec_state_t;

typedef struct {
    hmu_dec_state_t state;
    bool            esc_pending;
    uint16_t        length;
    uint16_t        body_idx;
    uint8_t         body[2 + HMU_MAX_PAYLOAD];  /* dst+cnt+payload */
    uint8_t         crc_hi;
    hmu_frame_cb    cb;
    void           *ctx;
    /* Stats */
    uint32_t        frames_ok;
    uint32_t        frames_crc_err;
    uint32_t        frames_truncated;
    uint32_t        bytes_skipped;
} hmu_decoder_t;

void hmu_decoder_init(hmu_decoder_t *d, hmu_frame_cb cb, void *ctx);
void hmu_decoder_feed(hmu_decoder_t *d, const uint8_t *data, size_t len);

#endif
