// SPDX-License-Identifier: GPL-2.0-or-later
/* backend.h — physisches Funkmodul = transport × radio × decoder × state
 *
 * Ein `backend` ist eine Composite-Instanz die einen konkreten USB-Stick,
 * UART-HAT-Pin, TCP-Tunnel oder RS485-Bus an einer bestimmten Adresse
 * repräsentiert. Mehrere Backends können in einem bmcd-Prozess parallel
 * laufen (v0.3.x Ziel) — siehe memory/architecture_v3_multi_backend.md.
 *
 * Lifecycle:
 *   backend_new(...)          — composite anlegen
 *   backend_open(be)          — transport_open + radio.boot + radio.identify
 *   backend_run_step(be)      — innere Loop liest fd, feedet decoder
 *   backend_close(be)         — transport_close (radio kein eigenes close)
 *   backend_free(be)          — frei
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_BACKEND_H
#define CUL32HM_BACKEND_H

#include <stdint.h>
#include "frame.h"
#include "radio.h"
#include "transport.h"

/* Forward — concentrator definiert smart_state global; das Backend kennt
 * nur einen opaken Pointer um in den Translator/Endpoints/etc. zu callen. */
struct smart_state;

/* Pro-Backend LLMAC-Translator-State (eigene cnt-Räume).
 * Aktuell global in concentrator.c definiert; wird in v0.3.x hierher
 * migriert — vorerst Forward-Declaration. */
struct llmac_pending_table;

typedef struct backend {
    /* Identität */
    char                 name[16];        /* CLI-Name "rfusb", "hmmod" */
    int                  index;           /* Slot-Index in backends[]-Vektor */

    /* Komponenten */
    struct transport    *transport;       /* uart/tcp/... — Eigentum: backend_free schließt + frees */
    const radio_ops_t   *radio;           /* nicht-Eigentum: stable static radio_ops */
    radio_info_t         info;            /* befüllt von radio.identify() */
    hmu_decoder_t        decoder;         /* DualCoPro-Frame-Decoder (radio_hmw überschreibt das ggf.) */

    /* State */
    struct llmac_pending_table *llmac;    /* per-backend pending-Tabelle (alloc beim Open) */
    void                *parent;          /* opake Reference auf smart_state für callbacks */

    /* Stats */
    uint32_t             frames_rx;
    uint32_t             frames_tx;
    uint32_t             reconnects;
    uint32_t             boot_failures;
} backend_t;

/* Konstruktor. transport wird übernommen (frei beim backend_free).
 * radio_name muss in der radio_ops-Registry existieren (radio_ops_by_name).
 * Returns: malloc'd backend_t oder NULL+errno. */
backend_t *backend_new(const char *name,
                       struct transport *transport,
                       const char *radio_name);

/* Lifecycle: open transport, boot radio, identify radio.
 * Returns: 0 ok, -1 transport open failed, -2 radio boot failed,
 *          -3 radio identify failed. */
int  backend_open (backend_t *be);
void backend_close(backend_t *be);
void backend_free (backend_t *be);

/* Ein Read+Decode-Schritt. Wird aus der zentralen select-Loop aufgerufen.
 * Liest aus be->transport->fd, feedet den Decoder, ruft auf jeden ganzen
 * Frame radio->on_frame_from_module(be, raw, len). */
void backend_run_step(backend_t *be);

/* Schreibt einen vollständig encodierten Frame raus. Nutzt transport->fd
 * direkt — keine Re-Encoding hier. */
int backend_write_raw(backend_t *be, const uint8_t *bytes, size_t n);

#endif
