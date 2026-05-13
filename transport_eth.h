// SPDX-License-Identifier: GPL-2.0-or-later
/* transport_eth.h — UDP-Transport für HB-RF-ETH-style-Module
 *
 * Implementiert das hb_rf_eth-Wire-Protokoll aus piVCCU
 * (`hb_rf_eth.c`, GPLv2-or-later, Alexander Reinert):
 *
 *   UDP nach `<host>:3008` (Default-Port).
 *   Frame:  [cmd:1][seq:1][payload...][CRC16-BE:2]
 *           CRC = piVCCU-poly 0x8005 init 0xd77f, computed über
 *           [cmd][seq][payload].
 *
 *   cmd-Bytes:
 *     0  Connect-Handshake (host → box)
 *     2  Keep-alive (box → host, alle 1s)
 *     3  GPIO-Write (host → box, 1 byte payload = LED-mask)
 *     4  Reset Radio Module (host → box, no payload)
 *     5  Start UART-Connection (host → box, no payload)
 *     7  UART-bytes (host ↔ box, payload = bytes)
 *
 * Connect-Sequenz:
 *   1. host sends [0][0][PROTOVER=2][ourEID=0][0][0]   (6 bytes total
 *      inkl. CRC).  PROTOVER=2 zur Compatibilität mit piVCCU 5.0+.
 *   2. box answers within 50ms with [0][?][2][ourEID][boxEID][crc_hi][crc_lo].
 *      Box-EID merken für später.
 *   3. host sends queue_msg(5, NULL, 0) (= start-UART-Connection-cmd).
 *   4. UART-Tunnel ist offen.  cmd=7-Frames in beide Richtungen.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_TRANSPORT_ETH_H
#define CUL32HM_TRANSPORT_ETH_H

#include "transport.h"

/* Default-Port für hb_rf_eth-Boxen. */
#define TRANSPORT_ETH_DEFAULT_PORT 3008

/* Konstruktor.  `host_port` ist "host:port" oder nur "host" (dann
 * Default-Port).  Returns malloc'd transport oder NULL+errno. */
struct transport *transport_eth_new(const char *host_port);

/* Variante ohne reset_pulse beim open() — nützlich wenn der Caller
 * den Reset auf einer anderen Schicht macht (z.B. transport_rfnethm
 * via HTTP-API).  Sonst identisch zu transport_eth_new. */
struct transport *transport_eth_new_noreset(const char *host_port);

#endif
