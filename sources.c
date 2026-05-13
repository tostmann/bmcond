// SPDX-License-Identifier: GPL-2.0-or-later
/* bmcond.sources.json persistence layer — see docs/sources_schema.md
 * for the contract.  This module is concerned only with reading,
 * writing, validating, and mutating the schema state in memory.
 * Discovery, runtime application (reload), and effective-state are
 * separate modules. */

#include "sources.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ─── small helpers ──────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int valid_id(const char *id)
{
    if (!id) return 0;
    size_t n = strlen(id);
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int valid_disco(const char *d)
{
    if (!d || !*d) return 1;   /* empty is allowed */
    return !strcmp(d, "libusb") || !strcmp(d, "mdns") || !strcmp(d, "manual");
}

/* ─── init / find ────────────────────────────────────────────────────── */

void bmcond_sources_init(struct bmcond_sources_state *s)
{
    memset(s, 0, sizeof(*s));
    s->schema_version = BMCOND_SOURCES_SCHEMA_VERSION;
}

const struct bmcond_source *bmcond_sources_find(
    const struct bmcond_sources_state *s, const char *id)
{
    if (!s || !id || !*id) return NULL;
    for (int i = 0; i < s->n_sources; ++i)
        if (!strcmp(s->sources[i].id, id))
            return &s->sources[i];
    return NULL;
}

static int find_index(const struct bmcond_sources_state *s, const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < s->n_sources; ++i)
        if (!strcmp(s->sources[i].id, id)) return i;
    return -1;
}

/* ─── validation ─────────────────────────────────────────────────────── */

static int validate_source(const struct bmcond_source *src,
                           char *err, size_t errcap)
{
    if (!valid_id(src->id)) {
        set_err(err, errcap, "invalid id '%.32s' — must match [a-z0-9_-]{1,32}", src->id);
        return -1;
    }
    if (!*src->transport) {
        set_err(err, errcap, "source '%s' missing transport", src->id);
        return -1;
    }
    if (!src->cap_bidcos && !src->cap_hmip) {
        set_err(err, errcap, "source '%s' has empty capabilities", src->id);
        return -1;
    }
    if (!valid_disco(src->discovered_via)) {
        set_err(err, errcap, "source '%s' invalid discovered_via '%s'",
                src->id, src->discovered_via);
        return -1;
    }
    if (strlen(src->notes) >= BMCOND_SOURCE_NOTES_MAX) {
        set_err(err, errcap, "source '%s' notes too long", src->id);
        return -1;
    }
    if (strlen(src->label) >= BMCOND_SOURCE_LABEL_MAX) {
        set_err(err, errcap, "source '%s' label too long", src->id);
        return -1;
    }
    return 0;
}

int bmcond_sources_validate(const struct bmcond_sources_state *s,
                            char *err, size_t errcap)
{
    if (s->schema_version != BMCOND_SOURCES_SCHEMA_VERSION) {
        set_err(err, errcap, "unsupported schema_version=%d (want %d)",
                s->schema_version, BMCOND_SOURCES_SCHEMA_VERSION);
        return -1;
    }
    if (s->n_sources < 0 || s->n_sources > BMCOND_SOURCES_MAX) {
        set_err(err, errcap, "n_sources=%d out of range", s->n_sources);
        return -1;
    }
    /* per-source validity + uniqueness */
    for (int i = 0; i < s->n_sources; ++i) {
        if (validate_source(&s->sources[i], err, errcap) < 0) return -1;
        for (int j = i + 1; j < s->n_sources; ++j) {
            if (!strcmp(s->sources[i].id, s->sources[j].id)) {
                set_err(err, errcap, "duplicate source id '%s'", s->sources[i].id);
                return -1;
            }
        }
    }
    /* slot validity */
    if (*s->slots.bidcos) {
        const struct bmcond_source *src = bmcond_sources_find(s, s->slots.bidcos);
        if (!src) {
            set_err(err, errcap, "slots.bidcos references unknown id '%s'",
                    s->slots.bidcos);
            return -1;
        }
        if (!src->cap_bidcos) {
            set_err(err, errcap, "slots.bidcos source '%s' lacks bidcos capability",
                    s->slots.bidcos);
            return -1;
        }
    }
    if (*s->slots.hmip) {
        const struct bmcond_source *src = bmcond_sources_find(s, s->slots.hmip);
        if (!src) {
            set_err(err, errcap, "slots.hmip references unknown id '%s'",
                    s->slots.hmip);
            return -1;
        }
        if (!src->cap_hmip) {
            set_err(err, errcap, "slots.hmip source '%s' lacks hmip capability",
                    s->slots.hmip);
            return -1;
        }
    }
    return 0;
}

/* ─── cJSON ↔ struct ─────────────────────────────────────────────────── */

static int parse_caps(cJSON *arr, bool *cap_bidcos, bool *cap_hmip,
                      const char *id, char *err, size_t errcap)
{
    *cap_bidcos = false;
    *cap_hmip = false;
    if (!arr || !cJSON_IsArray(arr)) {
        set_err(err, errcap, "source '%s' capabilities must be array", id);
        return -1;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(e)) {
            set_err(err, errcap, "source '%s' capabilities[%d] not a string", id, i);
            return -1;
        }
        const char *v = e->valuestring;
        if      (!strcmp(v, "bidcos")) *cap_bidcos = true;
        else if (!strcmp(v, "hmip"))   *cap_hmip   = true;
        else {
            set_err(err, errcap,
                    "source '%s' unknown capability '%s' (allowed: bidcos, hmip)",
                    id, v);
            return -1;
        }
    }
    return 0;
}

static int parse_source(cJSON *obj, struct bmcond_source *out,
                        char *err, size_t errcap)
{
    memset(out, 0, sizeof(*out));
    if (!obj || !cJSON_IsObject(obj)) {
        set_err(err, errcap, "source must be a JSON object");
        return -1;
    }
    cJSON *fid = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (!fid || !cJSON_IsString(fid)) {
        set_err(err, errcap, "source missing string field 'id'");
        return -1;
    }
    copy_str(out->id, sizeof(out->id), fid->valuestring);

    cJSON *ftr = cJSON_GetObjectItemCaseSensitive(obj, "transport");
    if (!ftr || !cJSON_IsString(ftr)) {
        set_err(err, errcap, "source '%s' missing string field 'transport'", out->id);
        return -1;
    }
    copy_str(out->transport, sizeof(out->transport), ftr->valuestring);

    cJSON *flab = cJSON_GetObjectItemCaseSensitive(obj, "label");
    if (flab && cJSON_IsString(flab))
        copy_str(out->label, sizeof(out->label), flab->valuestring);

    cJSON *fcaps = cJSON_GetObjectItemCaseSensitive(obj, "capabilities");
    if (parse_caps(fcaps, &out->cap_bidcos, &out->cap_hmip,
                   out->id, err, errcap) < 0) return -1;

    cJSON *fp = cJSON_GetObjectItemCaseSensitive(obj, "persistent");
    out->persistent = fp && cJSON_IsBool(fp) ? cJSON_IsTrue(fp) : false;

    cJSON *fdv = cJSON_GetObjectItemCaseSensitive(obj, "discovered_via");
    if (fdv && cJSON_IsString(fdv))
        copy_str(out->discovered_via, sizeof(out->discovered_via), fdv->valuestring);

    cJSON *fn = cJSON_GetObjectItemCaseSensitive(obj, "notes");
    if (fn && cJSON_IsString(fn))
        copy_str(out->notes, sizeof(out->notes), fn->valuestring);

    return validate_source(out, err, errcap);
}

static cJSON *source_to_cjson(const struct bmcond_source *src)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "id", src->id);
    cJSON_AddStringToObject(o, "transport", src->transport);
    if (*src->label)
        cJSON_AddStringToObject(o, "label", src->label);
    cJSON *caps = cJSON_AddArrayToObject(o, "capabilities");
    if (src->cap_bidcos) cJSON_AddItemToArray(caps, cJSON_CreateString("bidcos"));
    if (src->cap_hmip)   cJSON_AddItemToArray(caps, cJSON_CreateString("hmip"));
    cJSON_AddBoolToObject(o, "persistent", src->persistent);
    if (*src->discovered_via)
        cJSON_AddStringToObject(o, "discovered_via", src->discovered_via);
    if (*src->notes)
        cJSON_AddStringToObject(o, "notes", src->notes);
    return o;
}

/* ─── single-Source helpers ──────────────────────────────────────────── */

char *bmcond_source_to_json(const struct bmcond_source *src)
{
    cJSON *o = source_to_cjson(src);
    if (!o) return NULL;
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

int bmcond_source_from_json(const char *json, struct bmcond_source *out,
                            char *err, size_t errcap)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_err(err, errcap, "JSON parse failed near offset %ld",
                (long)(cJSON_GetErrorPtr() - json));
        return -1;
    }
    int rc = parse_source(root, out, err, errcap);
    cJSON_Delete(root);
    return rc;
}

/* ─── full-document JSON ↔ state ─────────────────────────────────────── */

char *bmcond_sources_to_json(const struct bmcond_sources_state *s)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "schema_version", s->schema_version);

    cJSON *arr = cJSON_AddArrayToObject(root, "sources");
    for (int i = 0; i < s->n_sources; ++i) {
        cJSON *src = source_to_cjson(&s->sources[i]);
        if (!src) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(arr, src);
    }

    cJSON *slots = cJSON_AddObjectToObject(root, "slots");
    if (*s->slots.bidcos) cJSON_AddStringToObject(slots, "bidcos", s->slots.bidcos);
    else                  cJSON_AddNullToObject  (slots, "bidcos");
    if (*s->slots.hmip)   cJSON_AddStringToObject(slots, "hmip",   s->slots.hmip);
    else                  cJSON_AddNullToObject  (slots, "hmip");

    char *out = cJSON_Print(root);   /* pretty for humans inspecting the file */
    cJSON_Delete(root);
    return out;
}

int bmcond_sources_from_json(const char *json,
                             struct bmcond_sources_state *out,
                             char *err, size_t errcap)
{
    bmcond_sources_init(out);
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_err(err, errcap, "JSON parse failed near offset %ld",
                (long)(cJSON_GetErrorPtr() - json));
        return -1;
    }

    cJSON *fv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!fv || !cJSON_IsNumber(fv)) {
        set_err(err, errcap, "missing 'schema_version' integer");
        cJSON_Delete(root); return -1;
    }
    out->schema_version = fv->valueint;

    cJSON *farr = cJSON_GetObjectItemCaseSensitive(root, "sources");
    if (farr && !cJSON_IsArray(farr)) {
        set_err(err, errcap, "'sources' must be an array");
        cJSON_Delete(root); return -1;
    }
    if (farr) {
        int n = cJSON_GetArraySize(farr);
        if (n > BMCOND_SOURCES_MAX) {
            set_err(err, errcap, "too many sources (%d > max %d)",
                    n, BMCOND_SOURCES_MAX);
            cJSON_Delete(root); return -1;
        }
        for (int i = 0; i < n; ++i) {
            if (parse_source(cJSON_GetArrayItem(farr, i),
                             &out->sources[out->n_sources], err, errcap) < 0) {
                cJSON_Delete(root); return -1;
            }
            out->n_sources++;
        }
    }

    cJSON *fslots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    if (fslots) {
        if (!cJSON_IsObject(fslots)) {
            set_err(err, errcap, "'slots' must be object");
            cJSON_Delete(root); return -1;
        }
        cJSON *fb = cJSON_GetObjectItemCaseSensitive(fslots, "bidcos");
        cJSON *fh = cJSON_GetObjectItemCaseSensitive(fslots, "hmip");
        if (fb && cJSON_IsString(fb))
            copy_str(out->slots.bidcos, sizeof(out->slots.bidcos), fb->valuestring);
        if (fh && cJSON_IsString(fh))
            copy_str(out->slots.hmip,   sizeof(out->slots.hmip),   fh->valuestring);
        /* null slots leave the strings empty (memset in init) */
    }

    cJSON_Delete(root);

    return bmcond_sources_validate(out, err, errcap);
}

/* ─── disk I/O ───────────────────────────────────────────────────────── */

static char *slurp_file(const char *path, size_t *out_n, int *out_errno)
{
    FILE *f = fopen(path, "r");
    if (!f) { *out_errno = errno; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 1024 * 1024) { fclose(f); *out_errno = EFBIG; return NULL; }
    rewind(f);
    char *p = malloc((size_t)sz + 1);
    if (!p) { fclose(f); *out_errno = ENOMEM; return NULL; }
    size_t r = fread(p, 1, (size_t)sz, f);
    fclose(f);
    p[r] = 0;
    if (out_n) *out_n = r;
    *out_errno = 0;
    return p;
}

int bmcond_sources_load(const char *path, struct bmcond_sources_state *out,
                        char *err, size_t errcap)
{
    bmcond_sources_init(out);
    if (!path || !*path) {
        set_err(err, errcap, "no path");
        return -1;
    }
    int e = 0;
    char *blob = slurp_file(path, NULL, &e);
    if (!blob) {
        if (e == ENOENT) {
            /* Fresh deployment — empty state is fine, not an error. */
            return 0;
        }
        set_err(err, errcap, "open %s: %s", path, strerror(e));
        return -1;
    }
    int rc = bmcond_sources_from_json(blob, out, err, errcap);
    free(blob);
    return rc;
}

static int mkdir_p(const char *path, mode_t mode)
{
    /* path is the file path; strip the basename and mkdir parents. */
    char buf[1024];
    copy_str(buf, sizeof(buf), path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;     /* nothing to do */
    *slash = 0;
    /* recursive mkdir */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

int bmcond_sources_save(const char *path, const struct bmcond_sources_state *s,
                        char *err, size_t errcap)
{
    if (bmcond_sources_validate(s, err, errcap) < 0) return -1;
    if (!path || !*path) {
        set_err(err, errcap, "no path");
        return -1;
    }
    if (mkdir_p(path, 0755) < 0) {
        set_err(err, errcap, "mkdir parent of %s: %s", path, strerror(errno));
        return -1;
    }

    char *json = bmcond_sources_to_json(s);
    if (!json) { set_err(err, errcap, "serialize: oom"); return -1; }

    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        set_err(err, errcap, "open %s: %s", tmp, strerror(errno));
        free(json); return -1;
    }
    size_t n = strlen(json);
    ssize_t w = write(fd, json, n);
    free(json);
    if (w != (ssize_t)n) {
        set_err(err, errcap, "write %s: %s", tmp, strerror(errno));
        close(fd); unlink(tmp); return -1;
    }
    if (fsync(fd) < 0) {
        /* fsync failure is rare but worth surfacing; do not abort */
        fprintf(stderr, "sources: fsync(%s): %s\n", tmp, strerror(errno));
    }
    close(fd);
    if (rename(tmp, path) < 0) {
        set_err(err, errcap, "rename %s → %s: %s", tmp, path, strerror(errno));
        unlink(tmp); return -1;
    }
    return 0;
}

/* ─── mutators ───────────────────────────────────────────────────────── */

int bmcond_sources_set_slots(struct bmcond_sources_state *s,
                             const char *bidcos_id_or_null,
                             const char *hmip_id_or_null,
                             char *err, size_t errcap)
{
    /* Hold the previous values so we can restore on validation failure. */
    struct bmcond_slots prev = s->slots;
    copy_str(s->slots.bidcos, sizeof(s->slots.bidcos),
             bidcos_id_or_null ? bidcos_id_or_null : "");
    copy_str(s->slots.hmip,   sizeof(s->slots.hmip),
             hmip_id_or_null   ? hmip_id_or_null   : "");
    if (bmcond_sources_validate(s, err, errcap) < 0) {
        s->slots = prev;
        return -1;
    }
    return 0;
}

int bmcond_sources_upsert(struct bmcond_sources_state *s,
                          const struct bmcond_source *src,
                          char *err, size_t errcap)
{
    if (validate_source(src, err, errcap) < 0) return -1;
    int idx = find_index(s, src->id);
    if (idx >= 0) {
        s->sources[idx] = *src;
        /* slots may now be invalid if capability got narrowed; re-validate. */
        if (bmcond_sources_validate(s, err, errcap) < 0) return -1;
        return 0;
    }
    if (s->n_sources >= BMCOND_SOURCES_MAX) {
        set_err(err, errcap, "too many sources (max %d)", BMCOND_SOURCES_MAX);
        return -1;
    }
    s->sources[s->n_sources++] = *src;
    return 0;
}

int bmcond_sources_remove(struct bmcond_sources_state *s, const char *id,
                          char *cleared, size_t cleared_cap,
                          char *err, size_t errcap)
{
    int idx = find_index(s, id);
    if (idx < 0) {
        set_err(err, errcap, "no source with id '%s'", id ? id : "");
        if (cleared && cleared_cap) cleared[0] = 0;
        return -1;
    }
    /* compact array */
    for (int i = idx; i < s->n_sources - 1; ++i)
        s->sources[i] = s->sources[i + 1];
    s->n_sources--;
    /* auto-clear slots */
    int n_cleared = 0;
    char buf[64] = "";
    if (!strcmp(s->slots.bidcos, id)) {
        s->slots.bidcos[0] = 0;
        strcat(buf, "bidcos");
        n_cleared++;
    }
    if (!strcmp(s->slots.hmip, id)) {
        if (n_cleared) strcat(buf, ",");
        strcat(buf, "hmip");
        s->slots.hmip[0] = 0;
        n_cleared++;
    }
    if (cleared && cleared_cap) copy_str(cleared, cleared_cap, buf);
    return 0;
}
