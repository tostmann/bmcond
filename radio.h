// SPDX-License-Identifier: GPL-2.0-or-later
/* radio.h — Funkmodul-Treiber-Klasse (vtable + Capabilities)
 *
 * Ein `radio_module` repräsentiert das Protokoll-Verhalten eines Funkmodul-
 * Typs am anderen Ende eines `transport`. Beispiele:
 *
 *   - dualcopro:  HM-MOD-RPI-PCB / HmIP-RFUSB / RPI-RF-MOD
 *                 (DualCoPro-Frame-Format, COMMON_IDENTIFY-Boot, BidCoS+HmIP)
 *   - hmuartlgw:  Legacy Co_CPU_App-Pfad, BidCoS-only
 *   - hmw:        HomeMatic Wired über RS485, HM485-Frames
 *   - cul / cul32: culfw-Befehle (ASCII-Protokoll, kein DualCoPro)
 *   - hmlgw2:     HomeMatic LAN-Gateway, binary über TCP
 *
 * Der `transport` (transport.h) sagt WO der Strom fließt (UART/TCP/USB-bulk),
 * der `radio_module` sagt WAS protokollarisch dahinterhängt. Beide werden in
 * `struct backend` (backend.h) zu einem konkreten "physischen Modul"
 * komponiert.
 *
 * Composite: backend = transport × radio × decoder × translator-state.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_RADIO_H
#define CUL32HM_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct backend;   /* forward — radio operiert auf Backend-Kontext */

/* Capabilities-Bitmask. Endpoints fragen über `radio_supports()` ob ein
 * Modul ihre Rolle bedienen kann (z.B. ein hmip-Endpoint braucht Module
 * mit RADIO_CAP_HMIP_RX|TX). */
typedef uint32_t radio_caps_t;
#define RADIO_CAP_BIDCOS_RX    (1u << 0)
#define RADIO_CAP_BIDCOS_TX    (1u << 1)
#define RADIO_CAP_HMIP_RX      (1u << 2)
#define RADIO_CAP_HMIP_TX      (1u << 3)
#define RADIO_CAP_HMW          (1u << 4)   /* HomeMatic Wired (RS485) */
#define RADIO_CAP_CULFW        (1u << 5)   /* culfw-ASCII-Protokoll */
#define RADIO_CAP_DUALCOPRO    (1u << 6)   /* DualCoPro-Framing (0xfd magic) */
#define RADIO_CAP_INTERCOM     (1u << 7)   /* InterCom-Plane reserviert */

/* Boot-Resultat. */
typedef enum {
    RADIO_BOOT_OK,           /* Modul lebt, im App-Mode */
    RADIO_BOOT_FAILED,       /* Modul antwortet nicht / ist tot */
    RADIO_BOOT_NEEDS_RESET,  /* Modul ist in komischem state — externer Reset nötig */
} radio_boot_result_t;

/* Identifikations-Info die `radio_identify()` zurückliefert.
 * Genug für Logging und für radio.supports-Pre-Check (falls die Capabilities
 * vom konkreten Modul-Untertyp abhängen — z.B. HmIP-RFUSB ist dual-stack,
 * HM-MOD-RPI-PCB ist BidCoS-only obwohl beide dualcopro-radios sind). */
typedef struct {
    char         name[32];          /* "HM-MOD-RPI-PCB", "HmIP-RFUSB", ... */
    char         app_tag[32];       /* "DualCoPro_App", "HMIP_TRX_App", ... */
    uint8_t      firmware[3];       /* major.minor.patch */
    radio_caps_t caps_actual;       /* verifiziert nach Identifikation */
} radio_info_t;

/* vtable. Implementierungen: src/radio_dualcopro.c, src/radio_hmuartlgw.c, ... */
typedef struct radio_ops {
    /* Stable identifier — z.B. "dualcopro", "hmuartlgw", "hmw". CLI-Match. */
    const char *name;

    /* Capabilities die dieser radio-Typ *theoretisch* kann. Konkrete
     * Verfügbarkeit prüft `identify()` (z.B. HmIP-RFUSB-TK vs RFUSB). */
    radio_caps_t caps_advertised;

    /* Boot/Identify pipeline. Beide bekommen das Backend (mit offenem fd
     * im transport) und schreiben ihren Zustand zurück.
     *   boot()      — bringt das Modul in App-Mode (CHANGE_APP wenn BL etc.)
     *   identify()  — füllt info.* (name, app_tag, firmware, caps_actual)
     * Reihenfolge: open → boot → identify → run.
     */
    radio_boot_result_t (*boot)    (struct backend *be);
    int                 (*identify)(struct backend *be, radio_info_t *info);

    /* Optionaler hardware-reset-Hook. Gibt 0 zurück wenn unterstützt
     * (typisch via piVCCU-IOCRESET oder GPIO), -1 bei ENOSYS. */
    int                 (*hw_reset)(struct backend *be);

    /* Per-radio Frame-Handler. Wird aufgerufen wenn der Backend-Decoder
     * einen vollständigen Frame entschlüsselt hat. Default-Impl
     * (dualcopro) routet basierend auf `dst` zwischen mux-lokal,
     * Endpoint-Forwarding und LLMAC-Translator. Andere radio-Typen
     * (hmw, culfw) parsen statt dualcopro-Frames ihr eigenes Format. */
    void (*on_frame_from_module)(struct backend *be,
                                 const uint8_t *raw, size_t raw_len);
} radio_ops_t;

/* Lookup nach name (CLI -t name=path:type). NULL wenn unbekannt. */
const radio_ops_t *radio_ops_by_name(const char *name);

/* Liste alle registrierten radio-Typen — für `--help` und Diagnostik. */
const radio_ops_t * const *radio_ops_all(size_t *out_count);

#endif
