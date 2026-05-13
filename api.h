// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CUL32HM_API_H
#define CUL32HM_API_H

/* Tiny HTTP/JSON admin endpoint for bmcond, served on TCP port 9126.
 * Provides read-only status + read/write of the persistent config file.
 * Designed to be called from the AddOn's WebUI SPA (xhr fetch). */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "radio_id.h"   /* bridge_state_t — Identitätsdaten only */

/* Lifetime of the pointers inside this struct must outlive api_init —
 * the API holds them by reference and reads them on each request. */
struct api_context {
    int               port;            /* TCP listen port; 0 = use 9126 */
    bridge_state_t   *bridge;          /* HW-Identitätsdaten (HMID/SGTIN/...) */
    /* CLI snapshot — what bmcd currently runs with. */
    const char       *cfg_transport;   /* e.g. "rfusb=/dev/raw-uart" */
    const char       *cfg_bidcos;      /* e.g. "/dev/mmd_bidcos" */
    const char       *cfg_hmip;        /* e.g. "/dev/mmd_hmip" */
    const char       *cfg_extra;       /* e.g. "-C -v" */
    /* Persistent cfg file (POST /api/config writes here). NULL = read-only. */
    const char       *cfg_path;
    /* sources.json path (sources/slots persistence). NULL = sources API
     * disabled (404 on /api/sources*). */
    const char       *sources_path;
};

int  api_init(const struct api_context *ctx);
int  api_listen_fd(void);
void api_handle_accept(void);
void api_shutdown(void);

#endif
