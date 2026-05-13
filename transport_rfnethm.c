// SPDX-License-Identifier: GPL-2.0-or-later
/* transport_rfnethm.c — RFNETHM-Box-Transport (eth + HTTP-API-Lifecycle)
 *
 * Spec-Quelle: docs/reply_from_rfnethm_2026-05-08_reset_api.md
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "transport_rfnethm.h"
#include "transport_eth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_TIMEOUT_MS 5000

struct rfnethm_state {
    char              host[128];
    int               http_port;
    struct transport *eth;        /* delegate */
    int               held_in_bl; /* 1 = wir haben flash_lock acquired */
};

/* ───── Minimal HTTP/1.0 POST ─────────────────────────────────────────
 * Liefert HTTP-Status-Code (200/501/...) oder -1 bei socket-error.
 * resp_buf nimmt body bis cap-1 bytes (NUL-terminiert).
 *
 * Bewusst schlank — kein keep-alive, kein chunked, kein TLS.  Reicht
 * für unsere zwei API-Calls. */
static int http_post(const char *host, int port, const char *path,
                     const char *body,
                     char *resp_buf, size_t resp_cap)
{
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai) {
        fprintf(stderr, "transport_rfnethm: getaddrinfo(%s:%d): %s\n",
                host, port, gai_strerror(gai));
        return -1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval to = { .tv_sec = HTTP_TIMEOUT_MS / 1000,
                          .tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        int saved = errno; close(fd); freeaddrinfo(res); errno = saved;
        return -1;
    }
    freeaddrinfo(res);

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, body ? strlen(body) : 0, body ? body : "");
    if (n < 0 || (size_t)n >= sizeof(req)) {
        close(fd); errno = E2BIG; return -1;
    }
    ssize_t off = 0;
    while ((size_t)off < (size_t)n) {
        ssize_t w = send(fd, req + off, (size_t)n - (size_t)off, 0);
        if (w <= 0) { if (errno == EINTR) continue; close(fd); return -1; }
        off += w;
    }

    /* Read entire response (HTTP/1.0 + Connection: close → server
     * closes when done, recv returns 0). */
    char resp[4096]; size_t got = 0;
    while (got + 1 < sizeof(resp)) {
        ssize_t r = recv(fd, resp + got, sizeof(resp) - got - 1, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    resp[got] = 0;
    close(fd);

    /* Parse status-line: "HTTP/1.x CODE TEXT" */
    int status = -1;
    if (got >= 12 && memcmp(resp, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Find body (after \r\n\r\n) */
    if (resp_buf && resp_cap > 0) {
        const char *body_start = strstr(resp, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            size_t blen = got - (size_t)(body_start - resp);
            if (blen >= resp_cap) blen = resp_cap - 1;
            memcpy(resp_buf, body_start, blen);
            resp_buf[blen] = 0;
        } else {
            resp_buf[0] = 0;
        }
    }
    return status;
}

/* ───── transport_ops ──────────────────────────────────────────────── */

static int rfnethm_open(struct transport *t)
{
    struct rfnethm_state *st = t->priv;

    /* 1. Acquire flash_lock via HTTP-API.
     * RFNETHM macht intern: HW-Reset, Banner-Tag erfassen, kein
     * CHANGE_APP, flash_lock=true, RX-Fanout aktiv. */
    char resp[1024];
    int code = http_post(st->host, st->http_port, "/api/source/uart/reset",
                         "{\"hold_in_bl\":true}",
                         resp, sizeof(resp));
    if (code != 200) {
        fprintf(stderr,
            "transport_rfnethm: HTTP /api/source/uart/reset hold_in_bl=true → "
            "%d (%s)\n", code, code < 0 ? strerror(errno) : "non-200");
        /* Soft-fail: weitermachen mit eth_open, vielleicht funktioniert
         * der UART-tunnel auch ohne flash_lock — bei normalem Inventory
         * (kein Flash) ist's ohnehin egal. */
    } else {
        fprintf(stderr,
            "transport_rfnethm: hold_in_bl acquired — reply: %s\n", resp);
        st->held_in_bl = 1;
    }

    /* 2. Settle nach HW-Reset — Modul braucht ~800ms bis BL-UART ready
     * (laut RFNETHM-Spec 1.5–4s je nach Modul-Familie + RST-Polaritäts-
     * Discovery; wir geben 1s als sane Default). */
    usleep(1000 * 1000);

    /* 3. Open the underlying transport_eth (noreset-Variante). */
    if (st->eth->ops->open(st->eth) < 0) {
        /* Best-effort release flash_lock */
        if (st->held_in_bl) {
            http_post(st->host, st->http_port, "/api/source/uart/reset",
                      "{\"hold_in_bl\":false}", NULL, 0);
            st->held_in_bl = 0;
        }
        return -1;
    }
    t->fd = st->eth->fd;
    fprintf(stderr,
        "transport_rfnethm: opened (eth-tunnel + flash_lock)\n");
    return 0;
}

static void rfnethm_close(struct transport *t)
{
    struct rfnethm_state *st = t->priv;
    /* Close eth tunnel first — releases UDP socket, helper-thread. */
    st->eth->ops->close(st->eth);
    t->fd = -1;
    /* Then release flash_lock + back to App via HTTP. */
    if (st->held_in_bl) {
        char resp[1024];
        int code = http_post(st->host, st->http_port,
                             "/api/source/uart/reset",
                             "{\"hold_in_bl\":false}",
                             resp, sizeof(resp));
        if (code == 200) {
            fprintf(stderr,
                "transport_rfnethm: hold_in_bl released — reply: %s\n", resp);
        } else {
            fprintf(stderr,
                "transport_rfnethm: WARN release-call HTTP %d — module may "
                "stay in BL (auto-release after timeout)\n", code);
        }
        st->held_in_bl = 0;
    }
}

static void rfnethm_free(struct transport *t)
{
    if (!t) return;
    rfnethm_close(t);
    if (t->priv) {
        struct rfnethm_state *st = t->priv;
        if (st->eth) st->eth->ops->free(st->eth);
        free(st);
    }
    free(t);
}

static const struct transport_ops rfnethm_ops = {
    .open  = rfnethm_open,
    .close = rfnethm_close,
    .free  = rfnethm_free,
};

struct transport *transport_rfnethm_new(const char *host_spec)
{
    if (!host_spec || !*host_spec) { errno = EINVAL; return NULL; }
    struct transport *t = calloc(1, sizeof(*t));
    struct rfnethm_state *st = calloc(1, sizeof(*st));
    if (!t || !st) { free(t); free(st); errno = ENOMEM; return NULL; }
    t->ops = &rfnethm_ops;
    t->fd = -1;
    t->priv = st;
    snprintf(t->label, sizeof(t->label), "rfnethm");
    snprintf(t->target, sizeof(t->target), "%s", host_spec);

    /* Parse host[:http_port]. */
    const char *colon = strchr(host_spec, ':');
    if (colon) {
        size_t hl = (size_t)(colon - host_spec);
        if (hl >= sizeof(st->host)) hl = sizeof(st->host) - 1;
        memcpy(st->host, host_spec, hl);
        st->host[hl] = 0;
        st->http_port = atoi(colon + 1);
        if (st->http_port <= 0 || st->http_port > 65535)
            st->http_port = TRANSPORT_RFNETHM_DEFAULT_HTTP_PORT;
    } else {
        snprintf(st->host, sizeof(st->host), "%s", host_spec);
        st->http_port = TRANSPORT_RFNETHM_DEFAULT_HTTP_PORT;
    }

    /* Compose underlying eth transport (UDP-port 3008 fixed via
     * transport_eth-Default).  host:3008 als spec. */
    char eth_spec[140];
    snprintf(eth_spec, sizeof(eth_spec), "%s", st->host);
    /* _noreset-Variante: HW-Reset macht der RFNETHM-HTTP-API-Call,
     * transport_eth's eingebauter cmd=4 wäre redundant + könnte den
     * Modul-Boot-Cycle nochmal anstoßen wenn das Timing schlecht passt. */
    st->eth = transport_eth_new_noreset(eth_spec);
    if (!st->eth) {
        free(st); free(t); errno = ENOMEM; return NULL;
    }
    return t;
}
