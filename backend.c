// SPDX-License-Identifier: GPL-2.0-or-later
/* backend.c — Composite Backend, Lifecycle und Read-Loop
 *
 * Stub-Implementierung für v0.3.x Phase 1: Klassen sind angelegt, aber
 * concentrator.c nutzt sie noch nicht (Migration in Phase 3). Dieser
 * Code wird grün-kompiliert ins Binary linked, ist aber bisher nur
 * via direkten Tests aufrufbar.
 */

#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

backend_t *backend_new(const char *name,
                       struct transport *transport,
                       const char *radio_name)
{
    if (!name || !transport || !radio_name) { errno = EINVAL; return NULL; }
    const radio_ops_t *r = radio_ops_by_name(radio_name);
    if (!r) {
        fprintf(stderr, "backend_new: unknown radio type '%s'\n", radio_name);
        errno = ENOENT;
        return NULL;
    }
    backend_t *be = calloc(1, sizeof(*be));
    if (!be) return NULL;
    snprintf(be->name, sizeof(be->name), "%s", name);
    be->transport = transport;
    be->radio     = r;
    be->index     = -1;
    hmu_decoder_init(&be->decoder, NULL, NULL);  /* cb wird in Phase 3 gesetzt */
    return be;
}

int backend_open(backend_t *be)
{
    if (!be || !be->transport || !be->radio) { errno = EINVAL; return -1; }

    if (transport_open(be->transport) < 0) {
        fprintf(stderr, "backend[%s]: transport open failed: %s\n",
                be->name, strerror(errno));
        return -1;
    }

    if (be->radio->boot) {
        radio_boot_result_t br = be->radio->boot(be);
        if (br != RADIO_BOOT_OK) {
            fprintf(stderr, "backend[%s]: radio boot failed (%d)\n",
                    be->name, br);
            be->boot_failures++;
            return -2;
        }
    }

    if (be->radio->identify) {
        if (be->radio->identify(be, &be->info) < 0) {
            fprintf(stderr, "backend[%s]: radio identify failed\n", be->name);
            return -3;
        }
        fprintf(stderr,
                "backend[%s]: %s = '%s' app=%s fw=%u.%u.%u caps=0x%02x\n",
                be->name, be->radio->name,
                be->info.name, be->info.app_tag,
                be->info.firmware[0], be->info.firmware[1], be->info.firmware[2],
                be->info.caps_actual);
    }

    return 0;
}

void backend_close(backend_t *be)
{
    if (!be) return;
    if (be->transport) transport_close(be->transport);
}

void backend_free(backend_t *be)
{
    if (!be) return;
    backend_close(be);
    if (be->transport) transport_free(be->transport);
    /* radio ist static, llmac-table-Eigentum kommt in Phase 3 */
    free(be);
}

void backend_run_step(backend_t *be)
{
    if (!be || !be->transport || be->transport->fd < 0) return;
    uint8_t buf[512];
    ssize_t n = read(be->transport->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "backend[%s]: read error: %s\n",
                    be->name, strerror(errno));
        }
        return;
    }
    hmu_decoder_feed(&be->decoder, buf, (size_t)n);
    /* Per-Frame-Callback wurde im decoder bei _init gesetzt — pro Frame
     * läuft on_frame_from_module(be, ...). In Phase 1 ist cb=NULL,
     * Decoder läuft aber konsistent (CRC-tracking, stats). */
    be->frames_rx++;
}

int backend_write_raw(backend_t *be, const uint8_t *bytes, size_t n)
{
    if (!be || !be->transport || be->transport->fd < 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t off = 0;
    while ((size_t)off < n) {
        ssize_t w = write(be->transport->fd, bytes + (size_t)off, n - (size_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return -1;
        }
        off += w;
    }
    be->frames_tx++;
    return 0;
}
