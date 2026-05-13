// SPDX-License-Identifier: GPL-2.0-or-later
/* transport_rfnethm.h — RFNETHM-Box (ESP32 UART-Bridge) Transport
 *
 * Komposition aus:
 *   1. transport_eth (UDP cmd=7 für UART-bytes, cmd=4 für RST-Pulse)
 *   2. HTTP-API-Aufrufe für RFNETHM-spezifische Lifecycle:
 *      - POST /api/source/uart/reset {"hold_in_bl":true}  beim open
 *      - POST /api/source/uart/reset {"hold_in_bl":false} beim close
 *
 * Damit:
 *   - flash_lock wird vor flash gesetzt (= USB-Hot-Swap-Schutz)
 *   - cmd=4 bleibt piVCCU-konform (pure HW-Pulse, kein flash-side-effect)
 *   - Nach close: Modul kommt automatisch wieder in App-Mode
 *
 * Spec-Quelle: docs/reply_from_rfnethm_2026-05-08_reset_api.md
 *
 * CLI-Form (concentrator): `-R rfnethm=host[:port]`
 *   Default: HTTP-Port 80, UDP-Port 3008
 *   Beispiel: -R rfnethm=rfnethm-DE54.local
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_TRANSPORT_RFNETHM_H
#define CUL32HM_TRANSPORT_RFNETHM_H

#include "transport.h"

/* Default HTTP-Port für RFNETHM-API. */
#define TRANSPORT_RFNETHM_DEFAULT_HTTP_PORT 80
/* Default UDP-Port für hb_rf_eth-Wire (von transport_eth.h). */

/* host_spec: "host" oder "host:http_port".  UDP-Port immer 3008.
 * Returns: transport oder NULL+errno. */
struct transport *transport_rfnethm_new(const char *host_spec);

#endif
