// SPDX-License-Identifier: GPL-2.0-or-later
/* copro_query.h — Read-only DualCoPro Co-CPU-Inventarisierung
 *
 * Stage 2 Iter 1 von Co-CPU-FW-Update-Pfad.  Read-only Identify-/SGTIN-/
 * Versions-Probe gegen ein Modul am offenen UART/USB-fd, OHNE Mode-Switch
 * und OHNE Schreiben in Modul-Flash.
 *
 * Spec: docs/copro_update_protocol_re_2026-05-08.md
 *   Common-Subsystem (dst=0xFE):
 *     cmd=0x01 getIdentify  → ASCII-Tag (mode-agnostic)
 *     cmd=0x04 getSGTIN     → 12 Bytes SGTIN (mode-agnostic)
 *   TRXAdapter-Subsystem (dst=0x01):
 *     cmd=0x02 getVersion   → 3 oder 9 Bytes Version (App-Mode only)
 *   Bootloader-Subsystem (dst=0x00):
 *     cmd=0x02 getVersion   → 3 Bytes Version (BL-Mode only)
 *
 * Aufrufkontext: NACH erfolgreichem radio_dualcopro_boot_to_app(),
 * BEVOR rfd seine PTYs übernimmt — d.h. der Concentrator hat exklusiven
 * Zugriff aufs Modul, kein Counter-Konflikt mit rfd-Frames.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_COPRO_QUERY_H
#define CUL32HM_COPRO_QUERY_H

#include <stdint.h>
#include <stddef.h>

/* Default-Timeouts (ms).  4000 ms entspricht den Java-Tool-Defaults
 * (TRXAdapterUpdater.responseEvent.waitOne(4000L)).  1000 ms reicht für
 * lokal-USB; bei TCP-Tunnel (HB-RF-ETH) sind 4000 ms stabiler. */
#define COPRO_QUERY_DEFAULT_TIMEOUT_MS  4000

/* Erfragt den Identify-Tag (mode-agnostic — funktioniert in BL und App).
 * Schreibt NUL-terminiert nach tag_out (max tag_cap-1 + NUL).
 * Returns 0 bei Erfolg, -1 bei Timeout/Fehler. */
int copro_query_identify(int fd, char *tag_out, size_t tag_cap, int timeout_ms);

/* Erfragt SGTIN — 12 Bytes (mode-agnostic).
 * Returns 0 bei Erfolg, -1 bei Timeout/Fehler. */
int copro_query_sgtin(int fd, uint8_t sgtin_out[12], int timeout_ms);

/* Erfragt App-Version via TRXAdapter (dst=0x01 cmd=0x02).
 * Modul muss im App-Mode sein.  Reply-Format kann 3 Bytes (single
 * version) oder 9 Bytes (3 versions) sein — wir nehmen die ersten 3.
 * Schreibt nach version_out[3] = {major, minor, patch}.
 * Returns 0 bei Erfolg, -1 bei Timeout/Fehler. */
int copro_query_app_version(int fd, uint8_t version_out[3], int timeout_ms);

/* Hardware-Modul-Typ — gleiche Werte wie piVCCU's `radio_module_type_t`
 * im detect_radio_module-Tool (Apache-2.0, alex@areinert.de). */
typedef enum {
    COPRO_HW_NONE           = 0,
    COPRO_HW_HMIP_RFUSB     = 1,
    COPRO_HW_HM_MOD_RPI_PCB = 3,
    COPRO_HW_RPI_RF_MOD     = 4,
    COPRO_HW_UNKNOWN        = 255,
} copro_hw_type_t;

/* Erfragt Hardware-Modul-Typ via TRXAdapter (dst=0x01 cmd=0x09 getMCUType).
 * Reply-Layout: cmd=0x04 status=0x01 + 1 Byte = enum-Wert.
 * Modul muss im App-Mode sein.
 *
 * Ausnahme: Legacy-Co_CPU-Module (HM-MOD-RPI-PCB) supporten cmd=0x09 nicht;
 * für die liefert getIdentify einen `Co_CPU_App`-Tag und der MCU-type ist
 * implizit HM-MOD-RPI-PCB.  Caller sollte das vorher klassifizieren.
 *
 * Returns 0 bei Erfolg, -1 bei Timeout/Fehler. */
int copro_query_mcu_type(int fd, copro_hw_type_t *type_out, int timeout_ms);

/* Human-readable Modul-Name.  Nutzt SGTIN-Prefix-Check für TK-Variante:
 * `3014F5AC...` → "HMIP-RFUSB-TK" (Telekom), sonst "HMIP-RFUSB".
 * sgtin darf NULL sein — dann TK-Check übersprungen.
 * Returns const string, niemals NULL. */
const char *copro_hw_type_name(copro_hw_type_t type, const char *sgtin_hex);

/* Erfragt Bootloader-Version via Bootloader-Subsystem (dst=0x00 cmd=0x02).
 * Modul muss im BL-Mode sein.  Schreibt 3-Byte Version.
 * Returns 0 bei Erfolg, -1 bei Timeout/Fehler. */
int copro_query_bl_version(int fd, uint8_t version_out[3], int timeout_ms);

/* ───── Stage 2 Iter 2 — Mode-Switch-API ──────────────────────────────── */

/* Klassifikation des Identify-Tags.  Beleg: hmip-copro-update.jar,
 * CommonSubsystem.java Z. 37 (Tag-Whitelist) + TRXAdapterUpdater.java
 * Z. 73-84 (getSupported{Bootloader,Application}Identifiers). */
typedef enum {
    COPRO_MODE_UNKNOWN     = 0,
    COPRO_MODE_BOOTLOADER  = 1,    /* Tag endet auf _Bl / _BL */
    COPRO_MODE_APPLICATION = 2,    /* Tag endet auf _App */
} copro_mode_t;

/* Klassifiziert einen Identify-Tag (NUL-terminiert).  Returns
 * COPRO_MODE_UNKNOWN wenn weder _Bl noch _App erkennbar. */
copro_mode_t copro_classify_tag(const char *tag);

/* Wechselt das Modul in den Bootloader-Mode.  Sequenz:
 *   1. copro_query_identify → aktueller Mode
 *   2. wenn bereits BL: kein Frame, sofort return 0
 *   3. send COMMON cmd=0x02 startBootloader, wait ACK (4000ms)
 *   4. wait für neuen Identify-Push aus Modul (5000ms — Spec)
 *   5. validate: neuer Tag muss BL-klassifiziert sein
 *
 * Schreibt den finalen Tag (max cap-1 + NUL) nach new_tag_out wenn != NULL.
 * timeout_ms ist die Gesamtdauer für Steps 3+4; Default 9000 wenn 0.
 * Returns 0 bei Erfolg, -1 sonst.
 *
 * RE-Quelle: TRXAdapterUpdater.StartBootloader (Z. 314-334). */
int copro_start_bootloader(int fd, char *new_tag_out, size_t cap, int timeout_ms);

/* Wechselt das Modul in den Application-Mode (analog zu start_bootloader).
 * Sequenz: COMMON cmd=0x03 StartApplication, wait ACK + Push.
 * App braucht länger zum Booten (Spec: 9000ms für Identify-Push).
 *
 * Schreibt den finalen Tag (max cap-1 + NUL) nach new_tag_out wenn != NULL.
 * timeout_ms ist die Gesamtdauer; Default 13000 wenn 0.
 * Returns 0 bei Erfolg, -1 sonst.
 *
 * RE-Quelle: TRXAdapterUpdater.StartApplication (Z. 336-358). */
int copro_start_application(int fd, char *new_tag_out, size_t cap, int timeout_ms);

/* ───── Stage 2 Iter 3 — Flash-Pfad ───────────────────────────────────── */

/* Sendet einen einzelnen Update-Frame (opake Bytes aus .eq3-File) ans
 * Bootloader-Subsystem.  Modul muss im BL-Mode sein.
 *
 *   wire: dst=0x00 (Bootloader)  cnt=g_seq_os++  cmd=0x05  + frame_bytes
 *   reply: dst=0x00 cmd=0x04 status=0x01 (ACK) bei Erfolg
 *
 * timeout_ms: 4000 default (Java-Tool-Wert).  Returns 0 bei ACK,
 * -1 bei Timeout/NACK/Status≠0x01.
 *
 * RE-Quelle: Bootloader.sendUpdateFrame, TRXAdapterUpdater.updateTrxAdapter
 * Z. 146-156. */
int copro_send_update_frame(int fd, const uint8_t *frame_bytes, size_t len,
                            int timeout_ms);

/* Forward-Decl — eq3_image.h darf nicht zwingend included sein. */
struct eq3_image;

/* Optionale Konfiguration für copro_flash_image. */
typedef struct {
    int  force_overwrite;        /* 1 = flash auch wenn current==expected */
    int  per_frame_timeout_ms;   /* 0 = default 4000 */
    void (*on_progress)(void *ctx, size_t frame_idx, size_t n_frames);
    void *progress_ctx;
    int  assume_in_bl;           /* 1 = skip start_bootloader; Caller hat
                                  *     Modul schon via HW-Reset oder anders
                                  *     in BL gebracht.  Nötig für legacy
                                  *     Co_CPU-FW (HM-MOD-UART) die keinen
                                  *     COMMON-Subsystem-Switch supportet. */
    int  skip_app_after;         /* 1 = nach flash NICHT in App switchen.
                                  *     Modul bleibt im BL — Caller verantwortlich. */
} copro_flash_opts_t;

/* Flash-Resultat. */
typedef enum {
    COPRO_FLASH_OK,
    COPRO_FLASH_SKIPPED_VERSION_MATCH,
    COPRO_FLASH_FAIL_BL_ENTER,
    COPRO_FLASH_FAIL_FRAME,
    COPRO_FLASH_FAIL_APP_ENTER,
} copro_flash_result_t;

/* Top-Level Flash-Pfad.  Sequenz (RE'd aus TRXAdapterUpdater):
 *   1. read App-Version, vergleiche mit image->expected_version.
 *      Wenn equal && !force_overwrite → return SKIPPED_VERSION_MATCH.
 *   2. copro_start_bootloader (wenn nicht schon BL).
 *   3. log copro_query_bl_version.
 *   4. for frame in image->frames: copro_send_update_frame; on Fehler
 *      sofort abort, return FAIL_FRAME.
 *   5. copro_start_application — return FAIL_APP_ENTER bei stuck.
 *   6. log neue App-Version.
 *
 * MOdul-Mode-Side-Effects: nach Erfolg im App, nach Fehler ggf. im BL
 * (Recovery: Re-Run mit force_overwrite, oder hw_reset). */
copro_flash_result_t copro_flash_image(int fd,
                                       const struct eq3_image *image,
                                       const copro_flash_opts_t *opts,
                                       char *err_msg, size_t err_cap);

const char *copro_flash_result_name(copro_flash_result_t r);

#endif
