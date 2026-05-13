// SPDX-License-Identifier: GPL-2.0-or-later
/* TCP-Transport — Concentrator verbindet sich zu host:port (z.B. ser2net),
 * der seinerseits den lokalen UART/Stream ans Funkmodul wrappt.
 *
 * Anwendungsfälle:
 *   - ser2net auf Pi5, /dev/ttyAMA0 → 0.0.0.0:5000. BusMatic-Container
 *     auf anderem Host connectet per TCP (LAN-deployed Concentrator).
 *   - HB-RF-ETH-Devices (Reinert-Hardware) — TCP-Adresse kompatibel, aber
 *     wir reden TCP-raw, nicht das proprietäre Reinert-Wireformat.
 *   - Custom socat-Bridges, ngrok-Tunnels etc.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef BUSMATIC_TRANSPORT_TCP_H
#define BUSMATIC_TRANSPORT_TCP_H

#include "transport.h"

/* host_port: "host:port" (z.B. "127.0.0.1:5000" oder "[::1]:5000"). */
struct transport *transport_tcp_new(const char *host_port);

#endif
