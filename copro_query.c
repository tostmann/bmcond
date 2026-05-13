// SPDX-License-Identifier: GPL-2.0-or-later
/* copro_query.c — Read-only DualCoPro Co-CPU-Inventarisierung
 *
 * Stage 2 Iter 1 von Co-CPU-FW-Update-Pfad.  Spec:
 *   docs/copro_update_protocol_re_2026-05-08.md
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "copro_query.h"
#include "eq3_image.h"
#include "frame.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

/* ───── Reply-Match-State ─────────────────────────────────────────────── */

typedef struct {
    /* Match-Kriterien — vom Caller gesetzt, vom Decoder geprüft. */
    uint8_t  want_dst;        /* z.B. HMU_DST_COMMON oder HMU_DST_APP */
    uint8_t  want_cnt;        /* Sequence-counter (frame-level cnt-byte).
                               * Pflicht-match — sonst kollidieren Replies
                               * verschiedener Anfragen mit gleichem
                               * resp_cmd (z.B. getIdentify-Reply vs. getSGTIN-
                               * Reply, beide cmd=0x05). */
    uint8_t  want_resp_cmd;   /* Push-cmd den wir erwarten:
                               *   Common  cmd=0x05 (response)
                               *   TRX     cmd=0x04 (response)
                               *   BL      cmd=0x04 (response) */
    uint8_t  want_orig_cmd;   /* cmd den wir gefragt haben — Diagnostik-Field,
                               * Reply enthält ihn nicht direkt. */

    /* Ergebnis-Buffer */
    uint8_t *out;
    size_t   out_cap;
    size_t   out_len;
    int      got;             /* 0 = nothing, 1 = ok */
    int      got_status;      /* aus reply: 0=ERROR, 1=ACK, 2=BUSY, 3=WRONG_INPUT */
} cq_match_t;

/* Per-Subsystem-Sequence-Counter.  Spec: TRXAdapterUpdater inkrementiert
 * den Counter pro Subsystem getrennt (CommunicationData.getSequenceCounter
 * liefert die Subsystem-spezifische Instanz).  Ohne diesen Trick matchen
 * Replies aus früheren Anfragen (cmd=0x05 ist gemeinsamer resp_cmd für
 * alle Common-cmds 0x01..0x04) als unsere — Bug 2026-05-08 gefangen. */
static uint8_t g_seq_common = 0;
static uint8_t g_seq_os     = 0;
static uint8_t g_seq_app    = 0;

/* Reply-Layout (laut RE):
 *   payload[0] = response-cmd (z.B. 0x05 in Common, 0x04 in TRX/BL)
 *   payload[1] = status-byte (0..3, siehe Spec)
 *   payload[2..] = data (wenn status==1 = ACK)
 *
 * Aber: das Java-Tool prüft vorm Status-byte zusätzlich
 *   `commData.getCommand(this).getMessage().get(2)` als Echo des orig_cmd
 *   — d.h. der ORIG-cmd wird in der CommunicationData-State gehalten
 *   (vom Sender), nicht im Wire-Frame zurückgespielt.  Das heißt: das
 *   Wire-Reply hat NUR resp-cmd + status + data.  Wir korrelieren
 *   selbst via Reihenfolge (eine Anfrage zur Zeit, sync). */

static void cq_on_frame(void *ctx, uint8_t dst, uint8_t cnt,
                        const uint8_t *p, size_t plen)
{
    cq_match_t *m = ctx;
    if (m->got) return;            /* schon befüllt */
    if (dst != m->want_dst) return;
    if (cnt != m->want_cnt) return;
    if (plen < 2) return;
    if (p[0] != m->want_resp_cmd) return;

    /* status-byte ist payload[1].  Wir verlangen ACK=0x01. */
    m->got_status = p[1];
    if (p[1] != 0x01) {
        /* Nicht-ACK ist auch ein Match — Caller will wissen dass das Modul
         * geantwortet hat (vs. Timeout).  out_len bleibt 0. */
        m->got = 1;
        return;
    }

    size_t data_len = plen - 2;
    if (data_len > m->out_cap) data_len = m->out_cap;
    if (m->out && data_len) memcpy(m->out, p + 2, data_len);
    m->out_len = data_len;
    m->got = 1;
}

/* ───── Sync-IO Helper ─────────────────────────────────────────────────── */

static int cq_drain(int fd, int drain_ms)
{
    struct timeval tv = { .tv_sec = drain_ms / 1000, .tv_usec = (drain_ms % 1000) * 1000 };
    uint8_t buf[256];
    int total = 0;
    for (;;) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) return total;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return total;
        total += (int)n;
        tv.tv_sec = 0; tv.tv_usec = 50 * 1000;
    }
}

static int cq_wait_match(int fd, hmu_decoder_t *dec, cq_match_t *m,
                         int timeout_ms)
{
    struct timeval deadline, now, tv;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  +=  timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }

    uint8_t buf[256];
    while (!m->got) {
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

static int cq_send_dataless(int fd, uint8_t dst, uint8_t cnt, uint8_t cmd)
{
    uint8_t enc[HMU_MAX_FRAME_ESC];
    uint8_t pl[1] = { cmd };
    int n = hmu_frame_encode(dst, cnt, pl, sizeof(pl), enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

/* Generischer Probe-mit-Retry.  Sendet ein dataless-Command an dst+cmd,
 * wartet auf ein Reply mit cmd=resp_cmd in payload[0] und passendem cnt-byte.
 * Schreibt nach out (max out_cap), liefert echte Datenlänge in *out_len_p. */
static int cq_probe(int fd, uint8_t dst, uint8_t cmd, uint8_t resp_cmd,
                    uint8_t *out, size_t out_cap, size_t *out_len_p,
                    int timeout_ms, const char *label)
{
    /* Drain altem Krimskrams (Push-Frames nach boot_to_app, vorherige
     * Probe-Replies noch unterwegs). */
    cq_drain(fd, 200);

    /* Subsystem-spezifischen counter wählen. */
    uint8_t *seq_ptr = (dst == HMU_DST_COMMON) ? &g_seq_common :
                       (dst == HMU_DST_OS)     ? &g_seq_os     :
                       (dst == HMU_DST_APP)    ? &g_seq_app    : NULL;
    if (!seq_ptr) {
        fprintf(stderr, "COPRO_QUERY[%s]: unbekannter dst=0x%02x\n", label, dst);
        return -1;
    }

    cq_match_t m = {
        .want_dst       = dst,
        .want_resp_cmd  = resp_cmd,
        .want_orig_cmd  = cmd,
        .out            = out,
        .out_cap        = out_cap,
    };
    hmu_decoder_t dec;
    hmu_decoder_init(&dec, cq_on_frame, &m);

    /* Bis zu 3 Versuche — Modul kann beim ersten Frame nach boot noch
     * USB-/UART-Buffering nachschwingen. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t cnt = (*seq_ptr)++;
        m.want_cnt = cnt;
        m.got = 0; m.out_len = 0; m.got_status = -1;
        if (cq_send_dataless(fd, dst, cnt, cmd) < 0) {
            fprintf(stderr, "COPRO_QUERY[%s]: TX failed: %s\n",
                    label, strerror(errno));
            return -1;
        }
        if (cq_wait_match(fd, &dec, &m, timeout_ms / 3 + 1) == 0) {
            if (m.got_status == 0x01) {
                if (out_len_p) *out_len_p = m.out_len;
                return 0;
            }
            /* Reply kam, aber kein ACK.  Bei TRX-getVersion in BL-Mode kommt
             * z.B. WRONG_INPUT — kein Retry, sondern direkt Fehler. */
            fprintf(stderr, "COPRO_QUERY[%s]: Modul antwortet, aber status=%d\n",
                    label, m.got_status);
            return -1;
        }
    }
    fprintf(stderr, "COPRO_QUERY[%s]: keine Antwort nach 3 Versuchen\n", label);
    return -1;
}

/* ───── Public API ───────────────────────────────────────────────────── */

int copro_query_identify(int fd, char *tag_out, size_t tag_cap, int timeout_ms)
{
    if (fd < 0 || !tag_out || tag_cap == 0) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    uint8_t buf[64];
    size_t  data_len = 0;
    /* Common-Subsystem (dst=0xFE): cmd=0x01 getIdentify, reply cmd=0x05.
     * Tag ist ASCII, NICHT NUL-terminiert.  Funktioniert für DualCoPro-
     * Familie (HMIP_TRX_Bl/App, DualCoPro_App), aber NICHT für Co_CPU-
     * Familie (HM-MOD-RPI-PCB-BL/Legacy-FW). */
    int rc = cq_probe(fd, HMU_DST_COMMON, 0x01, 0x05,
                      buf, sizeof(buf), &data_len, timeout_ms / 2, "IDENTIFY");
    if (rc == 0 && data_len > 0) {
        if (data_len >= tag_cap) data_len = tag_cap - 1;
        memcpy(tag_out, buf, data_len);
        tag_out[data_len] = 0;
        return 0;
    }

    /* Fallback OS-Layer: dst=0x00 cmd=0x00 OS_GET_APP — antwortet bei
     * Co_CPU_BL/App und HM-MOD-RPI-PCB-Familie.  Reply-Layout (laut
     * piVCCU/detect_radio_module/radiomoduledetector.cpp:146):
     *   cmd=0x04 (HMSYSTEM_ACK) data_len=10 data[0]=0x02 data[1..]="Co_CPU_BL"
     *   cmd=0x00 (push) data_len=9 data[0..]="Co_CPU_BL"
     * In OS-Layer ist data[1]=0x02 ein **info-marker** (= "tag-payload
     * folgt"), KEIN BUSY-status wie in DualCoPro-Subsystemen.  cq_probe
     * würde p[1]=0x02 als BUSY interpretieren und -1 returnen — daher
     * direkter custom-match-Loop hier. */
    /* Custom-Loop (cq_probe-Spec passt nicht weil OS-Layer p[1]=0x02
     * info-marker ist, kein BUSY-status).  Pragma: send OS_GET_APP,
     * read raw bytes, substring-search nach Co_CPU_*-Tags. */
    cq_drain(fd, 200);
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t cnt = g_seq_os++;
        cq_send_dataless(fd, HMU_DST_OS, cnt, 0x00);
        struct timeval deadline, now, tv;
        gettimeofday(&deadline, NULL);
        int per = (timeout_ms / 2) / 3 + 1;
        deadline.tv_sec += per / 1000;
        deadline.tv_usec += (per % 1000) * 1000;
        if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }
        uint8_t scan[512]; size_t scan_len = 0;
        int found = 0;
        while (!found) {
            gettimeofday(&now, NULL);
            long us = (deadline.tv_sec - now.tv_sec)*1000000L + (deadline.tv_usec - now.tv_usec);
            if (us <= 0) break;
            tv.tv_sec = us / 1000000; tv.tv_usec = us % 1000000;
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            int r = select(fd+1, &rfds, NULL, NULL, &tv);
            if (r <= 0) break;
            ssize_t n = read(fd, scan + scan_len,
                             sizeof(scan) - scan_len);
            if (n <= 0) break;
            scan_len += (size_t)n;
            for (size_t i = 0; i + 9 <= scan_len && !found; ++i) {
                if (memcmp(scan + i, "Co_CPU_BL", 9) == 0) {
                    size_t cap = (9 < tag_cap) ? 9 : tag_cap - 1;
                    memcpy(tag_out, "Co_CPU_BL", cap); tag_out[cap] = 0;
                    found = 1;
                }
                else if (i + 10 <= scan_len &&
                         memcmp(scan + i, "Co_CPU_App", 10) == 0) {
                    size_t cap = (10 < tag_cap) ? 10 : tag_cap - 1;
                    memcpy(tag_out, "Co_CPU_App", cap); tag_out[cap] = 0;
                    found = 1;
                }
            }
        }
        if (found) return 0;
    }
    return -1;
}

int copro_query_sgtin(int fd, uint8_t sgtin_out[12], int timeout_ms)
{
    if (fd < 0 || !sgtin_out) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    uint8_t buf[16];
    size_t  data_len = 0;
    /* Common-Subsystem cmd=0x04 getSGTIN, reply cmd=0x05 + 12 bytes. */
    int rc = cq_probe(fd, HMU_DST_COMMON, 0x04, 0x05,
                      buf, sizeof(buf), &data_len, timeout_ms, "SGTIN");
    if (rc < 0) return -1;

    if (data_len < 12) {
        fprintf(stderr, "COPRO_QUERY[SGTIN]: erwartete 12 bytes, bekam %zu\n",
                data_len);
        return -1;
    }
    memcpy(sgtin_out, buf, 12);
    return 0;
}

int copro_query_app_version(int fd, uint8_t version_out[3], int timeout_ms)
{
    if (fd < 0 || !version_out) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    uint8_t buf[16];
    size_t  data_len = 0;
    /* TRXAdapter (dst=0x01 = HMU_DST_APP): cmd=0x02 getVersion,
     * reply cmd=0x04. Daten-Länge: 3 bytes (single version) oder
     * 9 bytes (3 versionen × 3 bytes — app, hw, ?).  Wir nehmen die
     * ersten 3.  RE-Quelle: TRXAdapter.parsResponseData case 2. */
    int rc = cq_probe(fd, HMU_DST_APP, 0x02, 0x04,
                      buf, sizeof(buf), &data_len, timeout_ms, "APP_VERSION");
    if (rc < 0) return -1;

    if (data_len < 3) {
        fprintf(stderr, "COPRO_QUERY[APP_VERSION]: erwartete >=3 bytes, bekam %zu\n",
                data_len);
        return -1;
    }
    version_out[0] = buf[0];
    version_out[1] = buf[1];
    version_out[2] = buf[2];
    return 0;
}

int copro_query_mcu_type(int fd, copro_hw_type_t *type_out, int timeout_ms)
{
    if (fd < 0 || !type_out) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    uint8_t buf[8];
    size_t  data_len = 0;
    /* TRXAdapter (dst=0x01 cmd=0x09 getMCUType), reply cmd=0x04 status=0x01
     * + 1 byte = piVCCU radio_module_type_t enum.
     * RE-Quelle: TRXAdapter.java getMCUType (sendDatalessCommand((byte)9))
     * + piVCCU detect_radio_module/radiomoduledetector.cpp:210. */
    int rc = cq_probe(fd, HMU_DST_APP, 0x09, 0x04,
                      buf, sizeof(buf), &data_len, timeout_ms, "MCU_TYPE");
    if (rc < 0) return -1;
    if (data_len < 1) {
        fprintf(stderr, "COPRO_QUERY[MCU_TYPE]: leer\n");
        return -1;
    }
    *type_out = (copro_hw_type_t)buf[0];
    return 0;
}

const char *copro_hw_type_name(copro_hw_type_t type, const char *sgtin_hex)
{
    switch (type) {
    case COPRO_HW_HMIP_RFUSB:
        /* SGTIN-Prefix `3014F5AC` → Telekom-Variante (laut piVCCU
         * main.cpp:122 `(strstr(sgtin, "3014F5AC") == sgtin)`). */
        if (sgtin_hex && strncmp(sgtin_hex, "3014F5AC", 8) == 0)
            return "HMIP-RFUSB-TK";
        return "HMIP-RFUSB";
    case COPRO_HW_HM_MOD_RPI_PCB:  return "HM-MOD-RPI-PCB";
    case COPRO_HW_RPI_RF_MOD:      return "RPI-RF-MOD";
    case COPRO_HW_NONE:            return "(none)";
    default:                       return "(unknown)";
    }
}

int copro_query_bl_version(int fd, uint8_t version_out[3], int timeout_ms)
{
    if (fd < 0 || !version_out) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    uint8_t buf[8];
    size_t  data_len = 0;
    /* Bootloader (dst=0x00 = HMU_DST_OS): cmd=0x02 getVersion,
     * reply cmd=0x04 + 3 bytes.  RE-Quelle: Bootloader.parsResponseData. */
    int rc = cq_probe(fd, HMU_DST_OS, 0x02, 0x04,
                      buf, sizeof(buf), &data_len, timeout_ms, "BL_VERSION");
    if (rc < 0) return -1;

    if (data_len < 3) {
        fprintf(stderr, "COPRO_QUERY[BL_VERSION]: erwartete >=3 bytes, bekam %zu\n",
                data_len);
        return -1;
    }
    version_out[0] = buf[0];
    version_out[1] = buf[1];
    version_out[2] = buf[2];
    return 0;
}

/* ───── Stage 2 Iter 2 — Mode-Switch ──────────────────────────────────── */

copro_mode_t copro_classify_tag(const char *tag)
{
    if (!tag || !*tag) return COPRO_MODE_UNKNOWN;
    size_t n = strlen(tag);

    /* Whitelist (wie hmip-copro-update.jar CommonSubsystem Z. 37):
     *   bootloader: HMIP_TRX_Bl, HMIPW_USB_Bl, HMIPW_DRAP_Bl, HMIP_TRX_Bl_prod,
     *               Co_CPU_BL
     *   application: HMIP_TRX_App, DualCoPro_App, HMIPW_USB_App,
     *                HMIPW_CoPro_App, HMIPW_DRAP_App, HMIP_TRX_App_prod,
     *                Co_CPU_App
     *
     * Heuristik: Suffix-Match deckt alle bekannten Varianten + zukünftige. */
    if (n >= 4 && strcmp(tag + n - 4, "_App") == 0)         return COPRO_MODE_APPLICATION;
    if (n >= 8 && strcmp(tag + n - 8, "_App_prod") == 0)    return COPRO_MODE_APPLICATION;
    if (n >= 3 && (strcmp(tag + n - 3, "_Bl") == 0 ||
                   strcmp(tag + n - 3, "_BL") == 0))        return COPRO_MODE_BOOTLOADER;
    if (n >= 7 && strcmp(tag + n - 7, "_Bl_prod") == 0)     return COPRO_MODE_BOOTLOADER;

    /* Legacy-DUAL_ERR-Pseudotag aus radio_dualcopro.c */
    if (strstr(tag, "DualCoPro_App") != NULL)               return COPRO_MODE_APPLICATION;
    if (strstr(tag, "DUAL_ERR") != NULL)                    return COPRO_MODE_APPLICATION;
    return COPRO_MODE_UNKNOWN;
}

/* Wartet auf einen Identify-Push (dst=0xFE cmd=0x00 + ASCII-Tag) oder
 * einen IDENTIFY-Reply (dst=0xFE cmd=0x05 status=0x01 + Tag).  Nutzt einen
 * eigenen Match-State, weil cq_match_t nur auf cmd-Echo passt — Identify-
 * Push hat aber cmd=0x00 (Push-Form). */
typedef struct {
    char    tag[32];
    int     got;
} cq_identify_state_t;

static void cq_identify_on_frame(void *ctx, uint8_t dst, uint8_t cnt,
                                 const uint8_t *p, size_t plen)
{
    (void)cnt;
    cq_identify_state_t *st = ctx;
    if (st->got) return;
    if (dst != HMU_DST_COMMON) return;
    if (plen < 2) return;

    const uint8_t *tag_p = NULL;
    size_t taglen = 0;

    /* Identify-Push (vom Modul nach Reboot): cmd=0x00 + Tag */
    if (p[0] == 0x00) {
        tag_p = p + 1; taglen = plen - 1;
    }
    /* IDENTIFY-Reply auf manuelle Anfrage: cmd=0x05 status=0x01 + Tag */
    else if (p[0] == 0x05 && plen >= 3 && p[1] == 0x01) {
        tag_p = p + 2; taglen = plen - 2;
    } else {
        return;
    }

    if (taglen >= sizeof(st->tag)) taglen = sizeof(st->tag) - 1;
    memcpy(st->tag, tag_p, taglen);
    st->tag[taglen] = 0;
    st->got = 1;
}

/* Wartet auf einen frischen Identify-Push.  Returns 0 wenn Tag empfangen
 * (in *st), -1 bei Timeout. */
static int cq_wait_identify(int fd, cq_identify_state_t *st, int timeout_ms)
{
    hmu_decoder_t dec;
    hmu_decoder_init(&dec, cq_identify_on_frame, st);
    st->tag[0] = 0; st->got = 0;

    struct timeval deadline, now, tv;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  +=  timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }

    uint8_t buf[256];
    while (!st->got) {
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
        hmu_decoder_feed(&dec, buf, (size_t)n);
    }
    return 0;
}

/* Gemeinsamer Switch-Helper.  cmd=0x02 → BL, cmd=0x03 → App.
 * target_mode wird gegen den Push-Tag verifiziert. */
static int cq_switch_mode(int fd, uint8_t cmd, copro_mode_t target,
                          char *tag_out, size_t cap, int timeout_ms,
                          const char *label)
{
    /* 1. Identify-Probe: bereits am Ziel? */
    char current_tag[32] = {0};
    if (copro_query_identify(fd, current_tag, sizeof(current_tag),
                             COPRO_QUERY_DEFAULT_TIMEOUT_MS) < 0) {
        fprintf(stderr, "COPRO[%s]: identify-Probe fehlgeschlagen\n", label);
        return -1;
    }
    if (copro_classify_tag(current_tag) == target) {
        fprintf(stderr, "COPRO[%s]: bereits in '%s'\n", label, current_tag);
        if (tag_out && cap > 0) {
            snprintf(tag_out, cap, "%s", current_tag);
        }
        return 0;
    }

    /* 2. Send Switch-Cmd (COMMON dataless), warte ACK */
    fprintf(stderr, "COPRO[%s]: TX cmd=0x%02x (von '%s')\n",
            label, cmd, current_tag);
    /* Drain pending Push-Frames die beim identify reinkommen könnten. */
    cq_drain(fd, 200);

    uint8_t cnt = g_seq_common++;
    cq_match_t m = {
        .want_dst       = HMU_DST_COMMON,
        .want_cnt       = cnt,
        .want_resp_cmd  = 0x05,    /* COMMON-Subsystem-Reply */
        .want_orig_cmd  = cmd,
    };
    hmu_decoder_t dec_ack;
    hmu_decoder_init(&dec_ack, cq_on_frame, &m);

    if (cq_send_dataless(fd, HMU_DST_COMMON, cnt, cmd) < 0) {
        fprintf(stderr, "COPRO[%s]: TX failed\n", label);
        return -1;
    }
    /* ACK kommt schnell (typ <100ms), aber wir geben 4000ms wie das eq-3-
     * Tool (TRXAdapterUpdater.StartBootloader).  Alternativ kommt direkt
     * der Identify-Push ohne separate ACK — manche FW-Versionen verhalten
     * sich so.  In dem Fall springen wir direkt zu Step 3. */
    int ack_timeout = 4000;
    int ack_rc = cq_wait_match(fd, &dec_ack, &m, ack_timeout);

    /* 3. Warte auf Identify-Push */
    int push_timeout = (timeout_ms > 0)
                     ? (timeout_ms - (ack_rc == 0 ? 100 : ack_timeout))
                     : (target == COPRO_MODE_BOOTLOADER ? 5000 : 9000);
    if (push_timeout < 1000) push_timeout = 1000;

    cq_identify_state_t st = {0};
    if (cq_wait_identify(fd, &st, push_timeout) < 0) {
        fprintf(stderr, "COPRO[%s]: kein Identify-Push nach %d ms\n",
                label, push_timeout);
        return -1;
    }

    /* 4. Validate */
    if (copro_classify_tag(st.tag) != target) {
        fprintf(stderr, "COPRO[%s]: Push-Tag '%s' = falscher Mode\n",
                label, st.tag);
        return -1;
    }
    fprintf(stderr, "COPRO[%s]: Push-Bestätigung '%s'\n", label, st.tag);
    if (tag_out && cap > 0) {
        snprintf(tag_out, cap, "%s", st.tag);
    }
    return 0;
}

int copro_start_bootloader(int fd, char *new_tag_out, size_t cap, int timeout_ms)
{
    if (fd < 0) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = 9000;
    return cq_switch_mode(fd, 0x02, COPRO_MODE_BOOTLOADER,
                          new_tag_out, cap, timeout_ms, "START_BL");
}

int copro_start_application(int fd, char *new_tag_out, size_t cap, int timeout_ms)
{
    if (fd < 0) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = 13000;
    return cq_switch_mode(fd, 0x03, COPRO_MODE_APPLICATION,
                          new_tag_out, cap, timeout_ms, "START_APP");
}

/* ───── Stage 2 Iter 3 — Flash-Pfad ───────────────────────────────────── */

/* Wire: dst=0x00 (Bootloader)  cnt  cmd=0x05  + frame_bytes
 * Identisch zu cq_send_dataless aber mit Daten-Body. */
static int cq_send_bootloader_update(int fd, uint8_t cnt,
                                     const uint8_t *frame, size_t len)
{
    /* Frame-Payload für DualCoPro = [0x05][frame_bytes...].  Plus sentinel-
     * encoding: hmu_frame_encode wrappt mit dst+cnt+payload+CRC.  Also
     * wir bauen payload = "0x05" + frame, dann encode mit dst=OS+cnt. */
    uint8_t buf[1 + HMU_MAX_PAYLOAD];
    if (len + 1 > sizeof(buf)) { errno = E2BIG; return -1; }
    buf[0] = 0x05;
    memcpy(buf + 1, frame, len);

    uint8_t enc[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(HMU_DST_OS, cnt, buf, len + 1, enc, sizeof(enc));
    if (n < 0) return -1;
    return write(fd, enc, n) == n ? 0 : -1;
}

int copro_send_update_frame(int fd, const uint8_t *frame_bytes, size_t len,
                            int timeout_ms)
{
    if (fd < 0 || !frame_bytes || len == 0) { errno = EINVAL; return -1; }
    if (timeout_ms <= 0) timeout_ms = COPRO_QUERY_DEFAULT_TIMEOUT_MS;

    cq_drain(fd, 50);

    uint8_t cnt = g_seq_os++;
    cq_match_t m = {
        .want_dst       = HMU_DST_OS,
        .want_cnt       = cnt,
        .want_resp_cmd  = 0x04,    /* Bootloader-Subsystem-Reply */
        .want_orig_cmd  = 0x05,
    };
    hmu_decoder_t dec;
    hmu_decoder_init(&dec, cq_on_frame, &m);

    if (cq_send_bootloader_update(fd, cnt, frame_bytes, len) < 0) {
        fprintf(stderr, "COPRO[UPDATE_FRAME]: TX failed: %s\n", strerror(errno));
        return -1;
    }
    if (cq_wait_match(fd, &dec, &m, timeout_ms) < 0) {
        fprintf(stderr, "COPRO[UPDATE_FRAME]: timeout cnt=%u (%d ms)\n",
                cnt, timeout_ms);
        return -1;
    }
    if (m.got_status != 0x01) {
        fprintf(stderr, "COPRO[UPDATE_FRAME]: status=%d cnt=%u\n",
                m.got_status, cnt);
        return -1;
    }
    return 0;
}

const char *copro_flash_result_name(copro_flash_result_t r)
{
    switch (r) {
    case COPRO_FLASH_OK:                      return "OK";
    case COPRO_FLASH_SKIPPED_VERSION_MATCH:   return "SKIPPED_VERSION_MATCH";
    case COPRO_FLASH_FAIL_BL_ENTER:           return "FAIL_BL_ENTER";
    case COPRO_FLASH_FAIL_FRAME:              return "FAIL_FRAME";
    case COPRO_FLASH_FAIL_APP_ENTER:          return "FAIL_APP_ENTER";
    }
    return "UNKNOWN";
}

static void put_err(char *err_msg, size_t err_cap, const char *fmt, ...)
{
    if (!err_msg || err_cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_msg, err_cap, fmt, ap);
    va_end(ap);
}

copro_flash_result_t copro_flash_image(int fd,
                                       const struct eq3_image *image,
                                       const copro_flash_opts_t *opts,
                                       char *err_msg, size_t err_cap)
{
    static const copro_flash_opts_t defaults = { 0, 0, NULL, NULL };
    if (!opts) opts = &defaults;
    if (fd < 0 || !image) {
        put_err(err_msg, err_cap, "EINVAL");
        return COPRO_FLASH_FAIL_BL_ENTER;
    }
    int per_frame = opts->per_frame_timeout_ms > 0
                  ? opts->per_frame_timeout_ms : 4000;

    /* 1. Skip-if-Equal-Check (nur wenn force_overwrite==0 und version known). */
    if (image->version_known && !opts->force_overwrite) {
        uint8_t cur[3] = {0};
        if (copro_query_app_version(fd, cur, 0) == 0) {
            if (cur[0] == image->expected_version[0] &&
                cur[1] == image->expected_version[1] &&
                cur[2] == image->expected_version[2]) {
                fprintf(stderr,
                    "COPRO[FLASH]: current %u.%u.%u == expected %u.%u.%u, skip\n",
                    cur[0], cur[1], cur[2],
                    image->expected_version[0],
                    image->expected_version[1],
                    image->expected_version[2]);
                return COPRO_FLASH_SKIPPED_VERSION_MATCH;
            }
            fprintf(stderr,
                "COPRO[FLASH]: current %u.%u.%u → target %u.%u.%u\n",
                cur[0], cur[1], cur[2],
                image->expected_version[0],
                image->expected_version[1],
                image->expected_version[2]);
        }
    }

    /* 2. Enter Bootloader (or trust caller that we're already there). */
    char tag[32] = {0};
    if (opts->assume_in_bl) {
        fprintf(stderr, "COPRO[FLASH]: assume_in_bl=1 — skip start_bootloader\n");
    } else {
        if (copro_start_bootloader(fd, tag, sizeof(tag), 0) < 0) {
            put_err(err_msg, err_cap, "start_bootloader failed");
            return COPRO_FLASH_FAIL_BL_ENTER;
        }
        fprintf(stderr, "COPRO[FLASH]: entered BL '%s'\n", tag);
    }

    /* 3. BL-Version (informational).  Skip wenn assume_in_bl, weil legacy
     * Co_CPU-BL das Bootloader.getVersion-cmd nicht supportet → BUSY-reply
     * würde Modul-State irritieren vor flash-Loop. */
    uint8_t bl_v[3] = {0};
    if (!opts->assume_in_bl) {
        if (copro_query_bl_version(fd, bl_v, 0) == 0) {
            fprintf(stderr, "COPRO[FLASH]: BL version %u.%u.%u\n",
                    bl_v[0], bl_v[1], bl_v[2]);
        }
    } else {
        /* Wakeup-Probe wie eq3configcmd live-strace zeigt: COMMON_IDENTIFY
         * (nur das eine, KEIN OS_GET_APP — vergleich /tmp/strace-eq3.log).
         * Bringt Modul-BL in den "ready"-state.  cnt=0 (g_seq_common
         * ist hier noch frisch). */
        cq_drain(fd, 200);
        uint8_t cnt_c = g_seq_common++;
        cq_send_dataless(fd, HMU_DST_COMMON, cnt_c, 0x01);
        usleep(200 * 1000);
        cq_drain(fd, 200);
        fprintf(stderr,
                "COPRO[FLASH]: wakeup-probe done (COMMON_IDENTIFY cnt=%u)\n",
                cnt_c);
        /* g_seq_os auf 0 lassen — ersten update-frame mit cnt=0 wäre
         * möglich, aber eq3configcmd nutzt cnt=1.  Nicht klar warum.
         * Wir starten flash-loop mit dem nächsten g_seq_os++ = 0.
         * Falls das fehlt, stelle hier g_seq_os++ als sync-skip. */
    }

    /* 4. Send all frames. */
    for (size_t i = 0; i < image->n_frames; ++i) {
        if (opts->on_progress)
            opts->on_progress(opts->progress_ctx, i, image->n_frames);
        if (copro_send_update_frame(fd, image->frames[i].bytes,
                                    image->frames[i].len, per_frame) < 0) {
            put_err(err_msg, err_cap,
                    "frame %zu/%zu failed (ftyp=0x%02x)",
                    i, image->n_frames, image->frames[i].ftyp_base);
            return COPRO_FLASH_FAIL_FRAME;
        }
    }
    if (opts->on_progress)
        opts->on_progress(opts->progress_ctx, image->n_frames, image->n_frames);

    /* 5. Re-enter App (unless caller wants module to stay in BL). */
    if (opts->skip_app_after) {
        fprintf(stderr, "COPRO[FLASH]: skip_app_after=1 — module stays in BL\n");
        return COPRO_FLASH_OK;
    }
    if (copro_start_application(fd, tag, sizeof(tag), 0) < 0) {
        put_err(err_msg, err_cap, "start_application failed");
        return COPRO_FLASH_FAIL_APP_ENTER;
    }

    /* 6. New version (informational). */
    uint8_t new_v[3] = {0};
    if (copro_query_app_version(fd, new_v, 0) == 0) {
        fprintf(stderr, "COPRO[FLASH]: new App version %u.%u.%u\n",
                new_v[0], new_v[1], new_v[2]);
    }

    return COPRO_FLASH_OK;
}
