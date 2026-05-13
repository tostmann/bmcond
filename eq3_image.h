// SPDX-License-Identifier: GPL-2.0-or-later
/* eq3_image.h — Reader für eq-3 .eq3 Coprocessor-Update-Files
 *
 * Format (RE'd 2026-05-08, Quelle: TRXAdapterUpdater.updateTrxAdapter
 * + splitToFrames):
 *
 *   - 1 ASCII-Zeile Hex-Zeichen → decode zu Binary-Stream
 *   - Binary = Sequenz von Frames mit 2-Byte BE Length-Prefix.
 *     Length INKLUSIVE der 2 Length-Bytes.
 *
 * Per-Frame-Inhalt ist ein vor-signierter `FirmwareUpdateFrame`:
 *     [fcnt:1] [ftyp:1] [addr:3] [payload...]
 * Wir parsen das nicht — Payload ist opake Bytes für das Modul,
 * nonce/imageMac sind drin und vom eq-3-Build-System signiert.
 *
 * Filename-Konvention: `<name>-<MAJOR>.<MINOR>.<PATCH>.eq3`. Der
 * Loader zieht die Soll-Version mit Regex aus dem Filename — NICHT
 * aus dem Inhalt.  Reicht für Skip-wenn-Equal-Logik.
 *
 * Spec: docs/copro_update_protocol_re_2026-05-08.md
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_EQ3_IMAGE_H
#define CUL32HM_EQ3_IMAGE_H

#include <stdint.h>
#include <stddef.h>

/* Frame-Type-Enum aus Stage-1-RE (UpdateFrameType im jar) — bleibt nur
 * als Diagnose-Tool für eq3_ftyp_name(); wir parsen den Inhalt der
 * Chunks NICHT (Iter 3.1 Korrektur — vor-signierte chunks bleiben
 * opake bytes). */
typedef enum {
    EQ3_FTYP_UPDATE              = 0x2F,
    EQ3_FTYP_UPDATE_PART         = 0x2E,
    EQ3_FTYP_RESPONSE            = 0x2D,
    EQ3_FTYP_INFO                = 0x2C,
    EQ3_FTYP_UPDATE_INIT         = 0x2A,
    EQ3_FTYP_UPDATE_END          = 0x29,
    EQ3_FTYP_SET_UART_SPEED_HIGH = 0x28,
    EQ3_FTYP_BC_FLAG             = 0x80,
} eq3_ftyp_t;

typedef struct {
    uint8_t   *bytes;       /* malloc'd, owned by image */
    size_t     len;
    /* Folgende Felder sind RESERVIERT — nicht aktiv populiert seit Iter 3.1
     * (vor-signierte chunks dürfen wir nicht semantisch parsen). */
    eq3_ftyp_t ftyp_base;
    int        is_broadcast;
    uint8_t    fcnt;
    uint8_t    addr[3];
} eq3_frame_t;

typedef struct eq3_image {
    char         path[256];
    /* Soll-Version aus Filename — 0/0/0 wenn nicht parseable. */
    uint8_t      expected_version[3];
    int          version_known;
    /* Chunk-Größe aus den ersten 2 Bytes des Files (LEN_BE).
     * Z.B. 1042 für HM-MOD-UART, 562 für DualCoPro-RFUSB/RPI-RF-MOD. */
    uint16_t     chunk_size;
    /* Frame-Liste (malloc'd Array von malloc'd bytes). */
    eq3_frame_t *frames;
    size_t       n_frames;
    /* Statistik pro Type — RESERVIERT, nicht aktiv populiert. */
    size_t       count_by_ftyp[256];
} eq3_image_t;

/* Lädt + parsed ein .eq3-File. Allokiert Speicher in *out.
 * Returns: 0 ok, -1 + errno bei Fehler (file not found, malformed, ...).
 * Bei -1 ist *out unverändert. */
int  eq3_image_load(const char *path, eq3_image_t *out);
void eq3_image_free(eq3_image_t *img);

/* Helper: bekannte Frame-Type-Namen für Logging. */
const char *eq3_ftyp_name(uint8_t ftyp);

#endif
