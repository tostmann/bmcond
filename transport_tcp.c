// SPDX-License-Identifier: GPL-2.0-or-later
/* TCP-Transport — implementiert struct transport_ops via getaddrinfo+connect. */

#include "transport_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

struct tcp_priv {
    char *host;
    char *port;
};

static int tcp_open_op(struct transport *t)
{
    struct tcp_priv *p = t->priv;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    int gai = getaddrinfo(p->host, p->port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "TCP: getaddrinfo(%s:%s) → %s\n",
                p->host, p->port, gai_strerror(gai));
        errno = EHOSTUNREACH;
        return -1;
    }

    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) { saved_errno = errno; continue; }
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        errno = saved_errno;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    t->fd = fd;
    return 0;
}

static void tcp_close_op(struct transport *t)
{
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

static void tcp_free_op(struct transport *t)
{
    if (!t) return;
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
    }
    struct tcp_priv *p = t->priv;
    if (p) {
        free(p->host);
        free(p->port);
        free(p);
    }
    free(t);
}

static const struct transport_ops tcp_ops = {
    .open  = tcp_open_op,
    .close = tcp_close_op,
    .free  = tcp_free_op,
};

struct transport *transport_tcp_new(const char *host_port)
{
    if (!host_port) { errno = EINVAL; return NULL; }
    /* Letzter ':' trennt host:port (für IPv6-Hostnames). */
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port) { errno = EINVAL; return NULL; }
    size_t hlen = (size_t)(colon - host_port);
    if (hlen == 0 || *(colon + 1) == 0) { errno = EINVAL; return NULL; }

    struct transport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    struct tcp_priv  *p = calloc(1, sizeof(*p));
    if (!p) { free(t); return NULL; }

    p->host = strndup(host_port, hlen);
    p->port = strdup(colon + 1);
    if (!p->host || !p->port) {
        free(p->host); free(p->port); free(p); free(t);
        return NULL;
    }
    t->priv = p;
    t->ops  = &tcp_ops;
    t->fd   = -1;
    snprintf(t->target, sizeof(t->target), "%s", host_port);
    snprintf(t->label,  sizeof(t->label),  "tcp");
    return t;
}
