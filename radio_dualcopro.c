// SPDX-License-Identifier: GPL-2.0-or-later
/* radio_dualcopro.c — DualCoPro-Funkmodul-Treiber
 *
 * Deckt: HM-MOD-RPI-PCB, HmIP-RFUSB (alle Varianten), RPI-RF-MOD und
 * andere Module die das DualCoPro-Frame-Format mit 0xfd-Magic +
 * COMMON_IDENTIFY-Probe sprechen.
 *
 * Boot-Sequenz (empirisch verifiziert via captures/multimacd_hmip_rfusb_*):
 *   1. Drain Boot-Banner (~600ms)
 *   2. COMMON_IDENTIFY (dst=0xfe cmd=0x01) — kanonischer DualCoPro-Probe
 *      (Fallback OS_GET_APP für Legacy-Module die kein COMMON kennen)
 *   3. Reply auswerten:
 *        DUAL_ERR     → Modul ist schon im App-Mode, fertig
 *        Tag '_App'   → App-Mode, fertig
 *        Tag '_Bl|BL' → Bootloader, weiter
 *   4. CHANGE_APP via dst=COMMON cmd=0x03 (HmIP-RFUSB-BL akzeptiert nur
 *      via dst=COMMON; HM-MOD-RPI-PCB beides)
 *   5. ~700ms warten, COMMON-Push mit App-Tag liest
 *   6. Drain Rest
 *
 * caps_actual aus app_tag-Pattern abgeleitet:
 *    "DualCoPro_App"   → BidCoS+HmIP+DualCoPro
 *    "Co_CPU_App"      → BidCoS+DualCoPro (Legacy)
 *    "HMIP_TRX_App"    → BidCoS+HmIP+DualCoPro (HmIP-RFUSB-Familie)
 *    sonst             → DualCoPro nur
 *
 * v0.3.0-alpha Phase 2: Boot-Logik vollständig aus concentrator.c migriert.
 * Phase 3: on_frame_from_module wird ausgebaut für Multi-Backend-Routing.
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "radio.h"
#include "radio_dualcopro.h"
#include "backend.h"
#include "frame.h"
#include "hardware.h"
#include "transport.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

/* ───── App-Tag Klassifikation ────────────────────────────────────── */

static radio_caps_t caps_from_app_tag(const char *tag)
{
    if (!tag) return RADIO_CAP_DUALCOPRO;
    if (!strcmp(tag, "DualCoPro_App"))
        return RADIO_CAP_DUALCOPRO | RADIO_CAP_BIDCOS_RX | RADIO_CAP_BIDCOS_TX
             | RADIO_CAP_HMIP_RX | RADIO_CAP_HMIP_TX;
    if (!strcmp(tag, "Co_CPU_App"))
        return RADIO_CAP_DUALCOPRO | RADIO_CAP_BIDCOS_RX | RADIO_CAP_BIDCOS_TX;
    if (strstr(tag, "HMIP_TRX"))
        return RADIO_CAP_DUALCOPRO | RADIO_CAP_BIDCOS_RX | RADIO_CAP_BIDCOS_TX
             | RADIO_CAP_HMIP_RX | RADIO_CAP_HMIP_TX;
    /* DualCoPro_App (DUAL_ERR) — Modul ist im App, antwortet aber DUAL_ERR
     * auf OS-Anfrage. Voll-capable. */
    if (strstr(tag, "DualCoPro"))
        return RADIO_CAP_DUALCOPRO | RADIO_CAP_BIDCOS_RX | RADIO_CAP_BIDCOS_TX
             | RADIO_CAP_HMIP_RX | RADIO_CAP_HMIP_TX;
    return RADIO_CAP_DUALCOPRO;
}

/* ───── Boot-State (intern) ─────────────────────────────────────────── */

struct boot_state {
    enum { BOOT_NONE, BOOT_BL, BOOT_APP } got;
    char  app_tag[32];
    /* Welcher dst lieferte den BL-Tag? Bestimmt CHANGE_APP-Layer:
     *   HMU_DST_OS    (0x00) → HM-MOD-RPI-PCB / RPI-RF-MOD: OS-CHANGE_APP
     *   HMU_DST_COMMON(0xfe) → HmIP-RFUSB:                   COMMON-CHANGE_APP
     * Empirisch belegt: capture multimacd_hmip_rfusb_20260503/ANALYSIS.md */
    uint8_t bl_dst;
};

static void boot_on_frame(void *ctx, uint8_t dst, uint8_t cnt,
                          const uint8_t *p, size_t plen)
{
    (void)cnt;
    struct boot_state *bs = ctx;

    /* DUAL_ERR auf OS_GET_APP = Modul ist schon in DualCoPro_App und
     * weigert sich, OS-Layer-Anfragen anzunehmen. Kein CHANGE_APP nötig. */
    if (dst == HMU_DST_DUAL_ERR) {
        bs->got = BOOT_APP;
        snprintf(bs->app_tag, sizeof(bs->app_tag), "DualCoPro_App (DUAL_ERR)");
        return;
    }

    if (plen < 2) return;

    /* Vier Quellen für den App-Tag (alle empirisch belegt):
     *  - OS reply auf GET_APP:        dst=0x00, cmd=0x04 ACK_INFO, info=0x02, tag
     *  - alternativ Legacy:           dst=0x00, cmd=0x00, tag
     *  - COMMON-Push nach Boot:       dst=0xfe, cmd=0x00, tag
     *  - COMMON reply auf IDENTIFY:   dst=0xfe, cmd=0x05 ACK_INFO, info=0x01, tag
     *                                 (HmIP-RFUSB Bootloader; Capture
     *                                 captures/multimacd_hmip_rfusb_20260503_...) */
    const uint8_t *tag_p = NULL;
    size_t taglen = 0;
    if (dst == HMU_DST_OS && p[0] == 0x04 && plen >= 3 && p[1] == 0x02) {
        tag_p  = p + 2; taglen = plen - 2;
    } else if (dst == HMU_DST_OS && p[0] == 0x00 && plen >= 2) {
        tag_p  = p + 1; taglen = plen - 1;
    } else if (dst == HMU_DST_COMMON && p[0] == 0x00 && plen >= 2) {
        tag_p  = p + 1; taglen = plen - 1;
    } else if (dst == HMU_DST_COMMON && p[0] == 0x05 && plen >= 3 && p[1] == 0x01) {
        tag_p  = p + 2; taglen = plen - 2;
    } else {
        return;
    }
    bs->bl_dst = dst;

    if (taglen >= sizeof(bs->app_tag)) taglen = sizeof(bs->app_tag) - 1;
    memcpy(bs->app_tag, tag_p, taglen);
    bs->app_tag[taglen] = 0;

    /* Klassifikation primär per bekanntem Tag, dann per Suffix.
     * Suffix-Match deckt HmIP-RFUSB ab (HMIP_TRX_Bl / HMIP_TRX_App). */
    if (strcmp(bs->app_tag, "DualCoPro_App") == 0) bs->got = BOOT_APP;
    else if (strcmp(bs->app_tag, "Co_CPU_App") == 0) bs->got = BOOT_APP;
    else if (strcmp(bs->app_tag, "Co_CPU_BL")  == 0) bs->got = BOOT_BL;
    else {
        size_t tlen = strlen(bs->app_tag);
        if (tlen >= 4 && strcmp(bs->app_tag + tlen - 4, "_App") == 0)
            bs->got = BOOT_APP;
        else if (tlen >= 3 && (strcmp(bs->app_tag + tlen - 3, "_Bl") == 0
                            || strcmp(bs->app_tag + tlen - 3, "_BL") == 0))
            bs->got = BOOT_BL;
        else
            bs->got = BOOT_NONE;
    }
}

static int boot_wait_frame(int fd, hmu_decoder_t *dec, struct boot_state *bs,
                           int timeout_ms)
{
    struct timeval deadline, now, tv;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  +=  timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }

    bs->got = BOOT_NONE;
    bs->app_tag[0] = 0;
    uint8_t buf[256];

    while (bs->got == BOOT_NONE) {
        gettimeofday(&now, NULL);
        long remaining_us = (deadline.tv_sec - now.tv_sec) * 1000000L
                          + (deadline.tv_usec - now.tv_usec);
        if (remaining_us <= 0) return -1;
        tv.tv_sec  = remaining_us / 1000000;
        tv.tv_usec = remaining_us % 1000000;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        hmu_decoder_feed(dec, buf, (size_t)n);
    }
    return 0;
}

static void boot_drain(int fd, int drain_ms)
{
    struct timeval tv = { .tv_sec = drain_ms / 1000, .tv_usec = (drain_ms % 1000) * 1000 };
    uint8_t buf[256];
    for (;;) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) return;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return;
        tv.tv_sec = 0; tv.tv_usec = 100 * 1000;
    }
}

static int boot_send_get_app(int fd, uint8_t cnt)
{
    uint8_t enc[HMU_MAX_FRAME_ESC];
    uint8_t pl[1] = { 0x00 };
    int n = hmu_frame_encode(HMU_DST_OS, cnt, pl, sizeof(pl), enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

static int boot_send_common_identify(int fd, uint8_t cnt)
{
    uint8_t enc[HMU_MAX_FRAME_ESC];
    uint8_t pl[1] = { 0x01 };
    int n = hmu_frame_encode(HMU_DST_COMMON, cnt, pl, sizeof(pl), enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

static int boot_send_change_app(int fd, uint8_t cnt, uint8_t dst)
{
    /* Wire-Format dst=0xfe (COMMON) for HmIP-RFUSB, dst=0x00 (OS) for
     * HM-MOD-RPI-PCB / RPI-RF-MOD.  Payload identisch [0x03].  Beleg:
     * - HmIP-RFUSB: capture multimacd_hmip_rfusb_20260503/ANALYSIS.md
     *   "Bo 8 = fd0003fe 02031812" = dst=0xfe cnt=2 cmd=0x03
     * - HM-MOD-RPI-PCB: captures/INDEX.md
     *   "fd 00 03 00 01 03 9e 09" = dst=0x00 cnt=1 cmd=0x03 */
    uint8_t enc[HMU_MAX_FRAME_ESC];
    uint8_t pl[1] = { 0x03 };
    int n = hmu_frame_encode(dst, cnt, pl, sizeof(pl), enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

/* Timing-Werte mit Headroom für entfernte UART-Pfade (cp210x mit USB-
 * latency timer, TCP-Wrapper über LAN/WLAN). 600ms war für /dev/raw-uart
 * (hb_rf_usb_2 bulk-IN) ausreichend, hat aber bei /dev/ttyUSB0 Antwort-
 * Frames im RE-Test nach ~700-900ms verloren. Memory: cp210x_native_path.md,
 * bmcond_app_mode_probe_frames.md. */
#define BOOT_INIT_SETTLE_MS    800
#define BOOT_MAX_PROBES        6   /* 3× COMMON_IDENTIFY + 3× OS_GET_APP */
#define BOOT_PROBE_TIMEOUT_MS  1500

int radio_dualcopro_boot_to_app(int fd, char *app_tag_out, size_t app_tag_cap)
{
    struct boot_state bs;
    hmu_decoder_t dec;
    hmu_decoder_init(&dec, boot_on_frame, &bs);

    boot_drain(fd, BOOT_INIT_SETTLE_MS);

    /* Probe-Strategie: erst COMMON_IDENTIFY (kanonisch, funktioniert für
     * HmIP-RFUSB und HM-MOD-RPI-PCB), bei Stille fallback OS_GET_APP. */
    int probe = 0;
    uint8_t cnt = 0;
    while (probe < BOOT_MAX_PROBES) {
        const char *probe_name = (probe < BOOT_MAX_PROBES / 2) ? "COMMON_IDENTIFY" : "GET_APP";
        int tx_rc = (probe < BOOT_MAX_PROBES / 2)
                    ? boot_send_common_identify(fd, cnt)
                    : boot_send_get_app(fd, cnt);
        fprintf(stderr, "RADIO[dualcopro]: TX %s cnt=%d (probe %d)\n",
                probe_name, cnt, probe + 1);
        if (tx_rc < 0) {
            fprintf(stderr, "RADIO[dualcopro]: TX failed: %s\n", strerror(errno));
            return -1;
        }
        if (boot_wait_frame(fd, &dec, &bs, BOOT_PROBE_TIMEOUT_MS) == 0) break;
        probe++;
        cnt++;
    }
    if (probe >= BOOT_MAX_PROBES) {
        fprintf(stderr, "RADIO[dualcopro]: Modul stumm nach %d Versuchen\n",
                BOOT_MAX_PROBES);
        return -1;
    }
    fprintf(stderr, "RADIO[dualcopro]: Modul ist in '%s'\n", bs.app_tag);
    cnt++;

    if (bs.got == BOOT_BL) {
        /* Layer wählen je nach welchem dst den BL-Tag geliefert hat:
         *   OS-Layer-Reply  → HM-MOD-RPI-PCB / RPI-RF-MOD: OS-CHANGE_APP
         *   COMMON-Reply    → HmIP-RFUSB:                   COMMON-CHANGE_APP
         * Default-Fallback (kein bl_dst gesetzt) = COMMON, weil ältere
         * bmcd-Versionen das immer so machten. */
        uint8_t change_dst = (bs.bl_dst == HMU_DST_OS) ? HMU_DST_OS : HMU_DST_COMMON;
        const char *layer  = (change_dst == HMU_DST_OS) ? "OS" : "COMMON";
        fprintf(stderr, "RADIO[dualcopro]: TX %s-CHANGE_APP cnt=%d (BL via dst=0x%02x)\n",
                layer, cnt, bs.bl_dst);
        if (boot_send_change_app(fd, cnt, change_dst) < 0) {
            fprintf(stderr, "RADIO[dualcopro]: CHANGE_APP TX failed\n");
            return -1;
        }
        if (boot_wait_frame(fd, &dec, &bs, 1500) < 0) {
            /* Erste Variante stumm — anderen Layer als Fallback probieren.
             * Kein Modul der Welt kennt beide gleichzeitig, also die Stille
             * der einen Variante ist erwartbar wenn wir am Tag falsch lagen. */
            uint8_t alt_dst = (change_dst == HMU_DST_OS) ? HMU_DST_COMMON : HMU_DST_OS;
            const char *alt_layer = (alt_dst == HMU_DST_OS) ? "OS" : "COMMON";
            cnt++;
            fprintf(stderr, "RADIO[dualcopro]: keine Antwort — Fallback TX %s-CHANGE_APP cnt=%d\n",
                    alt_layer, cnt);
            if (boot_send_change_app(fd, cnt, alt_dst) < 0) {
                fprintf(stderr, "RADIO[dualcopro]: CHANGE_APP fallback TX failed\n");
                return -1;
            }
            if (boot_wait_frame(fd, &dec, &bs, 1500) < 0) {
                fprintf(stderr, "RADIO[dualcopro]: keine Push-Bestätigung\n");
                return -1;
            }
        }
        fprintf(stderr, "RADIO[dualcopro]: Push-Bestätigung: '%s'\n", bs.app_tag);
        boot_drain(fd, 100);
    } else if (bs.got != BOOT_APP) {
        fprintf(stderr, "RADIO[dualcopro]: unerwarteter App-Tag, abort\n");
        return -1;
    }

    if (app_tag_out && app_tag_cap > 0) {
        snprintf(app_tag_out, app_tag_cap, "%s", bs.app_tag);
    }
    return bs.got == BOOT_APP ? 0 : -1;
}

/* ───── radio_ops_t-Implementierung (vtable hooks) ──────────────────── */

static radio_boot_result_t dualcopro_boot(struct backend *be)
{
    if (!be || !be->transport) return RADIO_BOOT_FAILED;
    char tag[32];
    int rc = radio_dualcopro_boot_to_app(be->transport->fd, tag, sizeof(tag));
    if (rc < 0) return RADIO_BOOT_FAILED;
    /* App-Tag direkt in info ablegen falls Boot ohne identify aufgerufen */
    snprintf(be->info.app_tag, sizeof(be->info.app_tag), "%s", tag);
    be->info.caps_actual = caps_from_app_tag(tag);
    return RADIO_BOOT_OK;
}

static int dualcopro_identify(struct backend *be, radio_info_t *info)
{
    if (!be || !info) { errno = EINVAL; return -1; }
    /* boot() hat caps_actual und app_tag schon gesetzt — wir kopieren in
     * lokale info-struct, plus name aus IOCGDEVINFO falls verfügbar. */
    *info = be->info;

    hw_radio_info_t hw;
    if (hw_identify(be->transport->fd, be->transport->target, &hw) == 0 && hw.devinfo[0]) {
        snprintf(info->name, sizeof(info->name), "%s", hw_kind_name(hw.kind));
    } else {
        snprintf(info->name, sizeof(info->name), "dualcopro-generic");
    }
    return 0;
}

static int dualcopro_hw_reset(struct backend *be)
{
    if (!be || !be->transport) { errno = EINVAL; return -1; }
    return hw_reset_radio(be->transport->fd);
}

static void dualcopro_on_frame(struct backend *be,
                               const uint8_t *raw, size_t raw_len)
{
    /* Phase 3 wird das vollständige Routing aus concentrator.c
     * on_uart_frame() hierhin migrieren. */
    (void)be; (void)raw; (void)raw_len;
}

const radio_ops_t radio_dualcopro = {
    .name             = "dualcopro",
    .caps_advertised  = RADIO_CAP_DUALCOPRO
                      | RADIO_CAP_BIDCOS_RX | RADIO_CAP_BIDCOS_TX
                      | RADIO_CAP_HMIP_RX   | RADIO_CAP_HMIP_TX,
    .boot                  = dualcopro_boot,
    .identify              = dualcopro_identify,
    .hw_reset              = dualcopro_hw_reset,
    .on_frame_from_module  = dualcopro_on_frame,
};

radio_caps_t radio_dualcopro_caps_from_tag(const char *tag);
radio_caps_t radio_dualcopro_caps_from_tag(const char *tag)
{
    return caps_from_app_tag(tag);
}
