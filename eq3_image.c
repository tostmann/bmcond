// SPDX-License-Identifier: GPL-2.0-or-later
/* eq3_image.c — Reader für eq-3 .eq3 Files
 *
 * Spec: docs/copro_update_protocol_re_2026-05-08.md
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "eq3_image.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ───── Hex-Helpers ───────────────────────────────────────────────────── */

static int hex_nybble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes ASCII-hex-line into bytes. Skipped: whitespace.
 * Returns: malloc'd byte-array of length *out_len, or NULL+errno. */
static uint8_t *hex_decode(const char *line, size_t *out_len)
{
    size_t cap = 4096;
    uint8_t *buf = malloc(cap);
    if (!buf) { errno = ENOMEM; return NULL; }
    size_t n = 0;
    int high = -1;
    for (const char *p = line; *p && *p != '\n' && *p != '\r'; ++p) {
        if (isspace((unsigned char)*p)) continue;
        int v = hex_nybble(*p);
        if (v < 0) {
            free(buf); errno = EINVAL; return NULL;
        }
        if (high < 0) {
            high = v;
        } else {
            if (n >= cap) {
                cap *= 2;
                uint8_t *nb = realloc(buf, cap);
                if (!nb) { free(buf); errno = ENOMEM; return NULL; }
                buf = nb;
            }
            buf[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0) { free(buf); errno = EINVAL; return NULL; }  /* odd nybble count */
    *out_len = n;
    return buf;
}

/* ───── Filename-Version-Regex ────────────────────────────────────────── */

/* Sucht (\d+\.\d+\.\d+) am Ende von basename(path) (.eq3 abgeschnitten).
 * Z.B. "dualcopro_update_blhmip-4.4.22.eq3" → 4.4.22.
 * Returns 0 ok, -1 sonst. */
static int parse_version_from_filename(const char *path, uint8_t out[3])
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", base);

    /* Strip .eq3 if present. */
    size_t blen = strlen(buf);
    if (blen >= 4 && strcmp(buf + blen - 4, ".eq3") == 0) {
        buf[blen - 4] = 0;
        blen -= 4;
    }

    /* Backwards-scan: look for last `\d+\.\d+\.\d+`. Brute-force. */
    for (size_t i = 0; i + 4 < blen; ++i) {
        unsigned a, b, c;
        char tail;
        /* `%n` würde stehen, aber portabler: scanf-prefix-Match. */
        if (sscanf(buf + i, "%u.%u.%u%c", &a, &b, &c, &tail) >= 3) {
            /* Validiere: nach den Zahlen darf nichts (oder nur space) folgen.
             * sscanf-mit-%c erfordert minimum 4 Tokens; wenn nur 3 →
             * Ende des Strings. */
            if (a < 256 && b < 256 && c < 256) {
                /* Akzeptiere nur wenn die Match-Position auch wirklich
                 * bis zum String-Ende geht.  Sonst kann `1.2.3foo` matchen.
                 * Schnell-test: re-print + compare. */
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%u.%u.%u", a, b, c);
                size_t tlen = strlen(tmp);
                if (i + tlen == blen) {
                    out[0] = (uint8_t)a;
                    out[1] = (uint8_t)b;
                    out[2] = (uint8_t)c;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ───── Public API ───────────────────────────────────────────────────── */

int eq3_image_load(const char *path, eq3_image_t *out)
{
    if (!path || !out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));

    /* 1. File slurp */
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    struct stat st;
    if (stat(path, &st) < 0) { fclose(f); return -1; }
    if (st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) {
        fclose(f); errno = EFBIG; return -1;
    }
    char *line = malloc((size_t)st.st_size + 2);
    if (!line) { fclose(f); errno = ENOMEM; return -1; }
    size_t got = fread(line, 1, (size_t)st.st_size, f);
    fclose(f);
    line[got] = 0;

    /* 2. Decode hex → bytes (1 line, rest ignored) */
    size_t bytes_len = 0;
    uint8_t *bytes = hex_decode(line, &bytes_len);
    free(line);
    if (!bytes) return -1;
    if (bytes_len < 5) {
        free(bytes); errno = EINVAL; return -1;
    }

    /* 3. Per-chunk-frame split (Iter 3.1-fix-2, RE'd via byte-level
     * strace-Vergleich von `eq3configcmd update-coprocessor`).
     *
     * Echtes Format (jeder Chunk ist self-contained mit eigenem LEN+CRC):
     *   [LEN_BE_2 = chunk_size][chunk_size-2 bytes payload][CRC_2]
     *   [LEN_BE_2 = chunk_size][chunk_size-2 bytes payload][CRC_2]
     *   ...
     *   [LEN_BE_2 = ?         ][shorter payload         ][CRC_2]   (last)
     *
     * D.h. stride = chunk_size + 2 (= 2 LEN + (chunk_size-2) payload + 2 CRC).
     * payload-size pro chunk = chunk_size - 2.
     *
     * Java-jar's splitToFrames vergaß die per-chunk-CRC-suffix → ab Frame 1
     * synchron-fehl → IOException.  eq3configcmd C++ machts korrekt.
     *
     * Wir replay'en payloads (= 1:1 was ans Bootloader cmd=0x05 geht) —
     * CRC ist im Wire-Frame neu (per hmu_frame_encode), die per-chunk-CRC
     * im File ist ggf. eine andere Verifikation für Image-Integrity. */
    uint16_t chunk_size = ((uint16_t)bytes[0] << 8) | bytes[1];
    if (chunk_size < 5 || chunk_size > bytes_len) {
        fprintf(stderr,
            "eq3_image_load: invalid chunk-size %u (file %zu bytes)\n",
            (unsigned)chunk_size, bytes_len);
        free(bytes); errno = EINVAL; return -1;
    }
    size_t cap_frames = (bytes_len / (size_t)chunk_size) + 2;
    eq3_frame_t *frames = calloc(cap_frames, sizeof(*frames));
    if (!frames) { free(bytes); errno = ENOMEM; return -1; }
    size_t n_frames = 0;

    size_t pos = 0;
    while (pos + 2 < bytes_len) {
        /* Read this chunk's LEN.  Allow last-chunk-shorter-than-chunk_size
         * via clamping. */
        uint16_t this_len = ((uint16_t)bytes[pos] << 8) | bytes[pos + 1];
        if (this_len < 5) {
            fprintf(stderr, "eq3_image_load: tiny chunk-len %u at pos %zu\n",
                    (unsigned)this_len, pos);
            for (size_t i = 0; i < n_frames; ++i) free(frames[i].bytes);
            free(frames); free(bytes); errno = EINVAL; return -1;
        }
        size_t available = bytes_len - pos;
        size_t this_total = (size_t)this_len + 2;  /* 2 byte CRC suffix */
        if (this_total > available) this_total = available;
        if (this_total < 5) break;
        size_t payload_len = (this_total >= 4) ? this_total - 4 : 0;
        /* If this is a shortened last chunk (no full CRC tail), still
         * accept it: payload = available - 2 (skip leading LEN), no CRC. */
        if (this_total == available && this_total < (size_t)this_len + 2) {
            /* truncated tail — no CRC suffix */
            payload_len = available - 2;
        }
        if (payload_len == 0) break;

        eq3_frame_t *fr = &frames[n_frames++];
        fr->bytes = malloc(payload_len);
        if (!fr->bytes) {
            for (size_t i = 0; i + 1 < n_frames; ++i) free(frames[i].bytes);
            free(frames); free(bytes);
            errno = ENOMEM; return -1;
        }
        memcpy(fr->bytes, bytes + pos + 2, payload_len);
        fr->len = payload_len;
        pos += this_total;
    }
    free(bytes);
    out->chunk_size = chunk_size;

    /* 4. Filename-Version-Regex */
    snprintf(out->path, sizeof(out->path), "%s", path);
    if (parse_version_from_filename(path, out->expected_version) == 0) {
        out->version_known = 1;
    }

    out->frames   = frames;
    out->n_frames = n_frames;
    return 0;
}

void eq3_image_free(eq3_image_t *img)
{
    if (!img) return;
    if (img->frames) {
        for (size_t i = 0; i < img->n_frames; ++i) {
            free(img->frames[i].bytes);
        }
        free(img->frames);
    }
    memset(img, 0, sizeof(*img));
}

const char *eq3_ftyp_name(uint8_t ftyp)
{
    switch (ftyp & ~(uint8_t)EQ3_FTYP_BC_FLAG) {
    case EQ3_FTYP_UPDATE:              return "UPDATE";
    case EQ3_FTYP_UPDATE_PART:         return "UPDATE_PART";
    case EQ3_FTYP_RESPONSE:            return "RESPONSE";
    case EQ3_FTYP_INFO:                return "INFO";
    case EQ3_FTYP_UPDATE_INIT:         return "UPDATE_INIT";
    case EQ3_FTYP_UPDATE_END:          return "UPDATE_END";
    case EQ3_FTYP_SET_UART_SPEED_HIGH: return "SET_UART_SPEED_HIGH";
    default:                           return "UNKNOWN";
    }
}
