// SPDX-License-Identifier: GPL-2.0-or-later
/* eth_soak.c — Soak-Test-Helper für transport_eth / transport_rfnethm.
 *
 * Hält die UDP-Verbindung gegen eine hb-rf-eth-Box (oder RFNETHM-Clone)
 * offen, liest alle eingehenden Bytes, druckt alle 5 s einen Heartbeat.
 * Beendet auf SIGINT (oder nach optional --duration SECS).
 *
 * Zweck: testet die transport_eth-Liveness-Logic (1 s host keep-alive,
 * 5 s rx watchdog, eth_reconnect) in real conditions, optional unter
 * extern induzierten Disturbs (iptables-drop, AP-reboot etc.).
 *
 * Usage:
 *   eth_soak eth=10.10.11.92        # HB-RF-ETH direkt
 *   eth_soak rfnethm=10.10.11.133   # RFNETHM (mit HTTP-flash_lock-Lifecycle)
 *   eth_soak <spec> --duration 60   # max-runtime
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "transport.h"
#include "transport_eth.h"
#include "transport_rfnethm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    /* Force line-buffering on stderr so heartbeats are visible
     * in real-time when redirected to a file — otherwise libc
     * full-buffers and external scripts polling the log see nothing
     * until soak exits (and flushes). */
    setvbuf(stderr, NULL, _IOLBF, 0);

    const char *spec = NULL;
    const char *dump_rx_path = NULL;
    int max_dur_s = 0;   /* 0 = unlimited */
    int noreset = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            max_dur_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump-rx") == 0 && i + 1 < argc) {
            dump_rx_path = argv[++i];
        } else if (strcmp(argv[i], "--noreset") == 0) {
            noreset = 1;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "unknown opt %s\n", argv[i]);
            return 2;
        } else {
            spec = argv[i];
        }
    }
    if (!spec) {
        fprintf(stderr,
            "Usage: eth_soak <spec> [--duration SECS] [--dump-rx FILE] [--noreset]\n"
            "  spec      = eth=host[:port] | rfnethm=host[:http_port]\n"
            "  --dump-rx writes per-read frame log as <monotonic_ms> <hex>\n"
            "            für Sniff-Diff zwischen zwei parallelen Captures.\n"
            "  --noreset (eth= only) skip cmd=4 reset-pulse beim open(); useful\n"
            "            for passive sniffing wenn das Modul bereits in App-Mode\n"
            "            ist und in App-Mode bleiben soll (rfnethm= macht das ohnehin).\n");
        return 2;
    }

    FILE *dump_fp = NULL;
    if (dump_rx_path) {
        dump_fp = fopen(dump_rx_path, "w");
        if (!dump_fp) { perror("fopen --dump-rx"); return 1; }
        setvbuf(dump_fp, NULL, _IOLBF, 0);
    }

    struct transport *t = NULL;
    if (strncmp(spec, "eth=", 4) == 0)
        t = noreset ? transport_eth_new_noreset(spec + 4) : transport_eth_new(spec + 4);
    else if (strncmp(spec, "rfnethm=", 8) == 0)
        t = transport_rfnethm_new(spec + 8);   /* uses noreset internally */
    else {
        fprintf(stderr, "spec must start with eth= or rfnethm=\n");
        return 2;
    }
    if (!t) { perror("transport_*_new"); return 1; }

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    if (transport_open(t) < 0) {
        fprintf(stderr, "transport_open: %s\n", strerror(errno));
        transport_free(t);
        return 1;
    }

    fprintf(stderr, "eth_soak: opened — fd=%d target=%s label=%s\n",
            t->fd, t->target, t->label);

    uint64_t t0 = now_ms();
    uint64_t last_beat = t0;
    uint64_t last_rx = t0;
    uint64_t total_rx = 0;
    uint64_t rx_since_beat = 0;

    while (!g_stop) {
        if (max_dur_s > 0 && (now_ms() - t0) / 1000 >= (uint64_t)max_dur_s) {
            fprintf(stderr, "eth_soak: --duration reached, stopping\n");
            break;
        }
        struct pollfd p = { .fd = t->fd, .events = POLLIN };
        int r = poll(&p, 1, 1000);
        uint64_t now = now_ms();
        if (r > 0 && (p.revents & POLLIN)) {
            uint8_t buf[1024];
            ssize_t n = read(t->fd, buf, sizeof(buf));
            if (n > 0) {
                total_rx += n;
                rx_since_beat += n;
                last_rx = now;
                if (dump_fp) {
                    fprintf(dump_fp, "%llu ", (unsigned long long)now);
                    for (ssize_t i = 0; i < n; i++) fprintf(dump_fp, "%02x", buf[i]);
                    fprintf(dump_fp, "\n");
                }
            } else if (n == 0) {
                fprintf(stderr, "eth_soak: fd EOF — transport_close fired\n");
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                fprintf(stderr, "eth_soak: read err: %s\n", strerror(errno));
                break;
            }
        }
        if (now - last_beat >= 5000) {
            fprintf(stderr,
                "eth_soak: heartbeat t=+%llus  rx=%llu (this 5s: %llu)  last_rx=%llums ago\n",
                (unsigned long long)((now - t0) / 1000),
                (unsigned long long)total_rx,
                (unsigned long long)rx_since_beat,
                (unsigned long long)(now - last_rx));
            last_beat = now;
            rx_since_beat = 0;
        }
    }

    fprintf(stderr, "eth_soak: closing — total_rx=%llu bytes over %llus\n",
            (unsigned long long)total_rx,
            (unsigned long long)((now_ms() - t0) / 1000));
    if (dump_fp) fclose(dump_fp);
    transport_close(t);
    transport_free(t);
    return 0;
}
