// SPDX-License-Identifier: GPL-2.0-or-later
/* transport_eth.c — UDP-Transport für HB-RF-ETH-Boxen
 *
 * Spec-Quelle: piVCCU `hb_rf_eth.c` (GPLv2-or-later, Alexander Reinert).
 * Wire-protocol details siehe `transport_eth.h`.
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "transport_eth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ETH_BUF_SIZE  2048
#define HB_RF_ETH_PROTOCOL_VERSION 2

struct eth_state {
    char           host[128];
    int            port;
    int            udp_fd;
    int            caller_fd;     /* socketpair[0], handed via t->fd */
    int            helper_fd;     /* socketpair[1], used by thread */
    pthread_t      thread;
    int            thread_started;
    volatile int   stop_flag;
    pthread_mutex_t udp_lock;     /* serializes write to udp_fd from
                                   * helper-thread + main (reset_pulse) */
    uint8_t        ourEID;
    uint8_t        boxEID;
    int            connected;
    int            skip_reset;    /* 1 = transport_eth_new_noreset variant */
};

/* Forward decl — definition further below; needed because eth_thread_fn
 * (defined before the reconnect helper) drives the reconnect loop. */
static int eth_reconnect(struct eth_state *st);

/* Monotonic clock helper — for rx-watchdog + host-side keep-alive timing.
 * CLOCK_MONOTONIC ignores wall-clock changes (NTP step etc.) which is
 * what we want for liveness timers. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Same CRC as piVCCU hb_rf_eth_calc_crc + bmcond hmu_crc16. */
static uint16_t eth_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xd77f;
    while (len--) {
        crc ^= (uint16_t)(*buf++) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) { crc <<= 1; crc ^= 0x8005; }
            else              { crc <<= 1; }
        }
    }
    return crc;
}

/* Encode + send UDP packet.  payload (cmd + seq + data) is N bytes;
 * we append 2 bytes CRC and send total = N+2 bytes.  Caller must hold
 * udp_lock if multi-threaded. */
static int eth_send_packet(struct eth_state *st,
                           uint8_t cmd, uint8_t seq,
                           const uint8_t *data, size_t data_len)
{
    if (data_len + 4 > ETH_BUF_SIZE) { errno = E2BIG; return -1; }
    uint8_t pkt[ETH_BUF_SIZE];
    pkt[0] = cmd;
    pkt[1] = seq;
    if (data_len) memcpy(pkt + 2, data, data_len);
    uint16_t crc = eth_crc16(pkt, 2 + data_len);
    pkt[2 + data_len + 0] = (uint8_t)(crc >> 8);
    pkt[2 + data_len + 1] = (uint8_t)(crc & 0xff);
    ssize_t n = send(st->udp_fd, pkt, 4 + data_len, 0);
    return (n == (ssize_t)(4 + data_len)) ? 0 : -1;
}

static int eth_recv_packet_blocking(struct eth_state *st,
                                    uint8_t *out, size_t cap,
                                    int timeout_ms)
{
    struct pollfd p = { .fd = st->udp_fd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    ssize_t n = recv(st->udp_fd, out, cap, 0);
    if (n < 4) return -1;
    /* Verify CRC */
    uint16_t got = ((uint16_t)out[n-2] << 8) | out[n-1];
    uint16_t want = eth_crc16(out, n - 2);
    if (got != want) {
        fprintf(stderr, "transport_eth: rx packet CRC mismatch (got 0x%04x want 0x%04x)\n",
                got, want);
        return -1;
    }
    return (int)n;
}

/* Helper thread: poll UDP + caller-pipe; relay cmd=7 bytes both ways.
 *
 * Liveness model (mirrors piVCCU hb_rf_eth.c kernel-driver):
 *   - We send cmd=2 host→box every 1 s (keeps NAT state-table warm,
 *     tells box we are alive).
 *   - We track last_rx_ms — any valid box→host packet (cmd=2 or cmd=7)
 *     resets it.
 *   - 5 s of rx-silence ⇒ declare connection dead ⇒ eth_reconnect()
 *     with 500 ms→5 s exponential backoff.  Reconnect re-handshakes
 *     and re-issues cmd=5 (UART tunnel) but NOT cmd=4 (would reset
 *     the radio module and trash runtime state).
 *   - Caller bytes that arrive during a reconnect window are buffered
 *     by the socketpair until its buffer fills, then caller backpressures.
 *     Upper-layer protocols (AskSin/HmIP) handle losses via their own
 *     ACK/retry mechanisms. */
static void *eth_thread_fn(void *arg)
{
    struct eth_state *st = arg;
    uint8_t pkt[ETH_BUF_SIZE];
    uint8_t buf[1024];
    uint8_t tx_seq = 0;
    uint64_t last_rx_ms = monotonic_ms();
    uint64_t last_keepalive_tx_ms = 0;
    while (!st->stop_flag) {
        uint64_t now = monotonic_ms();

        if (now - last_keepalive_tx_ms >= 1000) {
            pthread_mutex_lock(&st->udp_lock);
            eth_send_packet(st, 2, 0, NULL, 0);
            pthread_mutex_unlock(&st->udp_lock);
            last_keepalive_tx_ms = now;
        }

        if (now - last_rx_ms > 5000) {
            fprintf(stderr,
                "transport_eth: no UDP rx for %llu ms → reconnecting\n",
                (unsigned long long)(now - last_rx_ms));
            if (eth_reconnect(st) < 0) break;   /* stop_flag set during retry */
            last_rx_ms = monotonic_ms();
            last_keepalive_tx_ms = last_rx_ms;
            continue;
        }

        struct pollfd fds[2] = {
            { .fd = st->udp_fd,    .events = POLLIN },
            { .fd = st->helper_fd, .events = POLLIN },
        };
        int r = poll(fds, 2, 500);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (st->stop_flag) break;
        if (r == 0) continue;
        if (fds[0].revents & POLLIN) {
            ssize_t n = recv(st->udp_fd, pkt, sizeof(pkt), 0);
            if (n >= 4) {
                uint16_t got = ((uint16_t)pkt[n-2] << 8) | pkt[n-1];
                uint16_t want = eth_crc16(pkt, n - 2);
                if (got == want) {
                    /* Any well-formed packet (cmd=2 keep-alive or cmd=7
                     * UART data) is liveness evidence — piVCCU does the
                     * same in recv_threadproc. */
                    last_rx_ms = monotonic_ms();
                    uint8_t cmd = pkt[0];
                    if (cmd == 7 && n > 4) {
                        /* UART-bytes from box → forward to caller */
                        ssize_t off = 0;
                        size_t inner = (size_t)n - 4;  /* skip cmd, seq, crc */
                        while ((size_t)off < inner) {
                            ssize_t w = write(st->helper_fd, pkt + 2 + off,
                                              inner - off);
                            if (w <= 0) { if (errno == EINTR) continue; break; }
                            off += w;
                        }
                    }
                    /* cmd 2 = keep-alive, already counted as liveness */
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(st->helper_fd, buf, sizeof(buf));
            if (n > 0) {
                pthread_mutex_lock(&st->udp_lock);
                eth_send_packet(st, 7, tx_seq++, buf, (size_t)n);
                pthread_mutex_unlock(&st->udp_lock);
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                /* peer closed or error → exit thread */
                break;
            }
        }
    }
    return NULL;
}

/* Public reset-pulse — synchron, sendet UDP cmd=4 + 100ms settle. */
static int eth_reset_pulse(struct eth_state *st)
{
    pthread_mutex_lock(&st->udp_lock);
    int rc = eth_send_packet(st, 4, 0, NULL, 0);
    pthread_mutex_unlock(&st->udp_lock);
    fprintf(stderr, "transport_eth: reset_pulse → cmd=4 (rc=%d)\n", rc);
    if (rc == 0) usleep(100 * 1000);  /* settle */
    return rc;
}

/* Connect-handshake gegen die Box.  Returns 0 ok, -1 error. */
static int eth_handshake(struct eth_state *st)
{
    /* Send: 6 bytes [0][seq=0][PROTOVER=2][endpointIdentifier] + 2 CRC.
     * piVCCU's connect-buffer war 6 bytes mit den letzten 2 als CRC-slot.
     *
     * endpointIdentifier = st->boxEID: at initial open st->boxEID is 0
     * (calloc), so the box sees a fresh-session connect.  On reconnect
     * it holds the EID the box assigned in the previous session — this
     * mirrors piVCCU's try_connect(currentEndpointIdentifier) session-
     * resume pattern. */
    uint8_t hello[4] = { HB_RF_ETH_PROTOCOL_VERSION, st->boxEID, 0, 0 };
    pthread_mutex_lock(&st->udp_lock);
    int sent = eth_send_packet(st, 0, 0, hello, 2);  /* only protover+EID; box echoes them */
    pthread_mutex_unlock(&st->udp_lock);
    if (sent != 0) return -1;

    uint8_t reply[64];
    /* 500 ms — piVCCU uses 50 ms inside a kernel-driver (LAN-only target);
     * we cover WLAN where Phase-0 measured p99.9=267 ms ICMP-RTT, so an
     * occasional stall can exceed 200 ms without indicating real failure. */
    int n = eth_recv_packet_blocking(st, reply, sizeof(reply), 500);
    if (n < 7) {
        fprintf(stderr, "transport_eth: handshake — no reply or too short (n=%d)\n", n);
        return -1;
    }
    /* Reply layout: [0][?][PROTOVER=2][ourEID][boxEID][crc_hi][crc_lo] */
    if (reply[0] != 0 || reply[2] != HB_RF_ETH_PROTOCOL_VERSION) {
        fprintf(stderr, "transport_eth: handshake — unexpected reply[0]=0x%02x [2]=0x%02x\n",
                reply[0], reply[2]);
        return -1;
    }
    st->boxEID = reply[4];
    fprintf(stderr, "transport_eth: handshake OK protover=%u ourEID=%u boxEID=%u\n",
            reply[2], reply[3], reply[4]);
    return 0;
}

/* Open a fresh UDP socket connected to st->host:st->port, store into
 * st->udp_fd.  Caller must have st->udp_fd == -1.  Used by both initial
 * open and reconnect. */
static int eth_connect_socket(struct eth_state *st)
{
    char host[128]; snprintf(host, sizeof(host), "%s", st->host);
    char port[16];  snprintf(port, sizeof(port),  "%d", st->port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai) {
        fprintf(stderr, "transport_eth: getaddrinfo(%s:%s) failed: %s\n",
                host, port, gai_strerror(gai));
        errno = EHOSTUNREACH; return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        int saved = errno; close(fd); freeaddrinfo(res); errno = saved; return -1;
    }
    freeaddrinfo(res);
    st->udp_fd = fd;
    return 0;
}

/* Rx-watchdog tripped — close the old UDP socket and try to re-establish
 * the session.  Loops with 500 ms → 5 s exponential backoff until either
 * success or stop_flag goes high.  On success re-issues cmd=5 to re-arm
 * the UART tunnel (box almost certainly tore down its session-state
 * during the silence).  Deliberately does NOT issue cmd=4 reset — that
 * would re-enter the bootloader and destroy the radio module's runtime
 * state, which is the opposite of what a transparent reconnect should do.
 * The handshake reuses st->boxEID (session-resume hint), matching
 * piVCCU's try_connect(currentEndpointIdentifier) pattern. */
static int eth_reconnect(struct eth_state *st)
{
    if (st->udp_fd >= 0) { close(st->udp_fd); st->udp_fd = -1; }
    int delay_ms = 500;
    while (!st->stop_flag) {
        fprintf(stderr,
            "transport_eth: reconnect attempt → %s:%d (backoff %d ms)\n",
            st->host, st->port, delay_ms);
        if (eth_connect_socket(st) == 0 && eth_handshake(st) == 0) {
            pthread_mutex_lock(&st->udp_lock);
            eth_send_packet(st, 5, 0, NULL, 0);
            pthread_mutex_unlock(&st->udp_lock);
            fprintf(stderr,
                "transport_eth: reconnect OK (boxEID=%u)\n", st->boxEID);
            return 0;
        }
        if (st->udp_fd >= 0) { close(st->udp_fd); st->udp_fd = -1; }
        for (int slept = 0; slept < delay_ms && !st->stop_flag; slept += 100)
            usleep(100 * 1000);
        if (delay_ms < 5000) delay_ms *= 2;
        if (delay_ms > 5000) delay_ms = 5000;
    }
    return -1;
}

/* ───── transport_ops ─────────────────────────────────────────────── */

static int eth_open(struct transport *t)
{
    struct eth_state *st = t->priv;
    if (st->udp_fd >= 0) {
        /* Already open — caller should close first.  No-op: signal success. */
        return 0;
    }

    /* socketpair for caller fd ↔ helper-thread */
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) return -1;
    st->caller_fd = sp[0];
    st->helper_fd = sp[1];

    if (eth_connect_socket(st) < 0) {
        close(sp[0]); close(sp[1]);
        st->caller_fd = -1; st->helper_fd = -1;
        return -1;
    }

    if (eth_handshake(st) < 0) {
        close(st->udp_fd); st->udp_fd = -1;
        close(sp[0]); close(sp[1]);
        st->caller_fd = -1; st->helper_fd = -1;
        errno = ECONNREFUSED;
        return -1;
    }

    pthread_mutex_lock(&st->udp_lock);
    eth_send_packet(st, 5, 0, NULL, 0);   /* start UART tunnel */
    pthread_mutex_unlock(&st->udp_lock);

    /* Reset-pulse so the module comes up in BL — analog zu transport_usb.
     * Skip wenn Caller (z.B. transport_rfnethm) den Reset extern macht.
     * Only at initial open; reconnect never resets (would brick runtime). */
    if (!st->skip_reset) {
        eth_reset_pulse(st);
    } else {
        fprintf(stderr, "transport_eth: skip_reset=1 (caller manages reset)\n");
    }

    /* Spawn helper thread */
    st->stop_flag = 0;
    if (pthread_create(&st->thread, NULL, eth_thread_fn, st) != 0) {
        close(st->udp_fd); close(sp[0]); close(sp[1]);
        st->udp_fd = -1; st->caller_fd = -1; st->helper_fd = -1;
        return -1;
    }
    st->thread_started = 1;

    t->fd = sp[0];
    fprintf(stderr, "transport_eth: opened %s:%d (fd=%d)\n", st->host, st->port, t->fd);
    return 0;
}

static void eth_close(struct transport *t)
{
    struct eth_state *st = t->priv;
    if (st->thread_started) {
        st->stop_flag = 1;
        /* Closing helper_fd will wake the poll() in helper thread. */
        if (st->helper_fd >= 0) { close(st->helper_fd); st->helper_fd = -1; }
        pthread_join(st->thread, NULL);
        st->thread_started = 0;
    }
    /* Clean disconnect — piVCCU sends cmd=1 in hb_rf_eth_disconnect.
     * Without it the box waits for its own 5s keep-alive watchdog to
     * declare us dead (visible as hb.ka_timeouts++ in RFNETHM stats).
     * Best-effort: ignore send errors; we're closing anyway. */
    if (st->udp_fd >= 0) {
        eth_send_packet(st, 1, 0, NULL, 0);
    }
    if (st->caller_fd >= 0) { close(st->caller_fd); st->caller_fd = -1; }
    if (st->helper_fd >= 0) { close(st->helper_fd); st->helper_fd = -1; }
    if (st->udp_fd >= 0)    { close(st->udp_fd);    st->udp_fd    = -1; }
    t->fd = -1;
}

static void eth_free(struct transport *t)
{
    if (!t) return;
    eth_close(t);
    if (t->priv) {
        struct eth_state *st = t->priv;
        pthread_mutex_destroy(&st->udp_lock);
        free(st);
    }
    free(t);
}

static const struct transport_ops eth_ops = {
    .open  = eth_open,
    .close = eth_close,
    .free  = eth_free,
};

static struct transport *transport_eth_new_internal(const char *host_port,
                                                     int skip_reset);

struct transport *transport_eth_new(const char *host_port)
{
    return transport_eth_new_internal(host_port, 0);
}

struct transport *transport_eth_new_noreset(const char *host_port)
{
    return transport_eth_new_internal(host_port, 1);
}

static struct transport *transport_eth_new_internal(const char *host_port,
                                                     int skip_reset)
{
    if (!host_port || !*host_port) { errno = EINVAL; return NULL; }
    struct transport *t = calloc(1, sizeof(*t));
    struct eth_state *st = calloc(1, sizeof(*st));
    if (!t || !st) { free(t); free(st); errno = ENOMEM; return NULL; }
    t->ops = &eth_ops;
    t->fd = -1;
    t->priv = st;
    snprintf(t->label, sizeof(t->label), "eth");
    snprintf(t->target, sizeof(t->target), "%s", host_port);

    /* Parse host:port (default port 3008 if not present). */
    const char *colon = strchr(host_port, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - host_port);
        if (hlen >= sizeof(st->host)) hlen = sizeof(st->host) - 1;
        memcpy(st->host, host_port, hlen);
        st->host[hlen] = 0;
        st->port = atoi(colon + 1);
        if (st->port <= 0 || st->port > 65535) st->port = TRANSPORT_ETH_DEFAULT_PORT;
    } else {
        snprintf(st->host, sizeof(st->host), "%s", host_port);
        st->port = TRANSPORT_ETH_DEFAULT_PORT;
    }
    pthread_mutex_init(&st->udp_lock, NULL);
    st->udp_fd = -1;
    st->caller_fd = -1;
    st->helper_fd = -1;
    st->skip_reset = skip_reset;
    return t;
}
