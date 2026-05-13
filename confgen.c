// SPDX-License-Identifier: GPL-2.0-or-later
/* confgen.c — Konfig-Generator (lean shim-mode)
 *
 * Schreibt /etc/config/rfd.conf und /var/run/bmcd-config.json.
 * Hm_mode + InterfacesList.xml + avoid_multimacd-Marker fallen weg —
 * multimacd kümmert sich um seinen eigenen Service-State.
 *
 * Emit-Strategie pro File:
 *   1. Backup pre (*.bmcd-pre) — falls aktiviert
 *   2. Bestehende Datei lesen
 *   3. Bestehender Inhalt zwischen # bmcd-managed-begin / # bmcd-managed-end
 *      raus, neuer Block rein. Rest bleibt.
 *   4. Atomic-write: temp-file + rename.
 */

#include "confgen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define MARK_BEGIN  "# bmcd-managed-begin (do not edit between markers)\n"
#define MARK_END    "# bmcd-managed-end\n"

static int file_read_all(const char *path, char **out_buf, size_t *out_len)
{
    *out_buf = NULL; *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    buf[sz] = 0;
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int file_write_atomic(const char *path, const char *content, size_t len)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.bmcd-tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len > 0 && fwrite(content, 1, len, f) != len) {
        fclose(f); unlink(tmp); return -1;
    }
    if (fflush(f) != 0) { fclose(f); unlink(tmp); return -1; }
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

static int backup_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;  /* nicht da — kein Backup nötig */
    char bak[256];
    snprintf(bak, sizeof(bak), "%s.bmcd-pre", path);
    /* Backup nur einmal erzeugen — wenn schon da, nicht überschreiben
     * (User-Originalstand bleibt erhalten). */
    if (stat(bak, &st) == 0) return 0;
    char *buf; size_t len;
    if (file_read_all(path, &buf, &len) < 0) return -1;
    int rc = file_write_atomic(bak, buf, len);
    free(buf);
    return rc;
}

/* ─── /etc/config/rfd.conf ────────────────────────────────────────────
 *
 * managed-Block: ein [Interface N]-Block pro Backend mit gefülltem
 * bidcos_comport.  Header (Listen Port, Log Level, etc.) bleibt erhalten
 * — nur unsere Section wird ersetzt.
 */

/* Header-only-Kopie: alles aus orig BIS zur ersten "[Interface "-Zeile,
 * oder bis MARK_BEGIN. Damit verhindern wir dass bestehende [Interface N]-
 * Blöcke neben unserem managed-Bereich co-existieren. */
static char *rfd_conf_header_only(const char *orig, size_t orig_len)
{
    if (!orig || orig_len == 0) return strdup("");
    const char *p = orig;
    const char *end = orig + orig_len;
    const char *cut = NULL;
    if (orig_len >= 11 && memcmp(orig, "[Interface ", 11) == 0) cut = orig;
    else {
        const char *q = strstr(p, "\n[Interface ");
        if (q && q < end) cut = q + 1;  /* nach dem \n */
    }
    const char *m = strstr(orig, MARK_BEGIN);
    if (m && (!cut || m < cut)) cut = m;
    if (!cut) cut = end;
    size_t pre_len = (size_t)(cut - orig);
    char *out = malloc(pre_len + 1);
    if (!out) return NULL;
    memcpy(out, orig, pre_len);
    out[pre_len] = 0;
    return out;
}

static int emit_rfd_conf(const confgen_input_t *in)
{
    const char *path = in->path_rfd_conf ? in->path_rfd_conf : "/etc/config/rfd.conf";

    /* managed-Block bauen */
    char block[2048];
    int n = 0;
    n += snprintf(block + n, sizeof(block) - n, "%s", MARK_BEGIN);
    int iface_id = 0;
    for (int j = 0; j < in->n_backends; ++j) {
        const confgen_backend_t *b = &in->backends[j];
        if (!b->bidcos_comport[0]) continue;
        n += snprintf(block + n, sizeof(block) - n,
            "[Interface %d]\n"
            "Type = CCU2\n"
            "ComPortFile = %s\n"
            "# backend = %s\n",
            iface_id, b->bidcos_comport, b->name);
        iface_id++;
    }
    n += snprintf(block + n, sizeof(block) - n, "%s", MARK_END);

    char *orig = NULL; size_t orig_len = 0;
    file_read_all(path, &orig, &orig_len);

    char *header = rfd_conf_header_only(orig, orig_len);
    free(orig);
    if (!header) return -1;
    size_t hl = strlen(header);
    bool need_nl = (hl > 0 && header[hl - 1] != '\n');

    size_t result_cap = hl + (need_nl ? 1 : 0) + (size_t)n + 1;
    char *result = malloc(result_cap);
    if (!result) { free(header); return -1; }
    memcpy(result, header, hl);
    if (need_nl) result[hl++] = '\n';
    memcpy(result + hl, block, (size_t)n);
    result[hl + n] = 0;
    free(header);
    size_t result_len = strlen(result);

    if (in->dry_run) {
        fprintf(stderr, "CONFGEN: dry-run %s (%zu bytes)\n", path, result_len);
        free(result); return 0;
    }
    if (in->backup_before_write) backup_file(path);
    int rc = file_write_atomic(path, result, result_len);
    free(result);
    if (rc < 0) {
        fprintf(stderr, "CONFGEN: write %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "CONFGEN: wrote %s (%d interfaces)\n", path, iface_id);
    return 0;
}

/* ─── /var/run/bmcd-config.json (bmcd's eigene Wahrheit) ──────────────── */

static int emit_bmcd_json(const confgen_input_t *in)
{
    const char *path = in->path_bmcd_json
                     ? in->path_bmcd_json : "/var/run/bmcd-config.json";
    char buf[4096];
    int n = 0;
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm);

    n += snprintf(buf + n, sizeof(buf) - n,
        "{\n"
        "  \"generated_at\": \"%s\",\n"
        "  \"generator\": \"bmcd-confgen\",\n"
        "  \"backends\": [\n", ts);
    for (int i = 0; i < in->n_backends; ++i) {
        const confgen_backend_t *b = &in->backends[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "    { \"idx\": %d, \"name\": \"%s\", \"hw_kind\": \"%s\","
            " \"transport\": \"%s\", \"app_tag\": \"%s\","
            " \"firmware\": \"%u.%u.%u\","
            " \"serial\": \"%s\", \"sgtin\": \"%s\","
            " \"bidcos_addr\": \"%s\", \"hmip_addr\": \"%s\","
            " \"dual_stack\": %s }%s\n",
            i, b->name, b->hw_kind, b->transport_path, b->app_tag,
            b->firmware[0], b->firmware[1], b->firmware[2],
            b->serial, b->sgtin, b->bidcos_address, b->hmip_address,
            b->dual_stack ? "true" : "false",
            (i + 1 < in->n_backends) ? "," : "");
    }
    n += snprintf(buf + n, sizeof(buf) - n,
        "  ],\n"
        "  \"endpoints\": [\n");
    int ep_emitted = 0;
    for (int i = 0; i < in->n_backends; ++i) {
        const confgen_backend_t *b = &in->backends[i];
        if (b->bidcos_comport[0]) {
            n += snprintf(buf + n, sizeof(buf) - n,
                "%s    { \"role\": \"bidcos\", \"target\": \"%s\","
                " \"backend_idx\": %d }",
                ep_emitted > 0 ? ",\n" : "",
                b->bidcos_comport, i);
            ep_emitted++;
        }
        if (b->hmip_comport[0]) {
            n += snprintf(buf + n, sizeof(buf) - n,
                "%s    { \"role\": \"hmip\", \"target\": \"%s\","
                " \"backend_idx\": %d }",
                ep_emitted > 0 ? ",\n" : "",
                b->hmip_comport, i);
            ep_emitted++;
        }
    }
    n += snprintf(buf + n, sizeof(buf) - n, "\n  ]\n}\n");

    if (in->dry_run) {
        fprintf(stderr, "CONFGEN: dry-run %s (%d bytes)\n", path, n);
        return 0;
    }
    if (file_write_atomic(path, buf, (size_t)n) < 0) {
        fprintf(stderr, "CONFGEN: write %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "CONFGEN: wrote %s\n", path);
    return 0;
}

int confgen_emit(const confgen_input_t *in)
{
    if (!in) { errno = EINVAL; return -1; }
    int rc = 0;
    if (emit_rfd_conf(in)  < 0) rc = -1;
    if (emit_bmcd_json(in) < 0) rc = -1;
    return rc;
}
