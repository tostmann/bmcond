// SPDX-License-Identifier: GPL-2.0-or-later
/* USB-Transport — libusb-1.0 direkt zum CP2102N im HmIP-RFUSB.
 *
 * Statt /dev/ttyUSB0 (Mainline cp210x-Kerneltreiber) klemmt sich dieser
 * Transport direkt an den USB-Stack. Damit fällt jede Host-Konfiguration
 * weg (kein Modul-Blacklist, kein udev-Rule, kein new_id-Write); funktioniert
 * auch auf HassOS-Read-Only-Root-FS.
 *
 * Der Mainline cp210x-Treiber muss vom Stick weg sein bevor open() ihn
 * claimen kann. Der Transport ruft selbst libusb_detach_kernel_driver(),
 * solange noch ein Treiber gebunden ist — keine udev-Rule nötig.
 *
 * VID:PID-Form, beide hex, 4 Zeichen je: "1b1f:c020".
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef BUSMATIC_TRANSPORT_USB_H
#define BUSMATIC_TRANSPORT_USB_H

#include "transport.h"

/* Konstruktor. baud: nominaler Baud (115200 für DualCoPro). Returns NULL+errno
 * bei Allokationsfehler oder ungültiger VID/PID (errno=EINVAL). */
struct transport *transport_usb_new(unsigned vid, unsigned pid, int baud);

/* Bequeme Form: "1b1f:c020" parsing. Whitespace und Großbuchstaben ok.
 * NULL bei Parser-Fehler (errno=EINVAL). */
struct transport *transport_usb_new_str(const char *vid_pid, int baud);

/* ─── Discovery-API ──────────────────────────────────────────────────── */

/* Ein gefundener Stick — informationelle View für Discovery, NICHT
 * direkt für transport_usb_new() (das nimmt nur VID/PID/baud). */
struct usb_discovery_hit {
    unsigned vid;
    unsigned pid;
    char     iserial[64];     /* iSerial-Descriptor; "" wenn nicht lesbar */
    char     bus_port[32];    /* "<bus>-<port_path>", stable solange Topology gleich */
    const char *kind_hint;    /* aus quirks-Tabelle: "eQ-3 HmIP-RFUSB", ... */
};

/* Enumeriert alle USB-Devices und liefert die zurück, die in der internen
 * usb_radio_quirks-Tabelle vorkommen.  Schreibt bis zu `max` hits in `out`,
 * gibt Anzahl zurück (oder -1 + errno bei libusb-Init-Fehler).  Versucht
 * den iSerial-Descriptor auszulesen — read-only, kein Treiber-Detach,
 * kein Interface-Claim. */
int transport_usb_discover(struct usb_discovery_hit *out, int max);

#endif
