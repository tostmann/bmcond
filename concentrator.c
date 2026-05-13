// SPDX-License-Identifier: GPL-2.0-or-later
/* busmatic-concentrator — pure userspace transport-shim für multimacd.
 *
 * Architektur (lean, post-2026.5.6):
 *
 *   bmcond ist KEIN Mac-Layer-Replacement.  multimacd (eq-3 proprietär,
 *   GPLv2-eq3_char_loop) ist die legitime Mac-Layer — DUTY/CSMA/AES/
 *   LLMAC-Translation/3burst-Retry passieren dort.
 *
 *   bmcond ersetzt nur den Kernel-Pfad:
 *     piVCCU:   hb_rf_usb_2.ko / hb_rf_eth.ko / generic_raw_uart.ko
 *               → /dev/raw-uart (Char-Device, multimacd opens this)
 *     bmcond:   transport_{uart,tcp,usb,eth,rfnethm}  +  PTY
 *               → /dev/raw-uart (Symlink auf PTY-slave, multimacd opens this)
 *
 *   Der Concentrator macht ausschließlich byte-pump zwischen einem
 *   Transport-fd und der PTY-master-Seite (via run_raw_uart_shim).
 *   Plus: einmaliger Boot-to-App (radio_dualcopro_boot_to_app) zur
 *   Hardware-Identifikation, optional .eq3-Flash für firmware-update,
 *   und confgen-Output für rfd.conf + /var/run/bmcd-config.json.
 *
 * CLI (Lean-Mode):
 *
 *   busmatic-concentrator \
 *     -U 1b1f:c020          (oder -t /dev/ttyAMA0, -N host:port, -E host)
 *     --raw-uart=/dev/raw-uart
 *     [-H HMID -S SERIAL -F MAJOR.MINOR.PATCH -G SGTIN]
 *     [-C [-D]]             (confgen emit / dry-run)
 *     [-v [-V]]             (verbose / very-verbose)
 *
 *   --raw-uart=PATH ist im Lean-Mode PFLICHT — ohne PTY-shim ist
 *   bmcond funktionslos.
 *
 * Lizenz: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <pty.h>
#include <getopt.h>

#include "api.h"
#include "confgen.h"
#include "copro_query.h"
#include "eq3_image.h"
#include "hardware.h"
#include "radio_dualcopro.h"
#include "radio_id.h"
#include "transport.h"
#include "transport_uart.h"
#include "transport_tcp.h"
#include "transport_usb.h"
#include "transport_eth.h"
#include "version.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ─── bridge_init (Identitätsdaten-Default) ──────────────────────────── */

void bridge_init(bridge_state_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->mode = BRIDGE_MODE_BIDCOS_ONLY;
    st->identify_as_dual = true;
    /* Defaults für CLI-Snapshot — werden via -H/-S/-F/-G überschrieben. */
    st->hmid[0] = 0x32; st->hmid[1] = 0xf1; st->hmid[2] = 0xdf;
    snprintf(st->serial, sizeof(st->serial), "TEQ2822427");
    st->firmware[0] = 2; st->firmware[1] = 8; st->firmware[2] = 6;
}

/* ─── transport_reconnect — Backoff-Loop für Disconnect-Recovery ──────── */

static int transport_reconnect(struct transport *t)
{
    int delay = 1;
    while (!g_stop) {
        if (transport_open(t) == 0) {
            fprintf(stderr, "CONC: %s reconnected to %s\n", t->label, t->target);
            return 0;
        }
        fprintf(stderr, "CONC: %s reconnect to %s failed: %s — retry in %ds\n",
                t->label, t->target, strerror(errno), delay);
        for (int i = 0; i < delay && !g_stop; ++i) sleep(1);
        if (delay < 30) delay = (delay * 2 > 30) ? 30 : delay * 2;
    }
    return -1;
}

/* ─── Inline-Flash (API-getriggert, läuft im main-loop) ──────────────── */

pthread_mutex_t  g_flash_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   g_flash_done_cv = PTHREAD_COND_INITIALIZER;
int              g_flash_request = 0;
static char      g_flash_path[256];
static char      g_flash_target_be[16];
static int       g_flash_force = 0;
static int       g_flash_status = 0;     /* 0=running 1=ok 2=skipped 4=fail */
static char      g_flash_msg[160];

/* progress-callback fürs flash-job — log alle 8 frames + start/end */
static void conc_flash_progress(void *ctx, size_t i, size_t n)
{
    (void)ctx;
    if (i == 0 || i == n || (i % 8) == 0) {
        fprintf(stderr, "CONC: flash-job progress %zu/%zu (%.0f%%)\n",
                i, n, n ? (100.0 * (double)i / (double)n) : 0.0);
    }
}

/* Synchroner API-Aufruf: signal request, wait for done.
 * Returns: 0=OK, 1=SKIPPED, 2=FAIL, 3=TIMEOUT */
int bmcd_inline_flash_request(const char *image_path, const char *backend_name,
                              int force, int timeout_ms,
                              char *msg_out, size_t msg_cap);
int bmcd_inline_flash_request(const char *image_path, const char *backend_name,
                              int force, int timeout_ms,
                              char *msg_out, size_t msg_cap)
{
    pthread_mutex_lock(&g_flash_mu);
    snprintf(g_flash_path, sizeof(g_flash_path), "%s", image_path ? image_path : "");
    snprintf(g_flash_target_be, sizeof(g_flash_target_be), "%s", backend_name ? backend_name : "");
    g_flash_force = force;
    g_flash_status = 0;
    g_flash_msg[0] = 0;
    g_flash_request = 1;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (g_flash_status == 0 && rc == 0) {
        rc = pthread_cond_timedwait(&g_flash_done_cv, &g_flash_mu, &deadline);
    }
    int st = g_flash_status;
    if (st == 0 && rc == ETIMEDOUT) st = 3;
    if (msg_out && msg_cap) snprintf(msg_out, msg_cap, "%s", g_flash_msg);
    pthread_mutex_unlock(&g_flash_mu);
    if (st == 1) return 0;        /* OK */
    if (st == 2) return 1;        /* SKIPPED */
    if (st == 4) return 2;        /* FAIL */
    return 3;                     /* TIMEOUT */
}

static void run_inline_flash(struct transport *t)
{
    if (!t) {
        snprintf(g_flash_msg, sizeof(g_flash_msg), "transport NULL");
        g_flash_status = 4; return;
    }
    fprintf(stderr, "CONC: inline-flash START image='%s' force=%d\n",
            g_flash_path, g_flash_force);

    /* close + re-open → reset_pulse → BL */
    transport_close(t);
    if (transport_open(t) < 0) {
        snprintf(g_flash_msg, sizeof(g_flash_msg),
                 "transport_open after close failed: %s", strerror(errno));
        g_flash_status = 4;
        fprintf(stderr, "CONC: inline-flash FAIL — %s\n", g_flash_msg);
        return;
    }
    usleep(500 * 1000);  /* settle */

    eq3_image_t img;
    if (eq3_image_load(g_flash_path, &img) < 0) {
        /* basename-style trim to keep msg buffer overflow-free. */
        const char *base = strrchr(g_flash_path, '/');
        base = base ? base + 1 : g_flash_path;
        snprintf(g_flash_msg, sizeof(g_flash_msg),
                 "eq3_image_load(%.100s) failed", base);
        g_flash_status = 4;
        fprintf(stderr, "CONC: inline-flash FAIL — %s\n", g_flash_msg);
        return;
    }
    fprintf(stderr, "CONC: inline-flash %zu chunks (chunk_size=%u)\n",
            img.n_frames, img.chunk_size);
    copro_flash_opts_t opts = {
        .force_overwrite = g_flash_force,
        .per_frame_timeout_ms = 4000,
        .assume_in_bl = 1,
        .skip_app_after = 1,
        .on_progress = conc_flash_progress,
    };
    char err[128] = {0};
    copro_flash_result_t r = copro_flash_image(t->fd, &img, &opts, err, sizeof(err));
    eq3_image_free(&img);

    char tag[32] = {0};
    int boot_rc = radio_dualcopro_boot_to_app(t->fd, tag, sizeof(tag));
    fprintf(stderr,
        "CONC: inline-flash result=%s err='%s' boot_back=%s tag='%s'\n",
        copro_flash_result_name(r), err, boot_rc == 0 ? "ok" : "FAIL", tag);

    if (r == COPRO_FLASH_OK) {
        snprintf(g_flash_msg, sizeof(g_flash_msg), "OK — back in App ('%s')", tag);
        g_flash_status = 1;
    } else if (r == COPRO_FLASH_SKIPPED_VERSION_MATCH) {
        snprintf(g_flash_msg, sizeof(g_flash_msg),
                 "skipped — version match (use force=true to override)");
        g_flash_status = 2;
    } else {
        snprintf(g_flash_msg, sizeof(g_flash_msg), "%s — %s",
                 copro_flash_result_name(r), err[0] ? err : "(no detail)");
        g_flash_status = 4;
    }
}

/* ─── run_raw_uart_shim — byte-pump zwischen Transport und PTY-master ──
 *
 * Multimacd öffnet die PTY-slave-Seite (über `link_path`-symlink) als
 * "Coprocessor Device Path" und macht selbst HMU-Demux + LLMAC-Translate
 * + 3burst-Retry + DUTY/CSMA/AES.  Wir liefern nur rohe Bytes.
 *
 * Lifecycle: bei Disconnect des Transports versucht der Pumper periodisch
 * reconnect (transport_reconnect macht backoff).  PTY bleibt offen — wenn
 * multimacd offen hält, sieht es einfach byte-Stillstand.  Kein Crash.
 *
 * Inline-Flash-Hook: vor jedem select-Cycle prüfen ob g_flash_request
 * gesetzt ist; wenn ja, transport_close + run_inline_flash + reopen.
 */
static int run_raw_uart_shim(struct transport *t, const char *link_path, int verbose)
{
    int master_fd = -1, slave_fd = -1;
    char slave_name[128] = {0};
    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
        fprintf(stderr, "shim: openpty failed: %s\n", strerror(errno));
        return 1;
    }
    struct termios tio;
    tcgetattr(slave_fd, &tio);
    cfmakeraw(&tio);
    cfsetspeed(&tio, B115200);
    tcsetattr(slave_fd, TCSANOW, &tio);
    close(slave_fd);

    int fl = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, fl | O_NONBLOCK);

    if (link_path && link_path[0]) {
        unlink(link_path);
        if (symlink(slave_name, link_path) < 0) {
            fprintf(stderr,
                "shim: symlink %s -> %s failed: %s — multimacd config muss "
                "auf %s direkt zeigen\n",
                link_path, slave_name, strerror(errno), slave_name);
        }
    }

    fprintf(stderr, "shim: raw-uart PTY %s%s%s <-> transport %s (%s)\n",
            slave_name,
            (link_path && link_path[0]) ? " <- " : "",
            (link_path && link_path[0]) ? link_path : "",
            t->target, t->label);

    uint8_t buf[2048];
    uint64_t tx_bytes = 0, rx_bytes = 0;
    while (!g_stop) {
        /* Inline-flash request? Bevor wir auf select gehen. */
        pthread_mutex_lock(&g_flash_mu);
        int do_flash = g_flash_request;
        if (do_flash) g_flash_request = 0;
        pthread_mutex_unlock(&g_flash_mu);
        if (do_flash) {
            FILE *busy = fopen("/var/run/bmcd-flash-busy", "w");
            if (busy) { fprintf(busy, "%ld\n", (long)time(NULL)); fclose(busy); }
            run_inline_flash(t);
            unlink("/var/run/bmcd-flash-busy");
            pthread_mutex_lock(&g_flash_mu);
            pthread_cond_broadcast(&g_flash_done_cv);
            pthread_mutex_unlock(&g_flash_mu);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (t->fd >= 0) { FD_SET(t->fd, &rfds); if (t->fd > maxfd) maxfd = t->fd; }
        FD_SET(master_fd, &rfds); if (master_fd > maxfd) maxfd = master_fd;

        struct timeval tv = {1, 0};
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "shim: select: %s\n", strerror(errno));
            break;
        }

        /* Transport -> PTY (RX vom EFM32 nach multimacd) */
        if (t->fd >= 0 && FD_ISSET(t->fd, &rfds)) {
            ssize_t n = read(t->fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t w = write(master_fd, buf, (size_t)n);
                if (w < 0 && errno != EAGAIN) {
                    fprintf(stderr, "shim: pty write: %s\n", strerror(errno));
                }
                rx_bytes += (uint64_t)n;
                if (verbose > 1) fprintf(stderr, "shim: rx %zd\n", n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                fprintf(stderr, "shim: transport read returned %zd (%s) — reconnect\n",
                        n, n < 0 ? strerror(errno) : "EOF");
                transport_close(t);
                if (transport_reconnect(t) < 0) {
                    fprintf(stderr, "shim: reconnect failed, exiting\n");
                    break;
                }
            }
        }

        /* PTY -> Transport (TX von multimacd zum EFM32) */
        if (FD_ISSET(master_fd, &rfds)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0 && t->fd >= 0) {
                ssize_t w = write(t->fd, buf, (size_t)n);
                if (w < 0) {
                    fprintf(stderr, "shim: transport write: %s\n", strerror(errno));
                }
                tx_bytes += (uint64_t)n;
                if (verbose > 1) fprintf(stderr, "shim: tx %zd\n", n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EIO) {
                /* EIO = multimacd hat slave-side noch nicht geöffnet — normal */
                fprintf(stderr, "shim: pty read: %s\n", strerror(errno));
            }
        }
    }

    fprintf(stderr, "shim: stop — tx=%llu rx=%llu\n",
            (unsigned long long)tx_bytes, (unsigned long long)rx_bytes);
    close(master_fd);
    if (link_path && link_path[0]) unlink(link_path);
    return 0;
}

/* ─── CLI parsing helpers ────────────────────────────────────────────── */

static int parse_hmid(const char *s, uint8_t out[3])
{
    int got = 0, v = 0, hi_set = 0;
    for (const char *p = s; *p && got < 3; ++p) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        int c;
        if (*p >= '0' && *p <= '9') c = *p - '0';
        else if (*p >= 'a' && *p <= 'f') c = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') c = *p - 'A' + 10;
        else return -1;
        if (!hi_set) { v = c << 4; hi_set = 1; }
        else { v |= c; out[got++] = (uint8_t)v; hi_set = 0; }
    }
    return got == 3 ? 0 : -1;
}

static int parse_sgtin(const char *s, uint8_t out[12])
{
    int got = 0, v = 0, hi_set = 0;
    for (const char *p = s; *p && got < 12; ++p) {
        if (*p == ':' || *p == '-' || *p == ' ') continue;
        int c;
        if (*p >= '0' && *p <= '9') c = *p - '0';
        else if (*p >= 'a' && *p <= 'f') c = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') c = *p - 'A' + 10;
        else return -1;
        if (!hi_set) { v = c << 4; hi_set = 1; }
        else { v |= c; out[got++] = (uint8_t)v; hi_set = 0; }
    }
    return got == 12 ? 0 : -1;
}

static int parse_firmware(const char *s, uint8_t out[3])
{
    unsigned a = 0, b = 0, c = 0;
    if (sscanf(s, "%u.%u.%u", &a, &b, &c) != 3) return -1;
    if (a > 255 || b > 255 || c > 255) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c;
    return 0;
}

/* "VID:PID" für -U; reject everything else. */
static int looks_like_vid_pid(const char *s)
{
    unsigned v = 0, p = 0;
    return sscanf(s, "%x:%x", &v, &p) == 2;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "busmatic-concentrator  v" FW_VERSION_STRING "  (built " FW_BUILD_DATE ")\n"
        "Pure userspace transport-shim for multimacd.  Replaces piVCCU's\n"
        "hb_rf_usb_2 / hb_rf_eth / generic_raw_uart kernel-modules with a\n"
        "userland PTY-symlink.\n"
        "\n"
        "Usage: %s -U|-t|-N|-E <spec> --raw-uart=<linkpath> [options]\n"
        "\n"
        "Transport (genau eines — optional, Default `-U 1b1f:c020`):\n"
        "  -t DEV               Lokales UART-Device (z.B. /dev/ttyAMA0)\n"
        "  -N HOST:PORT         TCP-Connect (z.B. 127.0.0.1:5000 zu ser2net)\n"
        "  -U VID:PID           libusb-direct (Default: 1b1f:c020 = HmIP-RFUSB)\n"
        "  -E HOST[:PORT]       UDP/hb_rf_eth-Box (Default-Port 3008)\n"
        "\n"
        "PTY-shim (Pflicht):\n"
        "  --raw-uart=PATH      PTY-slave als Symlink unter PATH (z.B.\n"
        "                       /dev/raw-uart).  multimacd öffnet diesen Pfad.\n"
        "\n"
        "Identitäts-Snapshot (für confgen + JSON-API):\n"
        "  -H HMID              3-byte hex                 (default: 32f1df)\n"
        "  -S SERIAL            10 ASCII chars             (default: TEQ2822427)\n"
        "  -F FW                M.m.p                      (default: 2.8.6)\n"
        "  -G SGTIN             24 hex chars (12 bytes)\n"
        "\n"
        "Konfig + Logging:\n"
        "  -C                   confgen emit: schreibt /etc/config/rfd.conf +\n"
        "                       /var/run/bmcd-config.json (Backup *.bmcd-pre).\n"
        "  -D                   confgen dry-run.\n"
        "  -v                   verbose\n"
        "  -V                   very verbose (raw byte counts)\n"
        "  -h                   help\n",
        prog);
}

/* ─── main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    bridge_state_t bridge;
    bridge_init(&bridge);

    /* Backend-Specification — exakt eins. */
    char     be_path[128]  = {0};
    int      be_is_tcp     = 0;
    int      be_is_usb     = 0;
    int      be_is_eth     = 0;
    int      baud          = B115200;

    char     raw_uart_link[128] = {0};

    int      verbose      = 0;
    int      very_verbose = 0;
    int      conf_emit    = 0;
    int      conf_dry     = 0;

    static struct option long_opts[] = {
        { "raw-uart", required_argument, 0, 0x102 },
        { 0, 0, 0, 0 }
    };

    /* Legacy: -U/-N/-t/-E akzeptieren optional einen Backend-Name-Prefix
     * `name=value` (z.B. `rfusb=1b1f:c020`).  Lean bmcond hat nur ein
     * Backend — der Name wird ignoriert, das value isoliert.  Erlaubt
     * headlessCCU's run.sh ihre `name=`-Convention unverändert zu nutzen.
     * `name` darf nur [a-zA-Z0-9_-] enthalten; sonst kein Strip (für
     * USB-Specs wie `1b1f:c020` ohne `=` greift's eh nicht). */
    #define STRIP_BACKEND_NAME(dst, cap) do { \
        const char *eq = strchr(optarg, '='); \
        const char *src = optarg; \
        if (eq && eq != optarg) { \
            bool ok = true; \
            for (const char *q = optarg; q < eq; ++q) { \
                if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') \
                      || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { \
                    ok = false; break; \
                } \
            } \
            if (ok) src = eq + 1; \
        } \
        snprintf(dst, cap, "%s", src); \
    } while (0)

    int c;
    while ((c = getopt_long(argc, argv, "t:N:U:E:b:H:S:F:G:vVCDh",
                              long_opts, NULL)) != -1) {
        switch (c) {
        case 't':
            if (be_path[0]) {
                fprintf(stderr, "CONC: nur ein Backend erlaubt (-t/-N/-U/-E)\n");
                return 2;
            }
            STRIP_BACKEND_NAME(be_path, sizeof(be_path));
            /* be_is_uart implied — handled in fallback branch */
            break;
        case 'N':
            if (be_path[0]) {
                fprintf(stderr, "CONC: nur ein Backend erlaubt (-t/-N/-U/-E)\n");
                return 2;
            }
            STRIP_BACKEND_NAME(be_path, sizeof(be_path));
            be_is_tcp = 1;
            break;
        case 'U':
            if (be_path[0]) {
                fprintf(stderr, "CONC: nur ein Backend erlaubt (-t/-N/-U/-E)\n");
                return 2;
            }
            STRIP_BACKEND_NAME(be_path, sizeof(be_path));
            if (!looks_like_vid_pid(be_path)) {
                fprintf(stderr, "CONC: -U erwartet VID:PID hex (got '%s')\n", be_path);
                return 2;
            }
            be_is_usb = 1;
            break;
        case 'E':
            if (be_path[0]) {
                fprintf(stderr, "CONC: nur ein Backend erlaubt (-t/-N/-U/-E)\n");
                return 2;
            }
            STRIP_BACKEND_NAME(be_path, sizeof(be_path));
            be_is_eth = 1;
            break;
        case 'b': {
            int b = atoi(optarg);
            switch (b) {
            case 9600:   baud = B9600;   break;
            case 19200:  baud = B19200;  break;
            case 38400:  baud = B38400;  break;
            case 57600:  baud = B57600;  break;
            case 115200: baud = B115200; break;
            case 230400: baud = B230400; break;
            default: fprintf(stderr, "unsupported baud: %s\n", optarg); return 2;
            }
            break;
        }
        case 'H':
            if (parse_hmid(optarg, bridge.hmid) < 0) {
                fprintf(stderr, "bad HMID: %s\n", optarg); return 2;
            }
            break;
        case 'S':
            if (strlen(optarg) != 10) {
                fprintf(stderr, "serial must be exactly 10 chars\n"); return 2;
            }
            memcpy(bridge.serial, optarg, 10);
            bridge.serial[10] = 0;
            break;
        case 'F':
            if (parse_firmware(optarg, bridge.firmware) < 0) {
                fprintf(stderr, "bad firmware version\n"); return 2;
            }
            break;
        case 'G':
            if (parse_sgtin(optarg, bridge.sgtin) < 0) {
                fprintf(stderr, "bad SGTIN (need 24 hex chars): %s\n", optarg); return 2;
            }
            break;
        case 'C': conf_emit = 1; break;
        case 'D': conf_dry  = 1; break;
        case 'v': verbose = 1; break;
        case 'V': verbose = 1; very_verbose = 1; break;
        case 0x102:
            snprintf(raw_uart_link, sizeof(raw_uart_link), "%s", optarg);
            break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    /* Default: HmIP-RFUSB via libusb-direct (`1b1f:c020`).  Das ist der
     * canonical use-case — lokaler eq-3-Stick am USB-Port.  User kann
     * jeden anderen Transport mit -t/-N/-U/-E explizit setzen. */
    if (!be_path[0]) {
        snprintf(be_path, sizeof(be_path), "1b1f:c020");
        be_is_usb = 1;
        fprintf(stderr, "CONC: kein Backend angegeben — Default -U 1b1f:c020 (HmIP-RFUSB)\n");
    }
    if (!raw_uart_link[0]) {
        fprintf(stderr, "CONC: --raw-uart=<linkpath> ist Pflicht\n");
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Early API-Start — JSON-API ist verfügbar bevor (potenziell lange
     * retryende) Transport-Setup beginnt. */
    char ts_buf[256] = "";
    const char *cfg_path     = getenv("BMCOND_CFG_PATH");
    const char *sources_path = getenv("BMCOND_SOURCES_JSON");
    if (!sources_path || !*sources_path) {
        sources_path = (access("/data/etc-config", W_OK) == 0)
                     ? "/data/etc-config/bmcond.sources.json"
                     : "/etc/bmcond/sources.json";
    }
    const char *transport_letter = be_is_usb ? "usb"
                                 : be_is_tcp ? "tcp"
                                 : be_is_eth ? "eth"
                                 : "uart";
    snprintf(ts_buf, sizeof(ts_buf), "%s=%s", transport_letter, be_path);

    struct api_context apictx = {
        .port           = 9126,
        .bridge         = &bridge,
        .cfg_transport  = ts_buf,
        .cfg_bidcos     = "/dev/mmd_bidcos",
        .cfg_hmip       = "/dev/mmd_hmip",
        .cfg_extra      = (verbose ? (very_verbose ? "-V -C" : "-v -C") : "-C"),
        .cfg_path       = cfg_path,
        .sources_path   = sources_path,
    };
    api_init(&apictx);

    /* Transport-Setup. */
    struct transport *t = NULL;
    if (be_is_usb)       t = transport_usb_new_str(be_path, 115200);
    else if (be_is_tcp)  t = transport_tcp_new(be_path);
    else if (be_is_eth)  t = transport_eth_new(be_path);
    else                 t = transport_uart_new(be_path, baud);

    if (!t) {
        fprintf(stderr, "CONC: transport_new(%s) failed: %s\n",
                be_path, strerror(errno));
        api_shutdown();
        return 1;
    }
    if (transport_open(t) < 0) {
        fprintf(stderr, "CONC: transport_open(%s) failed: %s — retry...\n",
                t->target, strerror(errno));
        if (transport_reconnect(t) < 0) {
            transport_free(t);
            api_shutdown();
            return 1;
        }
    }

    /* Hardware-Identifikation (vor multimacd-Übergabe).
     * Kein boot_to_app im shim-Mode by default — multimacd macht eigenen
     * Boot-Handshake.  Wir machen nur passive hw_identify falls confgen
     * was schreiben soll. */
    hw_radio_info_t hw;
    memset(&hw, 0, sizeof(hw));
    hw_identify(t->fd, t->target, &hw);
    if (hw.devinfo[0]) {
        fprintf(stderr, "CONC: backend = %s  [%s]%s%s\n",
                hw_kind_name(hw.kind), hw.devinfo,
                hw.dual_stack ? "  dual-stack(BidCoS+HmIP)" : "",
                hw.supports_radio_reset ? "  hw-reset" : "");
    } else {
        fprintf(stderr, "CONC: backend = %s\n", t->target);
    }

    /* Boot-to-App + read-only Inventarisierung — nur wenn confgen aktiv
     * ist (sonst wäre der Probe-Verkehr stören für multimacd's eigene
     * Boot-Sequenz). */
    char app_tag[32] = {0};
    uint8_t  probed_sgtin[12] = {0};
    bool     probed_sgtin_valid = false;
    uint8_t  probed_fw[3] = {0};
    bool     probed_fw_valid = false;
    copro_hw_type_t probed_hw = 0;
    bool     probed_hw_valid = false;

    if (conf_emit) {
        fprintf(stderr, "CONC: boot-to-app probe (for confgen)\n");
        if (radio_dualcopro_boot_to_app(t->fd, app_tag, sizeof(app_tag)) == 0) {
            int is_dualcopro = (strstr(app_tag, "DualCoPro_App") != NULL ||
                                strstr(app_tag, "HMIP_TRX_App")  != NULL);
            if (is_dualcopro) {
                if (copro_query_sgtin(t->fd, probed_sgtin, 0) == 0) {
                    probed_sgtin_valid = true;
                    fprintf(stderr,
                        "CONC: verified SGTIN="
                        "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
                        probed_sgtin[0], probed_sgtin[1], probed_sgtin[2],
                        probed_sgtin[3], probed_sgtin[4], probed_sgtin[5],
                        probed_sgtin[6], probed_sgtin[7], probed_sgtin[8],
                        probed_sgtin[9], probed_sgtin[10], probed_sgtin[11]);
                }
                if (copro_query_app_version(t->fd, probed_fw, 0) == 0) {
                    probed_fw_valid = true;
                    fprintf(stderr, "CONC: verified FW=%u.%u.%u\n",
                            probed_fw[0], probed_fw[1], probed_fw[2]);
                }
                if (copro_query_mcu_type(t->fd, &probed_hw, 0) == 0) {
                    probed_hw_valid = true;
                    char sgtin_hex[28] = {0};
                    for (int k = 0; k < 12; ++k)
                        sprintf(sgtin_hex + k*2, "%02X", probed_sgtin[k]);
                    fprintf(stderr,
                        "CONC: verified hw_type=%s (enum=%d)\n",
                        copro_hw_type_name(probed_hw,
                            probed_sgtin_valid ? sgtin_hex : NULL),
                        (int)probed_hw);
                }
            } else if (strstr(app_tag, "Co_CPU_App")) {
                probed_hw = COPRO_HW_HM_MOD_RPI_PCB;
                probed_hw_valid = true;
                fprintf(stderr,
                    "CONC: verified hw_type=HM-MOD-RPI-PCB (via Co_CPU_App tag)\n");
            }
        } else {
            fprintf(stderr, "CONC: WARN — boot-to-app fehlgeschlagen, confgen uses defaults\n");
        }

        /* confgen-Eingang vorbereiten. */
        confgen_backend_t cgbe = {0};
        snprintf(cgbe.name, sizeof(cgbe.name), "%s", "default");
        if (probed_hw_valid) {
            char sgtin_hex[28] = {0};
            for (int k = 0; k < 12; ++k) sprintf(sgtin_hex + k*2, "%02X", probed_sgtin[k]);
            snprintf(cgbe.hw_kind, sizeof(cgbe.hw_kind), "%s",
                     copro_hw_type_name(probed_hw,
                         probed_sgtin_valid ? sgtin_hex : NULL));
        } else {
            snprintf(cgbe.hw_kind, sizeof(cgbe.hw_kind), "%s",
                     hw_kind_name(hw.kind));
        }
        snprintf(cgbe.transport_path, sizeof(cgbe.transport_path), "%s", be_path);
        snprintf(cgbe.app_tag, sizeof(cgbe.app_tag), "%s", app_tag);
        if (probed_fw_valid) memcpy(cgbe.firmware, probed_fw, 3);
        else                  memcpy(cgbe.firmware, bridge.firmware, 3);
        if (probed_sgtin_valid) {
            char *p = cgbe.sgtin;
            for (int k = 0; k < 12 && p < cgbe.sgtin + sizeof(cgbe.sgtin) - 3; ++k) {
                snprintf(p, 3, "%02X", probed_sgtin[k]);
                p += 2;
            }
        } else if (hw.dual_stack) {
            char *p = cgbe.sgtin;
            for (int k = 0; k < 12 && p < cgbe.sgtin + sizeof(cgbe.sgtin) - 3; ++k) {
                snprintf(p, 3, "%02X", bridge.sgtin[k]);
                p += 2;
            }
        }
        snprintf(cgbe.serial, sizeof(cgbe.serial), "%s", bridge.serial);
        snprintf(cgbe.bidcos_address, sizeof(cgbe.bidcos_address),
                 "0x%02X%02X%02X",
                 bridge.hmid[0], bridge.hmid[1], bridge.hmid[2]);
        cgbe.dual_stack = hw.dual_stack;
        /* ComPort-Pfade — im shim-Mode sind das die multimacd-Symlinks. */
        snprintf(cgbe.bidcos_comport, sizeof(cgbe.bidcos_comport), "%s",
                 "/dev/mmd_bidcos");
        if (hw.dual_stack)
            snprintf(cgbe.hmip_comport, sizeof(cgbe.hmip_comport), "%s",
                     "/dev/mmd_hmip");

        confgen_input_t cgin = {
            .backends = &cgbe, .n_backends = 1,
            .backup_before_write = true,
            .dry_run = conf_dry ? true : false,
        };
        fprintf(stderr, "CONC: --conf-emit%s ...\n",
                conf_dry ? " (dry-run)" : "");
        if (confgen_emit(&cgin) < 0) {
            fprintf(stderr, "CONC: WARN — confgen had errors (continuing).\n");
        }
    }

    fprintf(stderr, "CONC: started v" FW_VERSION_STRING "  raw-uart=%s\n",
            raw_uart_link);

    int rc = run_raw_uart_shim(t, raw_uart_link, verbose);

    transport_free(t);
    api_shutdown();
    return rc;
}
