// SPDX-License-Identifier: GPL-2.0-or-later
/* UART-Transport (lokales Char-Device, z.B. /dev/ttyAMA0).
 *
 * Implementiert struct transport_ops mit termios-Setup, Reconnect-Open
 * und optionalem GPIO-Reset des angeschlossenen Funkmoduls.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef BUSMATIC_TRANSPORT_UART_H
#define BUSMATIC_TRANSPORT_UART_H

#include "transport.h"

/* device: Pfad zu Char-Device (z.B. "/dev/ttyAMA0"). baud: termios B*-Konstante. */
struct transport *transport_uart_new(const char *device, int baud);

/* GPIO-Reset des Moduls. Funktional an den UART-Pfad gekoppelt
 * (Reset-Pin ist nur lokal verdrahtet), daher hier — auch wenn der
 * Concentrator über TCP läuft, kann er auf einem Pi mit lokal
 * angestecktem Modul den Reset selbst machen.
 *
 * chip = "/dev/gpiochipN", line = BCM-Pin. Profil: 50ms LOW, 50ms HIGH,
 * Release auf input. Returns 0/-1. */
int transport_uart_reset_via_gpio(const char *chip, int line);

#endif
