// SPDX-License-Identifier: GPL-2.0-or-later
/* hmip_probe — empirischer USB-Layer-Test ob ein DualCoPro-Stick
 * tatsächlich HmIP-Hardware mitbringt oder nur das DualCoPro_App-
 * Firmware-Tag trägt.
 *
 * Hintergrund: HM-MOD-RPI-PCB (CC1101-only, kein HmIP-Radio) und
 * HmIP-RFUSB / RPI-RF-MOD (CC1101+CC1200, voll HmIP-fähig) laufen beide
 * mit derselben DualCoPro_App-Firmware.  Ein passiver app_tag-Vergleich
 * reicht also nicht für die HmIP-verified-capability-Ableitung.
 *
 * Dieses Tool:
 *   1. öffnet beide angegebenen USB-Sticks parallel via transport_usb,
 *   2. bringt jeden via radio_dualcopro_boot_to_app() in App-Mode,
 *   3. sendet ein Probe-Panel (COMMON_IDENTIFY, GET_ADAPTER_MIC,
 *      Garbage-SEND_PROTOCOL_FRAME) auf der HMIP-Layer (dst=0x02),
 *   4. drukt RX-Antworten side-by-side als hex-dump.
 *
 * Strikt USB-Layer — keine echten Air-Frames, keine AES-Operationen.
 * Out-of-Scope: das Modul muss bereits aus dem BL ins App durchgekommen
 * sein (radio_dualcopro_boot_to_app übernimmt das).
 *
 * Bauen:
 *   make tools/hmip_probe   (ergibt bin/hmip_probe)
 *
 * Laufen:
 *   bin/hmip_probe                    # default: 1b1f:c020 + 0403:6f70
 *   bin/hmip_probe 1b1f:c020 0403:6f70
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "frame.h"
#include "radio_dualcopro.h"
#include "transport.h"
#include "transport_usb.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* ─── Probe-Panel ─────────────────────────────────────────────────────
 *
 * Quellen-Recon (siehe HmIP-RE / CULFW32):
 *   dst=0xfe (COMMON) cmd=0x01 IDENTIFY        — auf jedem DualCoPro
 *   dst=0x02 (HMIP)   cmd=0x19 GET_ADAPTER_MIC — HmIP-only-Diag (sicher)
 *   dst=0x02 (HMIP)   cmd=0x03 SEND_PROTOCOL_FRAME — TX-Versuch (RF-aktiv)
 *
 * GET_ADAPTER_MIC ist die sichere Variante: Query-Cmd, kein RF.  Wenn
 * der HmIP-Stack im Firmware-RAM initialisiert ist, antwortet das
 * Modul mit einem 4-Byte-MIC.  Ist der Stack nicht initialisiert (kein
 * HmIP-Radio im Boot-Probe gefunden), erwarten wir entweder Stille
 * oder einen Error-Frame mit definiertem Cmd.  Genau dieser Differenz-
 * Pattern ist der empirische Output dieses Tools.
 */

struct probe_step {
    const char *name;
    uint8_t     dst;
    uint8_t     cmd_byte;
    const uint8_t *extra;       /* zusätzliches Payload nach dem cmd-Byte */
    size_t      extra_len;
    int         wait_ms;        /* timeout für RX nach TX */
};

/* Eine 24-Byte garbage HmIP-airframe-Struktur — strukturell-konformes
 * Header-Layout (0x10 = adapter→device, 0x01 = STATE), aber bewusst
 * unbekannte SGTIN + AES-Garbage.  Ein HmIP-fähiges Modul wird
 * versuchen das zu TX-en und mit Status-Code antworten; ein CC1101-only
 * Modul sollte den Frame am HmIP-Stack-Entry rejecten (anderer
 * Status-Code oder Stille).  ⚠ Erzeugt RF-Aktivität wenn HmIP-Hardware
 * vorhanden — Test im Lab ohne Pair-Beziehung zu sensitiven Aktoren! */
static const uint8_t hmip_send_protocol_garbage[24] = {
    0x10, 0x01,                                     /* mtype=adapter→dev, scmd=STATE */
    0x00, 0x00, 0x00, 0x01,                         /* ZielDevHmid (4B) */
    0x00, 0x00, 0x00, 0x02,                         /* SrcAdapterHmid (4B) */
    0x00, 0x00,                                     /* flags+channel */
    0x00,                                           /* secCnt */
    0xde, 0xad, 0xbe, 0xef,                         /* 4-Byte MIC */
    0xca, 0xfe, 0xba, 0xbe, 0x00, 0x11, 0x22        /* 7-Byte ciphertext */
};

static const struct probe_step PROBES[] = {
    /* 0xfe COMMON_IDENTIFY — Sanity, sollte beide IDs liefern */
    { "COMMON_IDENTIFY",   0xfe, 0x01, NULL,                       0,  800 },
    /* 0x02 GET_ADAPTER_MIC — sicher, kein RF */
    { "HMIP_GET_ADAPTER_MIC", 0x02, 0x19, NULL,                    0,  800 },
    /* 0x02 SEND_PROTOCOL_FRAME mit garbage-payload — triggert RF wenn
     * HmIP-Hardware da ist.  Letzter Probe weil destruktiv. */
    { "HMIP_SEND_PROTOCOL_FRAME(garbage)", 0x02, 0x03,
      hmip_send_protocol_garbage, sizeof(hmip_send_protocol_garbage), 1500 },
};
#define N_PROBES (sizeof(PROBES) / sizeof(PROBES[0]))

/* ─── RX-Sammler ──────────────────────────────────────────────────── */

#define MAX_RX_FRAMES 16

struct rx_frame {
    uint8_t  dst;
    uint8_t  cnt;
    size_t   plen;
    uint8_t  payload[256];
};

struct rx_collector {
    struct rx_frame frames[MAX_RX_FRAMES];
    int             n_frames;
};

static void rx_cb(void *ctx, uint8_t dst, uint8_t cnt,
                  const uint8_t *p, size_t plen)
{
    struct rx_collector *col = ctx;
    if (col->n_frames >= MAX_RX_FRAMES) return;
    if (plen > sizeof(col->frames[0].payload)) plen = sizeof(col->frames[0].payload);
    struct rx_frame *f = &col->frames[col->n_frames++];
    f->dst  = dst;
    f->cnt  = cnt;
    f->plen = plen;
    memcpy(f->payload, p, plen);
}

/* Liest fd bis timeout_ms abgelaufen, füttert decoder.  Sammelt alle
 * Frames die in dem Fenster ankommen. */
static void rx_collect(int fd, hmu_decoder_t *dec, int timeout_ms)
{
    struct timeval deadline, now, tv;
    gettimeofday(&deadline, NULL);
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    deadline.tv_sec  += timeout_ms / 1000 + deadline.tv_usec / 1000000;
    deadline.tv_usec %= 1000000;

    uint8_t buf[512];
    for (;;) {
        gettimeofday(&now, NULL);
        long rem_us = (deadline.tv_sec - now.tv_sec) * 1000000L
                    + (deadline.tv_usec - now.tv_usec);
        if (rem_us <= 0) return;
        tv.tv_sec  = rem_us / 1000000L;
        tv.tv_usec = rem_us % 1000000L;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (r == 0) return;

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        hmu_decoder_feed(dec, buf, (size_t)n);
    }
}

/* ─── Probe-Send-Helper ────────────────────────────────────────────── */

static int probe_send(int fd, const struct probe_step *s, uint8_t cnt)
{
    uint8_t pl[1 + 256];
    if (s->extra_len + 1 > sizeof(pl)) return -1;
    pl[0] = s->cmd_byte;
    if (s->extra_len) memcpy(pl + 1, s->extra, s->extra_len);

    uint8_t enc[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(s->dst, cnt, pl, 1 + s->extra_len,
                             enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

/* ─── Hex-Dump ─────────────────────────────────────────────────────── */

static void hexdump(FILE *out, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        fprintf(out, "%02x%s", p[i], (i + 1 == n) ? "" : " ");
}

static const char *dst_name(uint8_t d)
{
    switch (d) {
    case 0x00: return "OS";
    case 0x01: return "APP";
    case 0x02: return "HMIP";
    case 0x03: return "LLMAC";
    case 0xfe: return "COMMON";
    case 0xff: return "DUAL_ERR";
    default:   return "?";
    }
}

static void print_rx(FILE *out, const struct rx_collector *col, const char *prefix)
{
    if (col->n_frames == 0) {
        fprintf(out, "  %s (silence)\n", prefix);
        return;
    }
    for (int i = 0; i < col->n_frames; ++i) {
        const struct rx_frame *f = &col->frames[i];
        fprintf(out, "  %s [%d] dst=0x%02x(%s) cnt=%u plen=%zu  ",
                prefix, i, f->dst, dst_name(f->dst), f->cnt, f->plen);
        if (f->plen <= 32) {
            hexdump(out, f->payload, f->plen);
        } else {
            hexdump(out, f->payload, 16);
            fprintf(out, " … (%zu more)", f->plen - 16);
        }
        fprintf(out, "\n");
    }
}

/* ─── Per-Stick-Setup ──────────────────────────────────────────────── */

struct stick {
    char                     vid_pid[16];
    struct transport        *t;
    char                     app_tag[64];
    int                      booted;     /* 0 / 1 */
};

static int stick_open_and_boot(struct stick *st, const char *vid_pid)
{
    snprintf(st->vid_pid, sizeof(st->vid_pid), "%s", vid_pid);
    st->t = transport_usb_new_str(vid_pid, 115200);
    if (!st->t) {
        fprintf(stderr, "[%s] transport_usb_new_str FAIL\n", vid_pid);
        return -1;
    }
    if (transport_open(st->t) < 0) {
        fprintf(stderr, "[%s] transport_open FAIL\n", vid_pid);
        return -1;
    }
    fprintf(stderr, "[%s] open ok, fd=%d\n", vid_pid, st->t->fd);

    /* radio_dualcopro_boot_to_app druckt selbst auf stderr "RADIO[…]: TX
     * COMMON_IDENTIFY …".  Gibt 0 / -1 zurück und schreibt app_tag rein. */
    int rc = radio_dualcopro_boot_to_app(st->t->fd, st->app_tag, sizeof(st->app_tag));
    st->booted = (rc == 0);
    fprintf(stderr, "[%s] boot rc=%d app_tag='%s'\n",
            vid_pid, rc, st->app_tag);
    return rc;
}

static void stick_close(struct stick *st)
{
    if (st->t) {
        transport_close(st->t);
        transport_free(st->t);
        st->t = NULL;
    }
}

/* Probe-Panel ausführen, Ergebnisse ausdrucken */
static void stick_probe_panel(struct stick *st)
{
    fprintf(stdout, "\n══════════════════════════════════════════════════════\n");
    fprintf(stdout, " Stick: %-12s  app_tag='%s'  booted=%s\n",
            st->vid_pid, st->app_tag, st->booted ? "yes" : "NO");
    fprintf(stdout, "══════════════════════════════════════════════════════\n");
    if (!st->booted) {
        fprintf(stdout, "  → boot fehlgeschlagen, probe-panel übersprungen\n");
        return;
    }

    uint8_t cnt = 100;   /* off-by-radio_dualcopro_boot range */
    for (size_t i = 0; i < N_PROBES; ++i, ++cnt) {
        const struct probe_step *s = &PROBES[i];
        fprintf(stdout, "\n── %s  (TX dst=0x%02x cmd=0x%02x +%zu B) ──\n",
                s->name, s->dst, s->cmd_byte, s->extra_len);

        /* Decoder + collector neu pro Probe */
        struct rx_collector col = { 0 };
        hmu_decoder_t dec;
        hmu_decoder_init(&dec, rx_cb, &col);

        if (probe_send(st->t->fd, s, cnt) < 0) {
            fprintf(stdout, "  TX FAIL\n");
            continue;
        }
        rx_collect(st->t->fd, &dec, s->wait_ms);
        print_rx(stdout, &col, "RX");
    }
}

/* ─── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *a_vid_pid = (argc > 1) ? argv[1] : "1b1f:c020";
    const char *b_vid_pid = (argc > 2) ? argv[2] : "0403:6f70";

    fprintf(stderr, "hmip_probe: A=%s  B=%s\n", a_vid_pid, b_vid_pid);

    struct stick a = { 0 }, b = { 0 };
    int ra = stick_open_and_boot(&a, a_vid_pid);
    int rb = stick_open_and_boot(&b, b_vid_pid);

    /* Probe nacheinander; parallele Aktionen würden RF-Kollisionen
     * provozieren.  Boot ist bereits durch (zwei separate fds). */
    if (ra == 0) stick_probe_panel(&a);
    if (rb == 0) stick_probe_panel(&b);

    stick_close(&a);
    stick_close(&b);
    fputs("\nhmip_probe: done\n", stderr);
    return (ra == 0 && rb == 0) ? 0 : 1;
}
