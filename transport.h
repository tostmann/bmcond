// SPDX-License-Identifier: GPL-2.0-or-later
/* Transport-Abstraktion — Polymorphismus über Funktions-Pointer-Tabelle.
 *
 * Jeder Transport-Typ (UART, TCP, später UDP, USB-CDC, …) liefert ein
 * `struct transport_ops`-Static und einen `transport_*_new()`-Konstruktor,
 * der ein `struct transport*` heap-alloziert. Concentrator kennt keine
 * konkreten Typen — nur das Interface.
 *
 * Lifecycle:
 *
 *   t = transport_uart_new("/dev/ttyAMA0", B115200);   // konstruieren
 *   transport_open(t);                                  // verbinden, t->fd gesetzt
 *   ... select(t->fd) / read(t->fd) / write(t->fd) ...  // nutzen
 *   transport_close(t);                                 // trennen, fd zu, t bleibt
 *   transport_open(t);                                  // reconnect
 *   transport_free(t);                                  // alles frei
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef BUSMATIC_TRANSPORT_H
#define BUSMATIC_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

struct transport;

struct transport_ops {
    /* Verbindung aufbauen / wiederaufbauen. Returns 0 on success
     * (t->fd ≥ 0 gesetzt), -1 on error (errno gesetzt, t->fd = -1). */
    int  (*open)(struct transport *t);

    /* Verbindung trennen. Setzt t->fd = -1. Idempotent. */
    void (*close)(struct transport *t);

    /* Vollständige Destruktion: priv-state + struct selbst freigeben.
     * Vorher ggf. close aufrufen wenn nötig. */
    void (*free)(struct transport *t);
};

struct transport {
    const struct transport_ops *ops;
    int    fd;        /* -1 wenn nicht offen */
    void  *priv;      /* backend-spezifisch */
    char   target[128];   /* "/dev/ttyAMA0", "127.0.0.1:5000" — fürs Logging */
    char   label[16];     /* "uart", "tcp", "udp" */
};

/* Konstruktoren — in den jeweiligen transport_*.c definiert.
 * Allozieren auf dem Heap; Caller übernimmt Ownership; transport_free()
 * gibt komplett frei. NULL bei Allokationsfehler. */
struct transport *transport_uart_new(const char *device, int baud);
struct transport *transport_tcp_new(const char *host_port);
struct transport *transport_eth_new(const char *host_port);
struct transport *transport_rfnethm_new(const char *host_spec);

/* Thin Wrapper. Inline-fähig, aber als externe Funktionen nicht weniger
 * effizient (compiler inlined ggf. selbst). */
static inline int transport_open(struct transport *t)
    { return t->ops->open(t); }

static inline void transport_close(struct transport *t)
    { t->ops->close(t); }

static inline void transport_free(struct transport *t)
    { if (t) t->ops->free(t); }

#endif
