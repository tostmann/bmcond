// SPDX-License-Identifier: GPL-2.0-or-later
/* confgen.h — Konfig-Generator (lean shim-mode)
 *
 * Lean-Mode (multimacd-Shim):
 *   bmcond schreibt nur noch die zwei Files die rfd direkt braucht:
 *     /etc/config/rfd.conf            — [Interface N]-Blöcke
 *     /var/run/bmcd-config.json       — eigene Wahrheit (von JSON-API gelesen)
 *
 *   /var/hm_mode + /etc/config/InterfacesList.xml +
 *   /var/status/debmatic_avoid_multimacd entfallen — die schreibt
 *   multimacd selbst bzw. sind im shim-Mode redundant.
 *
 * Architekturprinzip: confgen ist NICHT-DESTRUKTIV per Default.
 *   - Backups vor jedem Schreiben (`*.bmcd-pre`-Suffix)
 *   - Idempotent: Markers `# bmcd-managed-begin/end` schließen den
 *     bmcd-eigenen Bereich ein. Nur dieser wird bei Re-Run regeneriert.
 *   - User-Edit-bar: Header / sonstige Bereiche bleiben bestehen.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_CONFGEN_H
#define CUL32HM_CONFGEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Per-Backend Identifikations-Snapshot, befüllt von main() nach
 * radio_dualcopro_boot_to_app(). Optional-Felder können leer/0 sein —
 * confgen schreibt dann nur was es weiß. */
typedef struct {
    char     name[16];          /* "rfusb", "hmmod" — Backend-CLI-Name */
    char     hw_kind[32];       /* "HmIP-RFUSB", "HM-MOD-RPI-PCB", ... */
    char     transport_path[128];/* /dev/raw-uart, /dev/ttyUSB0, VID:PID, ... */
    char     app_tag[32];       /* "DualCoPro_App", ... */
    uint8_t  firmware[3];       /* major.minor.patch */
    /* Optional — wenn bekannt aus eq-3-detect_radio_module o.ä.: */
    char     serial[24];        /* leer = unknown */
    char     sgtin[32];         /* leer = unknown */
    char     bidcos_address[16];/* "0xFFD6B4" leer = unknown */
    char     hmip_address[16];  /* "0x15CA92" leer = unknown */
    bool     dual_stack;        /* BidCoS+HmIP vs BidCoS-only */
    /* ComPort-Pfade die rfd in seine [Interface N]-Blöcke schreiben soll.
     * Im shim-Mode sind das typisch /dev/mmd_bidcos bzw. /dev/mmd_hmip
     * — multimacd erzeugt diese Symlinks selbst. */
    char     bidcos_comport[128];
    char     hmip_comport[128]; /* leer = kein HmIP-Endpoint */
} confgen_backend_t;

typedef struct {
    const confgen_backend_t   *backends;
    int                        n_backends;
    /* Output-Pfade. NULL = Default. */
    const char                *path_rfd_conf;         /* default /etc/config/rfd.conf */
    const char                *path_bmcd_json;        /* default /var/run/bmcd-config.json */
    bool                       backup_before_write;   /* default true */
    bool                       dry_run;               /* nur loggen, nicht schreiben */
} confgen_input_t;

/* Schreibt alle Konfigs. Returns: 0 ok, -1 wenn ≥1 File fehlgeschlagen
 * (Logging via stderr); andere Files werden trotzdem geschrieben. */
int confgen_emit(const confgen_input_t *in);

#endif
