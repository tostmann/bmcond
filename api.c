// SPDX-License-Identifier: GPL-2.0-or-later
/* HTTP/JSON admin endpoint for bmcond.
 *
 * Single-threaded, blocking accept+handle. Listens on a TCP port (default
 * 9126). Each accepted connection is handled fully synchronously (read
 * request, dispatch, write response, close) — fine for admin traffic from
 * the AddOn SPA, never more than one client at a time in practice.
 *
 * Endpoints:
 *   GET  /                  — embedded SPA (HTML+JS, calls the API below)
 *   GET  /api/health
 *   GET  /api/status        — live stats + backends/endpoints from /var/run/bmcd-config.json
 *   GET  /api/config        — current persistent config
 *   POST /api/config        — { "transport": "...", "bidcos": "...", "hmip": "...", "extra": "..." }
 *   GET  /api/sources, PUT, POST/DELETE /api/sources/<id>
 *   GET  /api/slots, PUT
 *   GET  /api/discover      — libusb-Enumerate (Stage 47b.1)
 *   GET  /api/effective     — claim+verified caps + stats
 *   POST /api/reload        — supervisor-restart (SIGTERM self)
 *   GET  /api/log/tail?n=N  — last N frame events as text/plain
 *   OPTIONS *               — CORS preflight
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "api.h"
#include "radio_id.h"
#include "sources.h"
#include "transport_usb.h"
#include "version.h"

#include <cjson/cJSON.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define API_DEFAULT_PORT  9126
#define RING_SIZE         256
#define DATA_CAP          64

struct ring_entry {
    uint64_t  ts_us;       /* monotonic μs since api_init */
    char      channel[8];  /* "bidcos", "hmip", "uart" */
    uint8_t   dst;
    uint8_t   cnt;
    uint8_t   plen;
    uint8_t   cmd;
    uint8_t   data[DATA_CAP];
    size_t    dlen;
};

static struct {
    int                 listen_fd;
    struct api_context  ctx;
    struct ring_entry   ring[RING_SIZE];
    unsigned            ring_pos;     /* next write slot */
    unsigned            ring_total;   /* total appended (for sequencing) */
    pthread_mutex_t     ring_lock;
    uint64_t            start_us;
    struct bmcond_sources_state sources;   /* in-memory sources.json */
    pthread_mutex_t     sources_lock;
    pthread_t           accept_thread;
    int                 accept_thread_started;
    volatile int        accept_thread_stop;
} g = { .listen_fd = -1,
        .ring_lock    = PTHREAD_MUTEX_INITIALIZER,
        .sources_lock = PTHREAD_MUTEX_INITIALIZER };

/* ────────────────────────────────────────────────────────────────────── */

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

int api_listen_fd(void) { return g.listen_fd; }

/* Forward — accept-Loop-Implementation siehe weiter unten. */
void api_handle_accept(void);

static void *api_accept_thread_fn(void *arg)
{
    (void)arg;
    while (!g.accept_thread_stop && g.listen_fd >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g.listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(g.listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;
        if (FD_ISSET(g.listen_fd, &rfds)) api_handle_accept();
    }
    return NULL;
}

int api_init(const struct api_context *ctx)
{
    if (g.listen_fd >= 0) return g.listen_fd;
    g.ctx = *ctx;
    int port = ctx->port > 0 ? ctx->port : API_DEFAULT_PORT;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("api: socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "api: bind(:%d) failed: %s\n", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, 4) < 0) { perror("api: listen"); close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    g.listen_fd = fd;
    g.start_us = now_us();

    /* Load sources.json (if path given). Missing file is fine — empty state. */
    bmcond_sources_init(&g.sources);
    if (g.ctx.sources_path && *g.ctx.sources_path) {
        char err[256] = "";
        if (bmcond_sources_load(g.ctx.sources_path, &g.sources, err, sizeof(err)) < 0) {
            fprintf(stderr, "API: sources_load %s: %s — using empty state\n",
                    g.ctx.sources_path, err);
            bmcond_sources_init(&g.sources);
        } else {
            fprintf(stderr, "API: loaded %s — %d source(s), bidcos=%s hmip=%s\n",
                    g.ctx.sources_path, g.sources.n_sources,
                    *g.sources.slots.bidcos ? g.sources.slots.bidcos : "(null)",
                    *g.sources.slots.hmip   ? g.sources.slots.hmip   : "(null)");
        }
    }

    fprintf(stderr, "API: listening on 0.0.0.0:%d\n", port);

    /* Background-Accept-Thread: macht die JSON-API unabhängig vom Main-Loop.
     * Ohne diesen Thread würde die API tot bleiben solange der main thread
     * in transport_reconnect-retry-Loops steckt (z.B. ohne USB-Hardware). */
    g.accept_thread_stop = 0;
    if (pthread_create(&g.accept_thread, NULL, api_accept_thread_fn, NULL) == 0) {
        g.accept_thread_started = 1;
    } else {
        fprintf(stderr, "api: pthread_create failed: %s — main loop must drive accept\n",
                strerror(errno));
    }
    return fd;
}

void api_shutdown(void)
{
    if (g.accept_thread_started) {
        g.accept_thread_stop = 1;
        pthread_join(g.accept_thread, NULL);
        g.accept_thread_started = 0;
    }
    if (g.listen_fd >= 0) { close(g.listen_fd); g.listen_fd = -1; }
}

/* Kept as private helper for future use (e.g. byte-level shim-trace).
 * Currently unused — the lean concentrator does not decode HMU frames. */
static void api_log_frame(const char *channel, uint8_t dst, uint8_t cnt,
                          uint8_t plen, uint8_t cmd,
                          const uint8_t *data, size_t dlen) __attribute__((unused));
static void api_log_frame(const char *channel, uint8_t dst, uint8_t cnt,
                          uint8_t plen, uint8_t cmd,
                          const uint8_t *data, size_t dlen)
{
    pthread_mutex_lock(&g.ring_lock);
    struct ring_entry *e = &g.ring[g.ring_pos];
    e->ts_us = now_us() - g.start_us;
    snprintf(e->channel, sizeof(e->channel), "%s", channel ? channel : "");
    e->dst = dst; e->cnt = cnt; e->plen = plen; e->cmd = cmd;
    e->dlen = dlen > DATA_CAP ? DATA_CAP : dlen;
    if (data && e->dlen) memcpy(e->data, data, e->dlen);
    g.ring_pos = (g.ring_pos + 1) % RING_SIZE;
    g.ring_total++;
    pthread_mutex_unlock(&g.ring_lock);
}

/* ─── tiny IO helpers ─────────────────────────────────────────────────── */

static ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf; size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; left -= (size_t)w;
    }
    return (ssize_t)n;
}

static void send_resp(int fd, int code, const char *status,
                      const char *ctype, const char *body, size_t blen)
{
    char hdr[512];
    int  n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, ctype, blen);
    if (n < 0 || (size_t)n >= sizeof(hdr)) return;
    write_all(fd, hdr, (size_t)n);
    if (body && blen) write_all(fd, body, blen);
}

static void send_text(int fd, int code, const char *status, const char *body)
{
    send_resp(fd, code, status, "text/plain; charset=utf-8",
              body, body ? strlen(body) : 0);
}

static void send_json(int fd, int code, const char *status,
                      const char *body, size_t blen)
{
    send_resp(fd, code, status, "application/json; charset=utf-8", body, blen);
}

static void send_html(int fd, const char *body, size_t blen)
{
    send_resp(fd, 200, "OK", "text/html; charset=utf-8", body, blen);
}

/* ─── tiny json escape ────────────────────────────────────────────────── */

static int json_esc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    if (!s) s = "";
    for (; *s && n + 2 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '"';  } }
        else if (c == '\\') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = '\\'; } }
        else if (c == '\n') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'n';  } }
        else if (c == '\r') { if (n + 2 < cap) { out[n++] = '\\'; out[n++] = 'r';  } }
        else if (c >= 0x20) { out[n++] = (char)c; }
    }
    out[n] = 0;
    return (int)n;
}

/* ─── /var/run/bmcd-config.json passthrough ───────────────────────────── */

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 1024*1024) { fclose(f); return NULL; }
    rewind(f);
    char *p = malloc((size_t)sz + 1);
    if (!p) { fclose(f); return NULL; }
    size_t r = fread(p, 1, (size_t)sz, f);
    fclose(f);
    p[r] = 0;
    if (out_n) *out_n = r;
    return p;
}

/* ─── embedded SPA ────────────────────────────────────────────────────── */

/* Single-page WebUI for source/slot management.
 * Calls: GET/PUT /api/sources, GET /api/discover, GET /api/effective,
 *        POST /api/reload, GET /api/health (poll after reload). */
static const char WEBUI_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"de\"><head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>bmcond &mdash; HomeMatic-RF</title>\n"
"<style>\n"
"body{font-family:system-ui,sans-serif;margin:0;padding:1em;background:#f5f5f5;color:#222}\n"
"h1{margin:0 0 .3em 0;font-size:1.3em}\n"
"main{max-width:1100px;margin:0 auto}\n"
"#bar{display:flex;gap:.5em;align-items:center;margin:.5em 0;flex-wrap:wrap}\n"
"button{padding:.4em .9em;cursor:pointer;border:1px solid #888;background:#fff;border-radius:3px;font-size:.95em}\n"
"button.primary{background:#0066cc;color:#fff;border-color:#0066cc}\n"
"button[disabled]{opacity:.5;cursor:not-allowed}\n"
"table{border-collapse:collapse;width:100%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.1);font-size:.92em}\n"
"th,td{padding:.4em .6em;border-bottom:1px solid #eee;text-align:left;vertical-align:top}\n"
"th{background:#fafafa;font-weight:600;font-size:.85em;text-transform:uppercase;color:#555}\n"
"tr:hover td{background:#fafcff}\n"
"code{font-family:ui-monospace,monospace;font-size:.88em;background:#f0f0f0;padding:0 .25em;border-radius:2px}\n"
".tag{display:inline-block;padding:0 .4em;font-size:.75em;border-radius:3px;margin-right:.25em;font-weight:600}\n"
".tag-saved{background:#dcefff;color:#0066cc}\n"
".tag-auto{background:#fff4dc;color:#996600}\n"
".tag-active{background:#dcffdc;color:#006600}\n"
".fw-badge{display:inline-block;padding:0 .35em;font-size:.7em;border-radius:3px;margin-left:.4em;font-weight:600;cursor:help}\n"
".fw-badge-up{background:#fff4dc;color:#996600}\n"
".fw-badge-ok{background:#dcffdc;color:#006600}\n"
".fw-badge-newer{background:#e0d6ff;color:#4a3787}\n"
".fw-flash-btn{margin-left:.4em;padding:.05em .5em;font-size:.7em;border:1px solid #ff9800;background:#fff;color:#996600;border-radius:3px;cursor:pointer}\n"
".fw-flash-btn:hover{background:#ff9800;color:#fff}\n"
".fw-flash-btn-force{border-color:#aaa;color:#666}\n"
".fw-flash-btn-force:hover{background:#666;color:#fff}\n"
".dot{display:inline-block;width:.7em;height:.7em;border-radius:50%;margin-right:.3em;vertical-align:middle}\n"
".dot-green{background:#4caf50}\n"
".dot-amber{background:#ff9800}\n"
".dot-grey{background:#bbb}\n"
".banner{padding:.5em .8em;border-radius:3px;margin:.5em 0}\n"
".banner-info{background:#dcefff;color:#003366}\n"
".banner-warn{background:#fff4dc;color:#664400}\n"
".banner-err{background:#ffdcdc;color:#660000}\n"
".banner-ok{background:#dcffdc;color:#003300}\n"
"small{color:#666;display:block;font-size:.85em}\n"
".dim{opacity:.5}\n"
"footer{margin-top:1em;color:#888;font-size:.8em}\n"
".modal-bg{position:fixed;inset:0;background:rgba(0,0,0,.5);display:none;align-items:center;justify-content:center;z-index:100}\n"
".modal{background:#fff;padding:1.2em 1.5em;border-radius:5px;max-width:520px;width:92%;max-height:90vh;overflow:auto;box-shadow:0 4px 16px rgba(0,0,0,.3)}\n"
".modal h2{margin:0 0 .8em 0;font-size:1.1em}\n"
".modal label{display:block;margin:.5em 0;font-size:.92em}\n"
".modal label>span{display:block;margin-bottom:.15em;color:#444}\n"
".modal input[type=text],.modal select,.modal textarea{width:100%;padding:.4em;box-sizing:border-box;font-family:inherit;font-size:.95em;border:1px solid #bbb;border-radius:3px}\n"
".modal input:invalid{border-color:#c33}\n"
".modal fieldset{border:1px solid #ddd;padding:.4em .8em;margin:.6em 0}\n"
".modal fieldset label{display:inline-block;margin-right:1em}\n"
".modal-bar{display:flex;gap:.5em;justify-content:flex-end;margin-top:1em}\n"
".modal-err{display:none;padding:.4em .6em;background:#ffdcdc;color:#660000;border-radius:3px;margin:.4em 0;font-size:.9em}\n"
"</style></head><body><main>\n"
"<h1>HomeMatic-RF &mdash; Konfiguration</h1>\n"
"<small>bmcond JSON-API auf <code id=\"selfurl\"></code></small>\n"
"<div id=\"banner\"></div>\n"
"<div id=\"bar\">\n"
"<button id=\"btn-add\" title=\"Manuelle Quelle hinzuf&uuml;gen (TCP/UDP/USB-Pfad)\">+ Neue Quelle&hellip;</button>\n"
"<button id=\"btn-rescan\" title=\"Auto-Discovery (libusb-Enumerate) neu ausf&uuml;hren\">Discovery</button>\n"
"<button id=\"btn-apply\" class=\"primary\" title=\"sources.json speichern und bmcd reladen\">Speichern + Reload</button>\n"
"<button id=\"btn-refresh\">Aktualisieren</button>\n"
"<span id=\"vermark\" class=\"dim\"></span>\n"
"</div>\n"
"<table id=\"srctab\">\n"
"<thead><tr><th>Quelle</th><th>Transport</th><th>Modul</th><th>Caps</th><th>BidCoS</th><th>HmIP</th><th>Status</th></tr></thead>\n"
"<tbody></tbody></table>\n"
"<footer>Schema v1 &mdash; <code>/api/sources</code>, <code>/api/slots</code>, <code>/api/discover</code>, <code>/api/effective</code>, <code>/api/reload</code></footer>\n"
"</main>\n"
"<div id=\"modal-bg\" class=\"modal-bg\"><div class=\"modal\">\n"
"<h2>Neue Quelle hinzuf&uuml;gen</h2>\n"
"<form id=\"addform\" autocomplete=\"off\">\n"
"<label><span>ID <small>(persistent, <code>[a-z0-9_-]</code>, max 32)</small></span><input type=\"text\" name=\"id\" pattern=\"[a-z0-9_-]{1,32}\" maxlength=\"32\" required></label>\n"
"<label><span>Protokoll</span><select name=\"proto\">\n"
"<option value=\"tcp=\">tcp= &mdash; TCP-Bridge (z.B. ser2net)</option>\n"
"<option value=\"udp=\">udp= &mdash; UDP-Bridge (z.B. raw-uart)</option>\n"
"<option value=\"usb=\">usb= &mdash; libusb (vid:pid)</option>\n"
"<option value=\"rfusb=\">rfusb= &mdash; HmIP-RFUSB (vid:pid, AES-Layer)</option>\n"
"<option value=\"path\">/dev/&hellip; &mdash; UART-Pfad (kernel-cdc/ttyUSB)</option>\n"
"</select></label>\n"
"<label><span id=\"addr-label\">Adresse</span><input type=\"text\" name=\"addr\" required></label>\n"
"<label><span>Label <small>(freitext, default = Adresse)</small></span><input type=\"text\" name=\"label\"></label>\n"
"<fieldset><legend>Capabilities <small>(mind. eine)</small></legend>\n"
"<label><input type=\"checkbox\" name=\"cap_bidcos\"> BidCoS</label>\n"
"<label><input type=\"checkbox\" name=\"cap_hmip\"> HmIP</label>\n"
"</fieldset>\n"
"<label><span>Notes <small>(optional)</small></span><textarea name=\"notes\" rows=\"2\"></textarea></label>\n"
"<div id=\"modal-err\" class=\"modal-err\"></div>\n"
"<div class=\"modal-bar\">\n"
"<button type=\"button\" id=\"modal-cancel\">Abbrechen</button>\n"
"<button type=\"submit\" class=\"primary\">Speichern</button>\n"
"</div></form></div></div>\n"
"<script>\n"
"'use strict';\n"
"const $=s=>document.querySelector(s);\n"
"const state={sources:[],slots:{bidcos:'',hmip:''},discover:[],effective:null};\n"
"const esc=s=>String(s==null?'':s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));\n"
"function banner(kind,text){$('#banner').innerHTML=text?`<div class=\"banner banner-${kind}\">${esc(text)}</div>`:'';}\n"
"async function fetchAll(){\n"
"  banner('info','Lade Status\\u2026');\n"
"  try{\n"
"    const [s,d,e,fw]=await Promise.all([\n"
"      fetch('api/sources').then(r=>r.json()),\n"
"      fetch('api/discover').then(r=>r.json()).catch(_=>({discovered:[]})),\n"
"      fetch('api/effective').then(r=>r.json()).catch(_=>null),\n"
"      fetch('api/firmware/inventory').then(r=>r.json()).catch(_=>({families:{}})),\n"
"    ]);\n"
"    state.sources=s.sources||[];\n"
"    state.slots=s.slots||{bidcos:'',hmip:''};\n"
"    state.schema_version=s.schema_version||1;\n"
"    state.discover=(d&&d.discovered)||[];\n"
"    state.effective=e;\n"
"    state.fw_inventory=(fw&&fw.families)||{};\n"
"    autoSlotDefaults();\n"
"    banner('','');\n"
"    render();\n"
"  }catch(err){banner('err','Fehler beim Laden: '+err);}\n"
"}\n"
"// Beim Fresh-Install (sources.json noch leer) und wenn genau EINE\n"
"// Discovered-Source diese Capability anbietet, den Radio-Button im\n"
"// Browser-State vorausw\\u00e4hlen.  Persistiert wird erst beim\n"
"// \\u201eSpeichern + Reload\\u201c \\u2014 User best\\u00e4tigt explizit.\n"
"//\n"
"// **Wichtig:** Sobald sources.json existiert (= User hat einmal Save\n"
"// gedr\\u00fcckt), die User-Entscheidung absolut respektieren \\u2014 auch\n"
"// wenn ein Slot bewusst auf null gesetzt wurde.  Sonst k\\u00f6nnte\n"
"// der User HmIP nicht wieder \\u201eabschalten\\u201c, weil die Auto-Wahl es\n"
"// gleich zur\\u00fcckdr\\u00fcckte.\n"
"function autoSlotDefaults(){\n"
"  if(Array.isArray(state.sources)&&state.sources.length>0)return;\n"
"  if(!Array.isArray(state.discover)||state.discover.length===0)return;\n"
"  for(const cap of ['bidcos','hmip']){\n"
"    if(state.slots[cap])continue;\n"
"    const cands=state.discover.filter(d=>Array.isArray(d.capabilities)&&d.capabilities.includes(cap));\n"
"    if(cands.length===1){state.slots[cap]=cands[0].id;}\n"
"  }\n"
"}\n"
"function mergedRows(){\n"
"  const ids=new Set(state.sources.map(s=>s.id));\n"
"  const rows=state.sources.map(s=>({...s,_saved:true,_disc:state.discover.some(d=>d.id===s.id)}));\n"
"  for(const d of state.discover){if(!ids.has(d.id))rows.push({...d,_saved:false,_disc:true});}\n"
"  return rows;\n"
"}\n"
"function statusFor(row){\n"
"  if(!state.effective||!Array.isArray(state.effective.backends))return{color:'grey',text:'\\u2014'};\n"
"  const be=state.effective.backends.find(b=>(b.source_ids||[]).includes(row.id));\n"
"  if(!be)return{color:'grey',text:'inactive'};\n"
"  const claim=new Set(be.claim_capabilities||[]);\n"
"  const verified=new Set(be.verified_capabilities||[]);\n"
"  let allOk=true;for(const c of claim)if(!verified.has(c))allOk=false;\n"
"  const tag=be.app_tag||(be.connected?'active':'');\n"
"  return{color:allOk?'green':'amber',text:tag};\n"
"}\n"
"function moduleFor(row){\n"
"  if(!state.effective||!Array.isArray(state.effective.backends))return{kind:'',fw:''};\n"
"  const be=state.effective.backends.find(b=>(b.source_ids||[]).includes(row.id));\n"
"  if(!be)return{kind:'',fw:''};\n"
"  return{kind:be.hw_kind||'',fw:be.firmware||'',sgtin:be.sgtin||''};\n"
"}\n"
"function vcmp(a,b){\n"
"  const A=String(a||'').split('.').map(x=>parseInt(x,10)||0);\n"
"  const B=String(b||'').split('.').map(x=>parseInt(x,10)||0);\n"
"  for(let i=0;i<3;i++){if((A[i]||0)!==(B[i]||0))return (A[i]||0)-(B[i]||0);}\n"
"  return 0;\n"
"}\n"
"function fwUpdateFor(kind,currentFw){\n"
"  if(!kind||!currentFw||!state.fw_inventory)return null;\n"
"  const fam=state.fw_inventory[kind];\n"
"  if(!fam||!fam.latest)return null;\n"
"  const cmp=vcmp(currentFw,fam.latest.version);\n"
"  return{latest:fam.latest.version,current:currentFw,cmp,path:fam.latest.path,bytes:fam.latest.bytes};\n"
"}\n"
"function activeIds(){\n"
"  if(!state.effective||!Array.isArray(state.effective.backends))return new Set();\n"
"  const out=new Set();\n"
"  for(const b of state.effective.backends)for(const id of (b.source_ids||[]))out.add(id);\n"
"  return out;\n"
"}\n"
"function render(){\n"
"  const tbody=$('#srctab tbody');\n"
"  tbody.innerHTML='';\n"
"  const rows=mergedRows();\n"
"  const live=activeIds();\n"
"  for(const r of rows){\n"
"    const tags=[];\n"
"    if(r._saved)tags.push('<span class=\"tag tag-saved\">saved</span>');\n"
"    if(r._disc&&!r._saved)tags.push('<span class=\"tag tag-auto\">auto</span>');\n"
"    if(live.has(r.id))tags.push('<span class=\"tag tag-active\">active</span>');\n"
"    const caps=Array.isArray(r.capabilities)?r.capabilities:[];\n"
"    const okB=caps.includes('bidcos'),okH=caps.includes('hmip');\n"
"    const cB=state.slots.bidcos===r.id?'checked':'';\n"
"    const cH=state.slots.hmip===r.id?'checked':'';\n"
"    const st=statusFor(r);\n"
"    const mod=moduleFor(r);\n"
"    const fwUpd=fwUpdateFor(mod.kind,mod.fw);\n"
"    let fwBadge='';\n"
"    if(fwUpd){\n"
"      if(fwUpd.cmp<0){fwBadge=`<span class=\"fw-badge fw-badge-up\" title=\"Update verf\\u00fcgbar: v${esc(fwUpd.latest)} (${(fwUpd.bytes/1024).toFixed(0)} kB)\">\\u2191 v${esc(fwUpd.latest)}</span><button class=\"fw-flash-btn\" data-image=\"${esc(fwUpd.path)}\" title=\"Flash v${esc(fwUpd.latest)} (Container-Restart, ~30 s)\">Flash</button>`;}\n"
"      else if(fwUpd.cmp===0){fwBadge=`<span class=\"fw-badge fw-badge-ok\" title=\"aktuell\">\\u2713</span><button class=\"fw-flash-btn fw-flash-btn-force\" data-image=\"${esc(fwUpd.path)}\" data-force=\"1\" title=\"Re-flash (force) v${esc(fwUpd.latest)}\">Re-Flash</button>`;}\n"
"      else fwBadge=`<span class=\"fw-badge fw-badge-newer\" title=\"Modul-FW neuer als verf\\u00fcgbares Image (v${esc(fwUpd.latest)})\">v${esc(mod.fw)}\\u2009&gt;\\u2009v${esc(fwUpd.latest)}</span>`;\n"
"    }\n"
"    const modCell=mod.kind?`<b>${esc(mod.kind)}</b>${mod.fw?`<small>v${esc(mod.fw)} ${fwBadge}</small>`:''}${mod.sgtin?`<small class=\"dim\" title=\"SGTIN\">${esc(mod.sgtin)}</small>`:''}`:'<span class=\"dim\">\\u2014</span>';\n"
"    const idEsc=esc(r.id);\n"
"    const tr=document.createElement('tr');\n"
"    tr.innerHTML=`\n"
"      <td>${tags.join('')}<b>${idEsc}</b><small>${esc(r.label||'')}</small></td>\n"
"      <td><code>${esc(r.transport||'')}</code><small>${esc(r.notes||r.discovered_via||'')}</small></td>\n"
"      <td>${modCell}</td>\n"
"      <td>${esc(caps.join(', '))}</td>\n"
"      <td>${okB?`<input type=\"radio\" name=\"slot-bidcos\" value=\"${idEsc}\" ${cB}>`:'<span class=\"dim\">\\u2014</span>'}</td>\n"
"      <td>${okH?`<input type=\"radio\" name=\"slot-hmip\" value=\"${idEsc}\" ${cH}>`:'<span class=\"dim\">\\u2014</span>'}</td>\n"
"      <td><span class=\"dot dot-${st.color}\"></span>${esc(st.text)}</td>`;\n"
"    tbody.appendChild(tr);\n"
"  }\n"
"  const trNone=document.createElement('tr');\n"
"  trNone.className='dim';\n"
"  trNone.innerHTML=`\n"
"    <td><i>(keine Quelle)</i></td><td></td><td></td><td></td>\n"
"    <td><input type=\"radio\" name=\"slot-bidcos\" value=\"\" ${state.slots.bidcos===''?'checked':''}></td>\n"
"    <td><input type=\"radio\" name=\"slot-hmip\" value=\"\" ${state.slots.hmip===''?'checked':''}></td>\n"
"    <td></td>`;\n"
"  tbody.appendChild(trNone);\n"
"  $('#vermark').textContent=`schema v${state.schema_version} \\u00b7 ${rows.length} sources \\u00b7 effective: ${state.effective?'live':'offline'}`;\n"
"}\n"
"function readSlots(){\n"
"  const b=document.querySelector('input[name=slot-bidcos]:checked');\n"
"  const h=document.querySelector('input[name=slot-hmip]:checked');\n"
"  return{bidcos:b?b.value:'',hmip:h?h.value:''};\n"
"}\n"
"async function applyAndReload(){\n"
"  const slots=readSlots();\n"
"  const ids=new Set(state.sources.map(s=>s.id));\n"
"  const sources=state.sources.map(s=>({...s}));\n"
"  for(const sel of [slots.bidcos,slots.hmip]){\n"
"    if(sel&&!ids.has(sel)){\n"
"      const d=state.discover.find(x=>x.id===sel);\n"
"      if(d){sources.push({id:d.id,transport:d.transport,label:d.label||'',capabilities:d.capabilities||[],persistent:false,discovered_via:d.discovered_via||'libusb',notes:d.notes||''});ids.add(sel);}\n"
"    }\n"
"  }\n"
"  const payload={schema_version:1,sources,slots};\n"
"  $('#btn-apply').disabled=true;$('#btn-refresh').disabled=true;$('#btn-rescan').disabled=true;\n"
"  banner('info','Speichere sources.json\\u2026');\n"
"  try{\n"
"    const put=await fetch('api/sources',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});\n"
"    if(!put.ok){banner('err','PUT /api/sources fehlgeschlagen ('+put.status+'): '+await put.text());return;}\n"
"    banner('warn','Reload l\\u00e4uft \\u2014 bmcd erh\\u00e4lt SIGTERM, Container restartet (5\\u201310s)\\u2026');\n"
"    await fetch('api/reload',{method:'POST'}).catch(_=>{});\n"
"    for(let i=0;i<30;i++){\n"
"      await new Promise(r=>setTimeout(r,1000));\n"
"      try{const h=await fetch('api/health',{cache:'no-store'});if(h.ok){banner('ok','Reload abgeschlossen.');await fetchAll();return;}}catch(_){}\n"
"    }\n"
"    banner('err','Reload-Timeout (30s) \\u2014 Container-Status manuell pr\\u00fcfen.');\n"
"  }finally{$('#btn-apply').disabled=false;$('#btn-refresh').disabled=false;$('#btn-rescan').disabled=false;}\n"
"}\n"
"$('#btn-rescan').onclick=fetchAll;\n"
"$('#btn-refresh').onclick=fetchAll;\n"
"$('#btn-apply').onclick=applyAndReload;\n"
"const protoMeta={\n"
"  'tcp=':{label:'host:port',ph:'cul868-roof.local:2325',pat:'[^:\\\\s]+:[0-9]{1,5}'},\n"
"  'udp=':{label:'host:port',ph:'10.0.0.5:3008',pat:'[^:\\\\s]+:[0-9]{1,5}'},\n"
"  'usb=':{label:'vid:pid (hex)',ph:'1b1f:c020',pat:'[0-9a-fA-F]{4}:[0-9a-fA-F]{4}'},\n"
"  'rfusb=':{label:'vid:pid (hex)',ph:'1b1f:c020',pat:'[0-9a-fA-F]{4}:[0-9a-fA-F]{4}'},\n"
"  'path':{label:'Ger\\u00e4te-Pfad',ph:'/dev/ttyUSB0',pat:'/.+'},\n"
"};\n"
"function modalErr(m){const el=$('#modal-err');if(!m){el.style.display='none';el.textContent='';return;}el.textContent=m;el.style.display='block';}\n"
"function onProtoChange(){\n"
"  const p=$('#addform [name=proto]').value;const m=protoMeta[p]||protoMeta['tcp='];\n"
"  $('#addr-label').textContent='Adresse \\u2014 '+m.label;\n"
"  const addr=$('#addform [name=addr]');addr.placeholder=m.ph;addr.pattern=m.pat;\n"
"}\n"
"function suggestId(){\n"
"  const f=$('#addform');if(f.id.value)return;\n"
"  const base=(f.label.value||f.addr.value||'').toLowerCase().replace(/[^a-z0-9_-]+/g,'-').replace(/^-+|-+$/g,'').slice(0,32);\n"
"  if(base&&!state.sources.some(s=>s.id===base))f.id.value=base;\n"
"}\n"
"function openAddModal(){\n"
"  const f=$('#addform');f.reset();f.proto.value='tcp=';onProtoChange();modalErr('');\n"
"  $('#modal-bg').style.display='flex';setTimeout(()=>f.id.focus(),50);\n"
"}\n"
"function closeAddModal(){$('#modal-bg').style.display='none';}\n"
"async function onAddSubmit(ev){\n"
"  ev.preventDefault();modalErr('');\n"
"  const f=ev.target;const fd=new FormData(f);\n"
"  const id=String(fd.get('id')||'').trim();\n"
"  const proto=String(fd.get('proto'));\n"
"  const addr=String(fd.get('addr')||'').trim();\n"
"  const label=String(fd.get('label')||'').trim();\n"
"  const notes=String(fd.get('notes')||'').trim();\n"
"  if(!/^[a-z0-9_-]{1,32}$/.test(id)){modalErr('ID ung\\u00fcltig \\u2014 erlaubt: a-z, 0-9, _-, max 32 Zeichen');return;}\n"
"  if(state.sources.some(s=>s.id===id)){modalErr('ID „'+id+'\" existiert bereits');return;}\n"
"  if(!addr){modalErr('Adresse fehlt');return;}\n"
"  const m=protoMeta[proto];if(m&&m.pat&&!new RegExp('^'+m.pat+'$').test(addr)){modalErr('Adresse passt nicht zum Protokoll ('+m.label+')');return;}\n"
"  const transport=(proto==='path')?addr:proto+addr;\n"
"  const caps=[];if(fd.get('cap_bidcos'))caps.push('bidcos');if(fd.get('cap_hmip'))caps.push('hmip');\n"
"  if(caps.length===0){modalErr('Mindestens eine Capability w\\u00e4hlen');return;}\n"
"  const body={id,transport,label:label||addr,capabilities:caps,notes,persistent:true,discovered_via:'manual'};\n"
"  const submit=f.querySelector('button[type=submit]');submit.disabled=true;\n"
"  try{\n"
"    const r=await fetch('api/sources/'+encodeURIComponent(id),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});\n"
"    if(!r.ok){modalErr('POST fehlgeschlagen ('+r.status+'): '+(await r.text()).slice(0,200));return;}\n"
"    closeAddModal();await fetchAll();banner('ok','Quelle „'+id+'\" hinzugef\\u00fcgt \\u2014 jetzt Slot zuweisen + „Speichern + Reload\".');\n"
"  }catch(e){modalErr('Netzwerkfehler: '+e);}finally{submit.disabled=false;}\n"
"}\n"
"async function onFlashClick(ev){\n"
"  const btn=ev.target.closest('.fw-flash-btn'); if(!btn)return;\n"
"  const img=btn.dataset.image;\n"
"  const force=btn.dataset.force==='1';\n"
"  const fname=img.split('/').pop();\n"
"  if(!confirm(`Firmware-Update jetzt fahren?\\n\\nImage: ${fname}\\n${force?'(Re-Flash erzwungen)\\n':''}\\nbmcond restartet automatisch nach dem Flash (~30 Sekunden Stack-Unterbrechung).`))return;\n"
"  btn.disabled=true; btn.textContent='Flashing\\u2026';\n"
"  banner('warn',`Flash l\\u00e4uft: ${fname}. ~60 s; mux pausiert solange.`);\n"
"  try{\n"
"    const r=await fetch('api/firmware/flash',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({image_path:img,force})});\n"
"    const j=await r.json().catch(()=>({status:'unknown',message:''}));\n"
"    if(j.status==='ok'){banner('ok',`Flash OK \\u2014 ${esc(j.message||'')}`);}\n"
"    else if(j.status==='skipped'){banner('info',`Skipped \\u2014 ${esc(j.message||'')}`);}\n"
"    else{banner('err',`Flash ${j.status||'fail'} \\u2014 ${esc(j.message||'')}`);}\n"
"    setTimeout(fetchAll,1500);\n"
"  }catch(e){banner('err','Netzwerkfehler beim Flash: '+e);}\n"
"  finally{btn.disabled=false; btn.textContent=force?'Re-Flash':'Flash';}\n"
"}\n"
"document.addEventListener('click',onFlashClick);\n"
"$('#btn-add').onclick=openAddModal;\n"
"$('#modal-cancel').onclick=closeAddModal;\n"
"$('#modal-bg').onclick=ev=>{if(ev.target===$('#modal-bg'))closeAddModal();};\n"
"document.addEventListener('keydown',ev=>{if(ev.key==='Escape'&&$('#modal-bg').style.display==='flex')closeAddModal();});\n"
"$('#addform [name=proto]').onchange=onProtoChange;\n"
"$('#addform [name=label]').onblur=suggestId;\n"
"$('#addform [name=addr]').onblur=suggestId;\n"
"$('#addform').onsubmit=onAddSubmit;\n"
"$('#selfurl').textContent=location.host;\n"
"fetchAll();\n"
"</script></body></html>\n";

static void handle_root(int fd, const char *ingress_path)
{
    /* Direkt-Zugriff: HTML pur senden, relative URLs lösen gegen / auf. */
    if (!ingress_path || !*ingress_path) {
        send_html(fd, WEBUI_HTML, sizeof(WEBUI_HTML) - 1);
        return;
    }
    /* Ingress-Zugriff: <base href="<X-Ingress-Path>/"> nach <head> einfügen,
     * damit fetch('api/sources') und Co. relativ zum Token-Prefix auflösen. */
    const char *head_close = strstr(WEBUI_HTML, "<head>");
    if (!head_close) {
        send_html(fd, WEBUI_HTML, sizeof(WEBUI_HTML) - 1);
        return;
    }
    head_close += 6;  /* hinter "<head>" */
    size_t prefix_len = (size_t)(head_close - WEBUI_HTML);
    size_t body_len   = (sizeof(WEBUI_HTML) - 1) - prefix_len;

    char base_tag[400];
    int  base_len = snprintf(base_tag, sizeof(base_tag),
                             "\n<base href=\"%s/\">", ingress_path);
    if (base_len < 0 || base_len >= (int)sizeof(base_tag)) {
        send_html(fd, WEBUI_HTML, sizeof(WEBUI_HTML) - 1);
        return;
    }

    size_t total = prefix_len + (size_t)base_len + body_len;
    char *buf = malloc(total);
    if (!buf) {
        send_html(fd, WEBUI_HTML, sizeof(WEBUI_HTML) - 1);
        return;
    }
    memcpy(buf,                       WEBUI_HTML, prefix_len);
    memcpy(buf + prefix_len,          base_tag,   (size_t)base_len);
    memcpy(buf + prefix_len + base_len, head_close, body_len);
    send_html(fd, buf, total);
    free(buf);
}

/* ─── handlers ────────────────────────────────────────────────────────── */

static void handle_health(int fd) { send_text(fd, 200, "OK", "ok"); }

/* Build status JSON. Skeleton:
 * { "version": "...", "build_date": "...", "uptime_s": N,
 *   "stats": { rx_bidcos, rx_hmip, rx_common, tx_replies, dropped },
 *   "config_snapshot": { ... },
 *   "config_json_raw": "<contents of /var/run/bmcd-config.json or null>" }
 *
 * The SPA digs backends/endpoints out of config_json_raw (parsed as JSON).
 */
static void handle_status(int fd)
{
    uint64_t up_s = (now_us() - g.start_us) / 1000000u;

    /* read /var/run/bmcd-config.json verbatim — confgen produces it on -C */
    size_t cfg_raw_n = 0;
    char *cfg_raw = slurp("/var/run/bmcd-config.json", &cfg_raw_n);

    char tbuf[256], lbuf[256], hbuf[256], xbuf[256];
    json_esc(tbuf, sizeof(tbuf), g.ctx.cfg_transport);
    json_esc(lbuf, sizeof(lbuf), g.ctx.cfg_bidcos);
    json_esc(hbuf, sizeof(hbuf), g.ctx.cfg_hmip);
    json_esc(xbuf, sizeof(xbuf), g.ctx.cfg_extra);

    /* allocate output: cfg_raw is already JSON, embed verbatim */
    size_t cap = 4096 + cfg_raw_n;
    char *out = malloc(cap);
    if (!out) { free(cfg_raw); send_text(fd, 500, "INTERNAL", "oom"); return; }

    /* Lean-mode (multimacd-shim): bmcond zählt keine Frames mehr — die
     * Mac-Layer-Statistik liegt in multimacd.  stats-Block wird leer
     * gehalten für API-Backward-Compat. */
    int n = snprintf(out, cap,
        "{"
          "\"version\":\"%s\","
          "\"build_date\":\"%s\","
          "\"uptime_s\":%llu,"
          "\"stats\":{"
            "\"frames_logged\":%u"
          "},"
          "\"config_snapshot\":{"
            "\"transport\":\"%s\","
            "\"bidcos\":\"%s\","
            "\"hmip\":\"%s\","
            "\"extra\":\"%s\""
          "},"
          "\"runtime_config\":%s"
        "}",
        FW_VERSION_STRING, FW_BUILD_DATE, (unsigned long long)up_s,
        g.ring_total,
        tbuf, lbuf, hbuf, xbuf,
        cfg_raw ? cfg_raw : "null");

    free(cfg_raw);

    if (n < 0 || (size_t)n >= cap) { free(out); send_text(fd, 500, "INTERNAL", "fmt"); return; }
    send_json(fd, 200, "OK", out, (size_t)n);
    free(out);
}

/* GET /api/config — return the persistent cfg file as JSON. If the file
 * doesn't exist yet, fall back to the in-memory CLI snapshot. */
static int parse_kv(const char *line, char *key, size_t kcap, char *val, size_t vcap)
{
    /* Match `KEY="value"` or `KEY=value` (shell-style) */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == 0) return 0;
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    size_t klen = (size_t)(eq - line);
    if (klen >= kcap) klen = kcap - 1;
    memcpy(key, line, klen); key[klen] = 0;
    /* trim key */
    while (klen && (key[klen-1] == ' ' || key[klen-1] == '\t')) key[--klen] = 0;
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    /* strip outer quotes */
    if (*v == '"' || *v == '\'') {
        char q = *v++;
        const char *end = strchr(v, q);
        size_t vl = end ? (size_t)(end - v) : strlen(v);
        if (vl >= vcap) vl = vcap - 1;
        memcpy(val, v, vl); val[vl] = 0;
    } else {
        size_t vl = strlen(v);
        while (vl && (v[vl-1] == '\n' || v[vl-1] == '\r' ||
                      v[vl-1] == ' '  || v[vl-1] == '\t')) vl--;
        if (vl >= vcap) vl = vcap - 1;
        memcpy(val, v, vl); val[vl] = 0;
    }
    return 1;
}

static void handle_config_get(int fd)
{
    char transport[256] = "", bidcos[256] = "", hmip[256] = "", extra[256] = "";

    /* try persistent file first */
    bool from_file = false;
    if (g.ctx.cfg_path) {
        FILE *f = fopen(g.ctx.cfg_path, "r");
        if (f) {
            char line[1024], k[64], v[256];
            while (fgets(line, sizeof(line), f)) {
                if (!parse_kv(line, k, sizeof(k), v, sizeof(v))) continue;
                if      (!strcmp(k, "BMCOND_TRANSPORT"))  snprintf(transport, sizeof(transport), "%s", v);
                else if (!strcmp(k, "BMCOND_BIDCOS_PTY")) snprintf(bidcos,    sizeof(bidcos),    "%s", v);
                else if (!strcmp(k, "BMCOND_HMIP_PTY"))   snprintf(hmip,      sizeof(hmip),      "%s", v);
                else if (!strcmp(k, "BMCOND_EXTRA"))      snprintf(extra,     sizeof(extra),     "%s", v);
            }
            fclose(f);
            from_file = true;
        }
    }
    if (!from_file) {
        if (g.ctx.cfg_transport) snprintf(transport, sizeof(transport), "%s", g.ctx.cfg_transport);
        if (g.ctx.cfg_bidcos)    snprintf(bidcos,    sizeof(bidcos),    "%s", g.ctx.cfg_bidcos);
        if (g.ctx.cfg_hmip)      snprintf(hmip,      sizeof(hmip),      "%s", g.ctx.cfg_hmip);
        if (g.ctx.cfg_extra)     snprintf(extra,     sizeof(extra),     "%s", g.ctx.cfg_extra);
    }

    char tbuf[512], lbuf[512], hbuf[512], xbuf[512];
    json_esc(tbuf, sizeof(tbuf), transport);
    json_esc(lbuf, sizeof(lbuf), bidcos);
    json_esc(hbuf, sizeof(hbuf), hmip);
    json_esc(xbuf, sizeof(xbuf), extra);

    char out[2048];
    int n = snprintf(out, sizeof(out),
        "{\"source\":\"%s\","
         "\"transport\":\"%s\","
         "\"bidcos\":\"%s\","
         "\"hmip\":\"%s\","
         "\"extra\":\"%s\","
         "\"cfg_path\":\"%s\"}",
        from_file ? "file" : "cli",
        tbuf, lbuf, hbuf, xbuf,
        g.ctx.cfg_path ? g.ctx.cfg_path : "");
    send_json(fd, 200, "OK", out, (size_t)n);
}

/* Extract a JSON string field from a flat object. Returns 1 on found,
 * 0 not found. Tolerant of whitespace; expects \" delimited values. */
static int json_str_field(const char *body, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            char esc = p[1];
            if      (esc == 'n')  out[i++] = '\n';
            else if (esc == 'r')  out[i++] = '\r';
            else if (esc == 't')  out[i++] = '\t';
            else                  out[i++] = esc;
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return 1;
}

static void shell_quote(const char *in, char *out, size_t cap)
{
    /* single-quote with embedded '\'' for safety */
    size_t n = 0;
    if (n + 1 < cap) out[n++] = '"';
    for (const char *s = in; *s && n + 2 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            if (n + 2 < cap) out[n++] = '\\';
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

static void handle_config_post(int fd, const char *body, size_t blen)
{
    (void)blen;
    if (!g.ctx.cfg_path || !*g.ctx.cfg_path) {
        send_text(fd, 400, "BAD", "no cfg_path configured (read-only mode)");
        return;
    }
    char transport[256] = "", bidcos[256] = "", hmip[256] = "", extra[256] = "";
    int got = 0;
    got += json_str_field(body, "transport", transport, sizeof(transport));
    got += json_str_field(body, "bidcos",    bidcos,    sizeof(bidcos));
    got += json_str_field(body, "hmip",      hmip,      sizeof(hmip));
    got += json_str_field(body, "extra",     extra,     sizeof(extra));
    if (got == 0) {
        send_text(fd, 400, "BAD", "no recognised fields (transport/bidcos/hmip/extra)");
        return;
    }

    char qt[300], ql[300], qh[300], qx[300];
    shell_quote(transport, qt, sizeof(qt));
    shell_quote(bidcos,    ql, sizeof(ql));
    shell_quote(hmip,      qh, sizeof(qh));
    shell_quote(extra,     qx, sizeof(qx));

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.ctx.cfg_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg), "open %s failed: %s", tmp, strerror(errno));
        send_text(fd, 500, "INTERNAL", msg);
        return;
    }
    fprintf(f,
        "# bmcond Concentrator configuration — written by /api/config POST\n"
        "BMCOND_TRANSPORT=%s\n"
        "BMCOND_BIDCOS_PTY=%s\n"
        "BMCOND_HMIP_PTY=%s\n"
        "BMCOND_EXTRA=%s\n",
        qt, ql, qh, qx);
    fclose(f);
    if (rename(tmp, g.ctx.cfg_path) < 0) {
        unlink(tmp);
        char msg[512];
        snprintf(msg, sizeof(msg), "rename to %s failed: %s",
                 g.ctx.cfg_path, strerror(errno));
        send_text(fd, 500, "INTERNAL", msg);
        return;
    }

    char out[2048];
    int n = snprintf(out, sizeof(out),
        "{\"ok\":true,"
         "\"path\":\"%s\","
         "\"note\":\"saved; restart bmcond to apply (rc.d/bmcond restart)\"}",
        g.ctx.cfg_path);
    send_json(fd, 200, "OK", out, (size_t)n);
}

/* ─── /api/sources + /api/slots ──────────────────────────────────────── */

static int sources_writable(int fd)
{
    if (g.ctx.sources_path && *g.ctx.sources_path) return 1;
    send_text(fd, 503, "UNAVAILABLE", "sources_path not configured");
    return 0;
}

static void send_err_json(int fd, int code, const char *status, const char *msg)
{
    char esc[400], buf[512];
    json_esc(esc, sizeof(esc), msg ? msg : "");
    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
    send_json(fd, code, status, buf, (size_t)n);
}

static int save_sources_locked(char *err, size_t errcap)
{
    return bmcond_sources_save(g.ctx.sources_path, &g.sources, err, errcap);
}

static void respond_sources(int fd)
{
    pthread_mutex_lock(&g.sources_lock);
    char *json = bmcond_sources_to_json(&g.sources);
    pthread_mutex_unlock(&g.sources_lock);
    if (!json) { send_text(fd, 500, "INTERNAL", "serialize"); return; }
    send_json(fd, 200, "OK", json, strlen(json));
    free(json);
}

static void respond_slots(int fd)
{
    pthread_mutex_lock(&g.sources_lock);
    char b[BMCOND_SOURCE_ID_MAX], h[BMCOND_SOURCE_ID_MAX];
    snprintf(b, sizeof(b), "%s", g.sources.slots.bidcos);
    snprintf(h, sizeof(h), "%s", g.sources.slots.hmip);
    pthread_mutex_unlock(&g.sources_lock);
    char eb[64], eh[64];
    json_esc(eb, sizeof(eb), b);
    json_esc(eh, sizeof(eh), h);
    char out[256];
    int n;
    if      (*b && *h) n = snprintf(out, sizeof(out), "{\"bidcos\":\"%s\",\"hmip\":\"%s\"}", eb, eh);
    else if (*b)       n = snprintf(out, sizeof(out), "{\"bidcos\":\"%s\",\"hmip\":null}", eb);
    else if (*h)       n = snprintf(out, sizeof(out), "{\"bidcos\":null,\"hmip\":\"%s\"}", eh);
    else               n = snprintf(out, sizeof(out), "{\"bidcos\":null,\"hmip\":null}");
    send_json(fd, 200, "OK", out, (size_t)n);
}

static void handle_sources_get(int fd) { respond_sources(fd); }
static void handle_slots_get(int fd)   { respond_slots(fd); }

static void handle_sources_put(int fd, const char *body)
{
    if (!sources_writable(fd)) return;
    char err[256] = "";
    struct bmcond_sources_state new_state;
    if (bmcond_sources_from_json(body, &new_state, err, sizeof(err)) < 0) {
        send_err_json(fd, 400, "BAD REQUEST", err);
        return;
    }
    pthread_mutex_lock(&g.sources_lock);
    g.sources = new_state;
    int rc = save_sources_locked(err, sizeof(err));
    pthread_mutex_unlock(&g.sources_lock);
    if (rc < 0) { send_err_json(fd, 500, "INTERNAL", err); return; }
    respond_sources(fd);
}

static void handle_sources_post_id(int fd, const char *id, const char *body)
{
    if (!sources_writable(fd)) return;
    char err[256] = "";
    struct bmcond_source src;
    if (bmcond_source_from_json(body, &src, err, sizeof(err)) < 0) {
        send_err_json(fd, 400, "BAD REQUEST", err);
        return;
    }
    /* Path-id wins over body-id when both present.  Mismatch = client bug. */
    if (id && *id) {
        if (strlen(id) >= sizeof(src.id)) {
            send_err_json(fd, 400, "BAD REQUEST", "id too long");
            return;
        }
        if (*src.id && strcmp(src.id, id) != 0) {
            send_err_json(fd, 400, "BAD REQUEST",
                          "path id and body id do not match");
            return;
        }
        snprintf(src.id, sizeof(src.id), "%s", id);
    }
    pthread_mutex_lock(&g.sources_lock);
    int rc = bmcond_sources_upsert(&g.sources, &src, err, sizeof(err));
    if (rc == 0) rc = save_sources_locked(err, sizeof(err));
    pthread_mutex_unlock(&g.sources_lock);
    if (rc < 0) {
        int code = strstr(err, "too many") ? 507 : 400;
        const char *st = (code == 507) ? "INSUFFICIENT STORAGE" : "BAD REQUEST";
        send_err_json(fd, code, st, err);
        return;
    }
    char *out = bmcond_source_to_json(&src);
    if (out) { send_json(fd, 200, "OK", out, strlen(out)); free(out); }
    else send_text(fd, 200, "OK", "{}");
}

static void handle_sources_delete_id(int fd, const char *id)
{
    if (!sources_writable(fd)) return;
    char err[256] = "", cleared[64] = "";
    pthread_mutex_lock(&g.sources_lock);
    int rc = bmcond_sources_remove(&g.sources, id,
                                   cleared, sizeof(cleared),
                                   err, sizeof(err));
    if (rc == 0) rc = save_sources_locked(err, sizeof(err));
    pthread_mutex_unlock(&g.sources_lock);
    if (rc < 0) { send_err_json(fd, 404, "NOT FOUND", err); return; }
    char esc_id[64], esc_cleared[128], buf[512];
    json_esc(esc_id, sizeof(esc_id), id ? id : "");
    json_esc(esc_cleared, sizeof(esc_cleared), cleared);
    int n = snprintf(buf, sizeof(buf),
        "{\"deleted\":\"%s\",\"cleared_slots\":\"%s\"}", esc_id, esc_cleared);
    send_json(fd, 200, "OK", buf, (size_t)n);
}

/* json_str_or_null — like json_str_field but distinguishes "key": null
 * from "key" missing entirely.  Used by handle_slots_put. */
static int json_str_or_null(const char *body, const char *key,
                            char *out, size_t cap, int *was_null)
{
    *was_null = 0;
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!strncmp(p, "null", 4)) { *was_null = 1; return 0; }
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) p++;   /* skip escape — simple */
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static void handle_slots_put(int fd, const char *body)
{
    if (!sources_writable(fd)) return;
    char b[BMCOND_SOURCE_ID_MAX], h[BMCOND_SOURCE_ID_MAX];
    int b_null = 0, h_null = 0;
    int got_b = json_str_or_null(body, "bidcos", b, sizeof(b), &b_null);
    int got_h = json_str_or_null(body, "hmip",   h, sizeof(h), &h_null);
    if (!got_b && !b_null && !got_h && !h_null) {
        send_err_json(fd, 400, "BAD REQUEST",
                      "no slot fields — must include bidcos and/or hmip "
                      "(string id or null)");
        return;
    }
    char err[256] = "";
    pthread_mutex_lock(&g.sources_lock);
    /* Unspecified slots keep their current value. */
    const char *new_b = got_b ? b : (b_null ? "" : g.sources.slots.bidcos);
    const char *new_h = got_h ? h : (h_null ? "" : g.sources.slots.hmip);
    int rc = bmcond_sources_set_slots(&g.sources, new_b, new_h, err, sizeof(err));
    if (rc == 0) rc = save_sources_locked(err, sizeof(err));
    pthread_mutex_unlock(&g.sources_lock);
    if (rc < 0) { send_err_json(fd, 400, "BAD REQUEST", err); return; }
    respond_slots(fd);
}

/* ─── /api/discover ──────────────────────────────────────────────────── */

/* Sanitize an arbitrary string into [a-z0-9_-]{1,N} for use as id-suffix.
 * Anything else collapses to '-'. */
static void sanitize_id_part(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    int last_dash = 0;
    for (const char *p = src; *p && n + 1 < cap; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            dst[n++] = c;
            last_dash = (c == '-' || c == '_');
        } else if (!last_dash) {
            dst[n++] = '-';
            last_dash = 1;
        }
    }
    /* trim trailing dash */
    while (n && (dst[n-1] == '-' || dst[n-1] == '_')) --n;
    dst[n] = 0;
}

/* ─── mDNS discovery (Stage 47b.2) ───────────────────────────────────── */

/* One mDNS hit, parsed from one '=' line of avahi-browse -p output.
 * Service-types we browse: _hmuartlgw._tcp (HMUARTLGW classic + DualCoPro),
 * _raw-uart._udp (hb-rf-eth), _culfw._tcp (legacy CUL host-marker). */
struct mdns_hit {
    char service[32];        /* "_hmuartlgw._tcp" etc. */
    char instance[64];       /* service-instance name */
    char hostname[128];      /* resolved hostname (.local) */
    char address[64];        /* IPv4 (or IPv6 textual) */
    int  port;               /* SRV port */
    char txt[512];           /* raw TXT records — quoted "k=v" tokens */
};

/* Browse one service-type via popen("avahi-browse -r -t -p <type>").
 * Returns count, or -1 if avahi-browse missing / error.  Caller capped
 * at <max>.  Output format:
 *   =;<iface>;IPv4;<instance>;<service>;<domain>;<host>;<addr>;<port>;<txt>
 * Six leading fields, then host/addr/port, then TXT (rest of line).  Skip
 * IPv6-marker lines for v1 (could surface dual-stack later). */
static int mdns_browse_one(const char *service_type,
                           struct mdns_hit *out, int max)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "avahi-browse -r -t -p %s 2>/dev/null", service_type);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int n = 0;
    char line[2048];
    while (fgets(line, sizeof(line), p) && n < max) {
        if (line[0] != '=') continue;
        char *fields[10] = {0};
        int nf = 0;
        char *s = line;
        while (nf < 9) {
            char *next = strchr(s, ';');
            if (!next) break;
            *next = 0;
            fields[nf++] = s;
            s = next + 1;
        }
        if (nf < 9) continue;
        fields[9] = s;
        size_t L = strlen(s);
        while (L > 0 && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = 0;
        /* fields[2] is "IPv4"|"IPv6" — take v4 only for v1 */
        if (strcmp(fields[2], "IPv4") != 0) continue;
        struct mdns_hit *h = &out[n++];
        memset(h, 0, sizeof(*h));
        snprintf(h->service,  sizeof(h->service),  "%s", fields[4]);
        snprintf(h->instance, sizeof(h->instance), "%s", fields[3]);
        snprintf(h->hostname, sizeof(h->hostname), "%s", fields[6]);
        snprintf(h->address,  sizeof(h->address),  "%s", fields[7]);
        h->port = atoi(fields[8]);
        snprintf(h->txt, sizeof(h->txt), "%s", fields[9]);
    }
    pclose(p);
    return n;
}

/* Pull one quoted "key=value" token from TXT-record blob.  Returns 1 if
 * found, 0 if missing.  Used to support the schema's `caps=`, `wire=`,
 * `fwver=` overrides (docs/sources_schema.md). */
static int mdns_txt_get(const char *txt, const char *key,
                        char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s=", key);
    const char *p = strstr(txt, pat);
    if (!p) return 0;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t L = (size_t)(end - p);
    if (L >= cap) L = cap - 1;
    memcpy(out, p, L);
    out[L] = 0;
    return 1;
}

/* Parse "bidcos,hmip" → cap_bidcos / cap_hmip.  Whitespace-tolerant. */
static void parse_caps_csv(const char *csv, bool *cb, bool *ch)
{
    *cb = false; *ch = false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", csv);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ' || *t == '\t') ++t;
        char *e = t + strlen(t);
        while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if      (!strcmp(t, "bidcos")) *cb = true;
        else if (!strcmp(t, "hmip"))   *ch = true;
    }
}

/* Token-search: returns true iff <needle> appears as a comma-separated
 * token in <csv> (whitespace-tolerant).  Used for CULFW32's
 * `bidcos=tx,rx` and `hmip=rx-sniff` TXT-records — capability requires
 * the "tx" token to be present (rx-only / sniff is not "this source is
 * usable for sending"). */
static bool csv_has_token(const char *csv, const char *needle)
{
    if (!csv || !*csv) return false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", csv);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ' || *t == '\t') ++t;
        char *e = t + strlen(t);
        while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!strcmp(t, needle)) return true;
    }
    return false;
}

static void handle_discover(int fd)
{
    struct usb_discovery_hit hits[16];
    int n = transport_usb_discover(hits, 16);
    if (n < 0) {
        send_err_json(fd, 500, "INTERNAL", "libusb discover failed");
        return;
    }

    /* Detect duplicate VID:PID — those need iserial/bus-port suffix on id. */
    int dup[16] = {0};
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) continue;
            if (hits[i].vid == hits[j].vid && hits[i].pid == hits[j].pid) {
                dup[i] = 1; break;
            }
        }
    }

    /* Build a JSON object: {"discovered":[...],"host_markers":[]} */
    cJSON *root = cJSON_CreateObject();
    if (!root) { send_text(fd, 500, "INTERNAL", "oom"); return; }
    cJSON *arr  = cJSON_AddArrayToObject(root, "discovered");

    for (int i = 0; i < n; ++i) {
        struct bmcond_source src;
        memset(&src, 0, sizeof(src));

        /* id */
        char base[33];
        snprintf(base, sizeof(base), "usb-%04x-%04x", hits[i].vid, hits[i].pid);
        if (dup[i]) {
            char sfx[24] = "";
            if (*hits[i].iserial)
                sanitize_id_part(sfx, sizeof(sfx), hits[i].iserial);
            else
                sanitize_id_part(sfx, sizeof(sfx), hits[i].bus_port);
            snprintf(src.id, sizeof(src.id), "%s-%s", base, sfx);
        } else {
            snprintf(src.id, sizeof(src.id), "%s", base);
        }

        snprintf(src.transport, sizeof(src.transport),
                 "usb=%04x:%04x", hits[i].vid, hits[i].pid);
        snprintf(src.label, sizeof(src.label), "%s",
                 hits[i].kind_hint ? hits[i].kind_hint : "USB radio");
        /* All sticks in the quirks table are DualCoPro radios; declared
         * caps are bidcos+hmip.  Whether HmIP actually carries traffic
         * is verified at backend-up via COMMON_IDENTIFY (Stage 47c). */
        src.cap_bidcos = true;
        src.cap_hmip   = true;
        src.persistent = false;
        snprintf(src.discovered_via, sizeof(src.discovered_via), "libusb");
        if (*hits[i].iserial || *hits[i].bus_port) {
            snprintf(src.notes, sizeof(src.notes),
                     "iserial=%s bus_port=%s",
                     *hits[i].iserial ? hits[i].iserial : "(unread)",
                     hits[i].bus_port);
        }

        cJSON *src_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(src_obj, "id",        src.id);
        cJSON_AddStringToObject(src_obj, "transport", src.transport);
        cJSON_AddStringToObject(src_obj, "label",     src.label);
        cJSON *caps = cJSON_AddArrayToObject(src_obj, "capabilities");
        cJSON_AddItemToArray(caps, cJSON_CreateString("bidcos"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("hmip"));
        cJSON_AddBoolToObject(src_obj,   "persistent",     false);
        cJSON_AddStringToObject(src_obj, "discovered_via", "libusb");
        if (*src.notes)
            cJSON_AddStringToObject(src_obj, "notes", src.notes);
        cJSON_AddItemToArray(arr, src_obj);
    }

    /* ── mDNS-browse three service-types (Stage 47b.2).
     * If avahi-browse is missing, mdns_browse_one returns -1; we silently
     * skip mDNS — discovery is still useful for libusb-only setups.
     *
     * Defaults from docs/sources_schema.md mDNS-Service-Discovery-Table:
     *   _culfw._tcp     (2323) — host-marker (NOT a Source in v1)
     *   _hmuartlgw._tcp (2325 classic / 2327 DualCoPro) — bidcos default
     *   _raw-uart._udp  (3008 hb-rf-eth) — bidcos default
     *
     * TXT-record overrides (caps=…, wire=…, fwver=…) win over defaults. */
    cJSON *host_markers = cJSON_AddArrayToObject(root, "host_markers");

    struct mdns_hit mh[32];
    int mcount;
    /* (1) _hmuartlgw._tcp — 2325 (classic) and 2327 (dualcopro) */
    mcount = mdns_browse_one("_hmuartlgw._tcp", mh, 32);
    for (int i = 0; mcount > 0 && i < mcount; ++i) {
        struct mdns_hit *h = &mh[i];
        char id_sfx[32];
        sanitize_id_part(id_sfx, sizeof(id_sfx), h->hostname);
        char id[33];
        snprintf(id, sizeof(id), "mdns-%s-%d", id_sfx, h->port);

        /* Capability resolution priority:
         *   1) explicit `caps=…` TXT (per docs/sources_schema.md)
         *   2) CULFW32-style `bidcos=…` / `hmip=…` TXT — capability iff
         *      the value contains "tx" (rx/sniff doesn't qualify as a
         *      usable-for-sending source).
         *   3) service-type default: bidcos. */
        bool cb, ch;
        char caps_buf[128] = "";
        if (mdns_txt_get(h->txt, "caps", caps_buf, sizeof(caps_buf))) {
            parse_caps_csv(caps_buf, &cb, &ch);
        } else {
            char bidcos_v[64] = "", hmip_v[64] = "";
            int has_b = mdns_txt_get(h->txt, "bidcos", bidcos_v, sizeof(bidcos_v));
            int has_h = mdns_txt_get(h->txt, "hmip",   hmip_v,   sizeof(hmip_v));
            if (has_b || has_h) {
                cb = has_b && csv_has_token(bidcos_v, "tx");
                ch = has_h && csv_has_token(hmip_v,   "tx");
            } else {
                cb = true; ch = false;   /* service-type default */
            }
        }

        char wire[32] = "", fwver[32] = "", model[32] = "";
        mdns_txt_get(h->txt, "wire",  wire,  sizeof(wire));
        if (!mdns_txt_get(h->txt, "fwver", fwver, sizeof(fwver)))
            mdns_txt_get(h->txt, "fw",    fwver, sizeof(fwver));
        mdns_txt_get(h->txt, "model", model, sizeof(model));

        char transport[256], notes[257], label[64];
        snprintf(transport, sizeof(transport),
                 "tcp=%s:%d", h->hostname, h->port);
        snprintf(label, sizeof(label),
                 "%s%s%s",
                 *h->instance ? h->instance : "HMUARTLGW",
                 *model ? " " : (*wire ? " " : ""),
                 *model ? model : (*wire ? wire : ""));
        snprintf(notes, sizeof(notes),
                 "service=%s%s%s%s%s",
                 h->service,
                 *fwver ? " fw=" : "",
                 *fwver ? fwver : "",
                 *h->address ? " addr=" : "",
                 *h->address ? h->address : "");

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",        id);
        cJSON_AddStringToObject(o, "transport", transport);
        cJSON_AddStringToObject(o, "label",     label);
        cJSON *caps = cJSON_AddArrayToObject(o, "capabilities");
        if (cb) cJSON_AddItemToArray(caps, cJSON_CreateString("bidcos"));
        if (ch) cJSON_AddItemToArray(caps, cJSON_CreateString("hmip"));
        cJSON_AddBoolToObject  (o, "persistent",     false);
        cJSON_AddStringToObject(o, "discovered_via", "mdns");
        cJSON_AddStringToObject(o, "notes",          notes);
        cJSON_AddItemToArray(arr, o);
    }
    /* (2) _raw-uart._udp — 3008 hb-rf-eth */
    mcount = mdns_browse_one("_raw-uart._udp", mh, 32);
    for (int i = 0; mcount > 0 && i < mcount; ++i) {
        struct mdns_hit *h = &mh[i];
        char id_sfx[32];
        sanitize_id_part(id_sfx, sizeof(id_sfx), h->hostname);
        char id[33];
        snprintf(id, sizeof(id), "mdns-%s-%d", id_sfx, h->port);

        bool cb = true, ch = false;   /* default: bidcos */
        char caps_buf[128] = "";
        if (mdns_txt_get(h->txt, "caps", caps_buf, sizeof(caps_buf)))
            parse_caps_csv(caps_buf, &cb, &ch);

        char transport[256], notes[257];
        snprintf(transport, sizeof(transport),
                 "udp=%s:%d", h->hostname, h->port);
        snprintf(notes, sizeof(notes),
                 "service=%s wire=hb-rf-eth%s%s",
                 h->service,
                 *h->address ? " addr=" : "",
                 *h->address ? h->address : "");

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id",        id);
        cJSON_AddStringToObject(o, "transport", transport);
        cJSON_AddStringToObject(o, "label",
            *h->instance ? h->instance : "hb-rf-eth (UDP)");
        cJSON *caps = cJSON_AddArrayToObject(o, "capabilities");
        if (cb) cJSON_AddItemToArray(caps, cJSON_CreateString("bidcos"));
        if (ch) cJSON_AddItemToArray(caps, cJSON_CreateString("hmip"));
        cJSON_AddBoolToObject  (o, "persistent",     false);
        cJSON_AddStringToObject(o, "discovered_via", "mdns");
        cJSON_AddStringToObject(o, "notes",          notes);
        cJSON_AddItemToArray(arr, o);
    }
    /* (3) _culfw._tcp — host-marker only (NOT a Source in v1) */
    mcount = mdns_browse_one("_culfw._tcp", mh, 32);
    for (int i = 0; mcount > 0 && i < mcount; ++i) {
        struct mdns_hit *h = &mh[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "service",  h->service);
        cJSON_AddStringToObject(o, "instance", h->instance);
        cJSON_AddStringToObject(o, "host",     h->hostname);
        if (*h->address)
            cJSON_AddStringToObject(o, "address", h->address);
        cJSON_AddNumberToObject  (o, "port",    h->port);
        cJSON_AddItemToArray(host_markers, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { send_text(fd, 500, "INTERNAL", "serialize"); return; }
    send_json(fd, 200, "OK", json, strlen(json));
    free(json);
}

/* ─── /api/effective + /api/reload ───────────────────────────────────── */

/* Build the effective-state JSON: merges /var/run/bmcd-config.json (the
 * confgen snapshot — what's actually running this PID), bridge_state stats,
 * and the slot mapping from sources.json.  Adds claim/verified-capabilities
 * derived from app_tag + dual_stack.  Read-only, no side effects. */
static void handle_effective(int fd)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { send_text(fd, 500, "INTERNAL", "oom"); return; }

    /* "running" — version + uptime. */
    cJSON *running = cJSON_AddObjectToObject(root, "running");
    cJSON_AddStringToObject(running, "version",     FW_VERSION_STRING);
    cJSON_AddStringToObject(running, "build_date",  FW_BUILD_DATE);
    uint64_t up_s = (now_us() - g.start_us) / 1000000u;
    cJSON_AddNumberToObject(running, "uptime_s",    (double)up_s);

    /* "stats" — Mac-Layer-Counter sind in multimacd, nicht hier.
     * Block bleibt leer (frames_logged auf in-memory-ring). */
    cJSON *stats = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(stats, "frames_logged", g.ring_total);

    /* "slots" — current slot assignment from sources.json. */
    pthread_mutex_lock(&g.sources_lock);
    char slot_b[BMCOND_SOURCE_ID_MAX], slot_h[BMCOND_SOURCE_ID_MAX];
    snprintf(slot_b, sizeof(slot_b), "%s", g.sources.slots.bidcos);
    snprintf(slot_h, sizeof(slot_h), "%s", g.sources.slots.hmip);
    pthread_mutex_unlock(&g.sources_lock);
    cJSON *slots = cJSON_AddObjectToObject(root, "slots");
    if (*slot_b) cJSON_AddStringToObject(slots, "bidcos", slot_b);
    else         cJSON_AddNullToObject  (slots, "bidcos");
    if (*slot_h) cJSON_AddStringToObject(slots, "hmip",   slot_h);
    else         cJSON_AddNullToObject  (slots, "hmip");

    /* "backends" — derived from /var/run/bmcd-config.json (confgen snapshot)
     * plus claim/verified capabilities from app_tag + dual_stack. */
    size_t cfg_n = 0;
    char *cfg = slurp("/var/run/bmcd-config.json", &cfg_n);
    cJSON *cfg_root = cfg ? cJSON_Parse(cfg) : NULL;
    cJSON *cfg_backends = cfg_root
        ? cJSON_GetObjectItemCaseSensitive(cfg_root, "backends") : NULL;
    cJSON *out_backends = cJSON_AddArrayToObject(root, "backends");

    if (cfg_backends && cJSON_IsArray(cfg_backends)) {
        int n = cJSON_GetArraySize(cfg_backends);
        for (int i = 0; i < n; ++i) {
            cJSON *src = cJSON_GetArrayItem(cfg_backends, i);
            cJSON *dst = cJSON_CreateObject();
            const char *fields[] = {
                "idx","name","hw_kind","transport","app_tag",
                "firmware","serial","sgtin","bidcos_addr","hmip_addr"
            };
            for (size_t k = 0; k < sizeof(fields)/sizeof(fields[0]); ++k) {
                cJSON *e = cJSON_GetObjectItemCaseSensitive(src, fields[k]);
                if (e) cJSON_AddItemToObject(dst, fields[k], cJSON_Duplicate(e, 1));
            }
            cJSON *dual = cJSON_GetObjectItemCaseSensitive(src, "dual_stack");
            int dual_stack = dual && cJSON_IsBool(dual) && cJSON_IsTrue(dual);
            cJSON *atag_e = cJSON_GetObjectItemCaseSensitive(src, "app_tag");
            const char *atag = (atag_e && cJSON_IsString(atag_e))
                             ? atag_e->valuestring : "";

            /* claim: what the backend is configured to support — derived
             * from confgen's dual_stack flag (sources.json could refine
             * later). */
            cJSON *claim = cJSON_AddArrayToObject(dst, "claim_capabilities");
            cJSON_AddItemToArray(claim, cJSON_CreateString("bidcos"));
            if (dual_stack) cJSON_AddItemToArray(claim, cJSON_CreateString("hmip"));

            /* verified: was die Firmware live nachgewiesen hat.
             *
             * BidCoS-Verify (app_tag-basiert): die COMMON_IDENTIFY-Probe in
             *   radio_dualcopro.c liefert app_tag *_App.  BidCoS-RX ist
             *   universal in jeder DualCoPro-App-Firmware — app_tag-Match
             *   genügt als Beleg.
             *
             * HmIP-Verify (traffic-basiert): app_tag reicht NICHT — die
             *   DualCoPro_App-Firmware initialisiert ihren HmIP-Stack
             *   auch auf CC1101-only-Hardware (HM-MOD-RPI-PCB), und USB-
             *   Layer-Probes (GET_ADAPTER_MIC, SEND_PROTOCOL_FRAME) liefern
             *   identische Antworten egal ob CC1200 vorhanden ist.  Belegt
             *   im hmip_probe-Dual-Stick-Test, siehe memory
             *   `hmip_probe_dual_stick_result.md`.  Hardware-Differenzierer
             *   ist nur am echten RF-TX messbar.
             *
             *   Lean-Mode: bmcond zählt keine Frames mehr (Mac-Layer in
             *   multimacd). verified_hmip kann nicht mehr aus rx_hmip
             *   abgeleitet werden — wir fallen auf claim-basiert zurück
             *   (dual_stack-Flag aus confgen-bmcd-config.json).  WebUI
             *   zeigt dann dasselbe für claim und verified, was für
             *   den lean-Pfad ok ist. */
            int verified_bidcos = (*atag != 0) && strstr(atag, "App") != NULL;
            int verified_hmip   = verified_bidcos && dual_stack;
            cJSON *verified = cJSON_AddArrayToObject(dst, "verified_capabilities");
            if (verified_bidcos) cJSON_AddItemToArray(verified, cJSON_CreateString("bidcos"));
            if (verified_hmip)   cJSON_AddItemToArray(verified, cJSON_CreateString("hmip"));

            /* connected: simple proxy = boot probe completed successfully. */
            cJSON_AddBoolToObject(dst, "connected", verified_bidcos != 0);

            cJSON_AddItemToArray(out_backends, dst);
        }
    }

    /* "endpoints" — pass through verbatim. */
    cJSON *cfg_eps = cfg_root
        ? cJSON_GetObjectItemCaseSensitive(cfg_root, "endpoints") : NULL;
    cJSON *out_eps = cJSON_AddArrayToObject(root, "endpoints");
    if (cfg_eps && cJSON_IsArray(cfg_eps)) {
        int n = cJSON_GetArraySize(cfg_eps);
        for (int i = 0; i < n; ++i)
            cJSON_AddItemToArray(out_eps,
                cJSON_Duplicate(cJSON_GetArrayItem(cfg_eps, i), 1));
    }

    /* Enrich each backend with source_ids[] — the source-id(s) from
     * sources.json whose slots route through this backend AND whose
     * transport matches the backend's actual transport.  Bug 2026-05-08:
     * confgen koppelt für DualCoPro-Module beide Endpoints (bidcos+hmip)
     * an dasselbe Backend.  Wenn der User für hmip einen anderen slot-
     * source-id konfiguriert hat als für bidcos (z.B. weil HmIP-RFUSB
     * früher steckte), würde der WebUI beide als "active" anzeigen,
     * obwohl nur eine Hardware physisch da ist.
     * Fix: source.transport-suffix gegen backend.transport vergleichen
     * und nur attachen wenn es matcht. */
    if (cJSON_IsArray(out_backends) && cJSON_IsArray(out_eps)) {
        int nbe = cJSON_GetArraySize(out_backends);
        int neps = cJSON_GetArraySize(out_eps);
        for (int i = 0; i < nbe; ++i) {
            cJSON *be = cJSON_GetArrayItem(out_backends, i);
            cJSON *idx_e = cJSON_GetObjectItemCaseSensitive(be, "idx");
            int be_idx = (idx_e && cJSON_IsNumber(idx_e)) ? idx_e->valueint : i;
            cJSON *be_xport_e = cJSON_GetObjectItemCaseSensitive(be, "transport");
            const char *be_xport = (be_xport_e && cJSON_IsString(be_xport_e))
                                 ? be_xport_e->valuestring : "";
            cJSON *sids = cJSON_AddArrayToObject(be, "source_ids");
            for (int j = 0; j < neps; ++j) {
                cJSON *ep = cJSON_GetArrayItem(out_eps, j);
                cJSON *bx_e = cJSON_GetObjectItemCaseSensitive(ep, "backend_idx");
                cJSON *role_e = cJSON_GetObjectItemCaseSensitive(ep, "role");
                if (!bx_e || !cJSON_IsNumber(bx_e)) continue;
                if (bx_e->valueint != be_idx) continue;
                if (!role_e || !cJSON_IsString(role_e)) continue;
                const char *role = role_e->valuestring;
                const char *sid = NULL;
                if      (!strcmp(role, "bidcos") && *slot_b) sid = slot_b;
                else if (!strcmp(role, "hmip")   && *slot_h) sid = slot_h;
                if (!sid) continue;

                /* Verify source.transport matches backend.transport.
                 * Source-Transports haben Prefix wie "usb=", "tcp=",
                 * "rfusb=" — der Suffix nach "=" ist der eigentliche
                 * Connector und muss zum backend.transport passen. */
                pthread_mutex_lock(&g.sources_lock);
                const struct bmcond_source *src = bmcond_sources_find(&g.sources, sid);
                const char *src_xport = src ? src->transport : NULL;
                int xport_match = 0;
                if (src_xport && *be_xport) {
                    const char *connector = strchr(src_xport, '=');
                    connector = connector ? connector + 1 : src_xport;
                    if (!strcmp(connector, be_xport)) xport_match = 1;
                }
                pthread_mutex_unlock(&g.sources_lock);
                if (!xport_match) continue;

                /* dedupe — a single source_id may serve both roles */
                int seen = 0;
                int ns = cJSON_GetArraySize(sids);
                for (int k = 0; k < ns; ++k) {
                    cJSON *s = cJSON_GetArrayItem(sids, k);
                    if (s && cJSON_IsString(s) && !strcmp(s->valuestring, sid)) {
                        seen = 1; break;
                    }
                }
                if (!seen) cJSON_AddItemToArray(sids, cJSON_CreateString(sid));
            }
        }
    }

    if (cfg_root) cJSON_Delete(cfg_root);
    free(cfg);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { send_text(fd, 500, "INTERNAL", "serialize"); return; }
    send_json(fd, 200, "OK", json, strlen(json));
    free(json);
}

/* GET /api/firmware/inventory — list available .eq3 files per
 * hw_kind family, with parsed versions and the latest.
 *
 * Returns:
 *   { "families": {
 *       "HMIP-RFUSB":      { "dir":"/firmware/HmIP-RFUSB",
 *                            "files":[ {name, version, path, bytes}, ... ],
 *                            "latest": {name, version, path, bytes} },
 *       "RPI-RF-MOD":      { ... },
 *       "HM-MOD-RPI-PCB":  { ... },
 *     }
 *   }
 *
 * Filename-Pattern: `*-X.Y.Z.eq3` (regex `(\d+\.\d+\.\d+)\.eq3$`).
 * Files ohne version-suffix werden ignoriert (z.B. legacy
 * `coprocessor_update.eq3` ohne -X.Y.Z). */
static int parse_filename_version(const char *name, uint8_t out[3])
{
    /* find last "-X.Y.Z" before .eq3 suffix */
    size_t n = strlen(name);
    if (n < 8) return -1;
    if (strcmp(name + n - 4, ".eq3") != 0) return -1;
    /* scan backwards for the dash that starts -X.Y.Z */
    int dot_count = 0;
    size_t i = n - 4;  /* points at '.' of .eq3 */
    while (i > 0) {
        char c = name[i - 1];
        if (c == '.') dot_count++;
        else if (c == '-' && dot_count == 2) {
            unsigned a, b, c2;
            if (sscanf(name + i, "%u.%u.%u.eq3", &a, &b, &c2) == 3 &&
                a < 256 && b < 256 && c2 < 256) {
                out[0] = (uint8_t)a;
                out[1] = (uint8_t)b;
                out[2] = (uint8_t)c2;
                return 0;
            }
            return -1;
        } else if (c < '0' || (c > '9' && c != '.' && c != '-')) {
            return -1;
        }
        --i;
    }
    return -1;
}

static int version_cmp(const uint8_t a[3], const uint8_t b[3])
{
    if (a[0] != b[0]) return (int)a[0] - (int)b[0];
    if (a[1] != b[1]) return (int)a[1] - (int)b[1];
    return (int)a[2] - (int)b[2];
}

static void handle_firmware_inventory(int cfd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *families = cJSON_AddObjectToObject(root, "families");

    struct fw_family {
        const char *hw_kind;
        const char *dir;
    } fams[] = {
        { "HMIP-RFUSB",     "/firmware/HmIP-RFUSB" },
        { "HMIP-RFUSB-TK",  "/firmware/HmIP-RFUSB" },  /* same dir, TK-Variante */
        { "RPI-RF-MOD",     "/firmware/RPI-RF-MOD" },
        { "HM-MOD-RPI-PCB", "/firmware/HM-MOD-UART" },
    };
    for (size_t k = 0; k < sizeof(fams)/sizeof(fams[0]); ++k) {
        cJSON *fam = cJSON_AddObjectToObject(families, fams[k].hw_kind);
        cJSON_AddStringToObject(fam, "dir", fams[k].dir);
        cJSON *files = cJSON_AddArrayToObject(fam, "files");

        DIR *d = opendir(fams[k].dir);
        if (!d) {
            cJSON_AddStringToObject(fam, "error", strerror(errno));
            continue;
        }
        struct dirent *ent;
        uint8_t latest_v[3] = {0,0,0};
        char    latest_name[256] = {0};
        size_t  latest_bytes = 0;
        while ((ent = readdir(d)) != NULL) {
            uint8_t v[3];
            if (parse_filename_version(ent->d_name, v) != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", fams[k].dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            cJSON *fe = cJSON_CreateObject();
            cJSON_AddStringToObject(fe, "name", ent->d_name);
            char vstr[16];
            snprintf(vstr, sizeof(vstr), "%u.%u.%u", v[0], v[1], v[2]);
            cJSON_AddStringToObject(fe, "version", vstr);
            cJSON_AddStringToObject(fe, "path", path);
            cJSON_AddNumberToObject(fe, "bytes", (double)st.st_size);
            cJSON_AddItemToArray(files, fe);
            if (version_cmp(v, latest_v) > 0) {
                memcpy(latest_v, v, 3);
                snprintf(latest_name, sizeof(latest_name), "%s", ent->d_name);
                latest_bytes = (size_t)st.st_size;
            }
        }
        closedir(d);
        if (latest_name[0]) {
            cJSON *latest = cJSON_AddObjectToObject(fam, "latest");
            cJSON_AddStringToObject(latest, "name", latest_name);
            char vstr[16];
            snprintf(vstr, sizeof(vstr), "%u.%u.%u", latest_v[0], latest_v[1], latest_v[2]);
            cJSON_AddStringToObject(latest, "version", vstr);
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", fams[k].dir, latest_name);
            cJSON_AddStringToObject(latest, "path", path);
            cJSON_AddNumberToObject(latest, "bytes", (double)latest_bytes);
        }
    }
    char *out = cJSON_PrintUnformatted(root);
    send_resp(cfd, 200, "OK", "application/json", out, out ? strlen(out) : 0);
    free(out);
    cJSON_Delete(root);
}

/* POST /api/firmware/flash — schreibt /var/run/bmcd-flash-job.json mit
 * dem Flash-Job (image_path + optional backend + force-flag) und
 * triggert reload-self.  Beim nächsten bmcond-Start wird der Job
 * VOR boot_to_app verarbeitet (Modul ist nach reset_pulse im BL,
 * perfekter Zeitpunkt für flash). Concentrator löscht die Job-Datei
 * nach Verarbeitung. */
static void handle_firmware_flash(int cfd, const char *body)
{
    if (!body || !*body) {
        send_text(cfd, 400, "BAD REQUEST", "missing JSON body");
        return;
    }
    cJSON *req = cJSON_Parse(body);
    if (!req) { send_text(cfd, 400, "BAD REQUEST", "invalid JSON"); return; }
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "image_path");
    if (!jp || !cJSON_IsString(jp)) {
        cJSON_Delete(req);
        send_text(cfd, 400, "BAD REQUEST", "image_path required");
        return;
    }
    const char *img = jp->valuestring;
    /* Sanity: only allow under /firmware/ to prevent traversal */
    if (strncmp(img, "/firmware/", 10) != 0 || strstr(img, "..") != NULL) {
        cJSON_Delete(req);
        send_text(cfd, 400, "BAD REQUEST",
                  "image_path must be under /firmware/ and contain no '..'");
        return;
    }
    if (access(img, R_OK) != 0) {
        cJSON_Delete(req);
        send_text(cfd, 404, "NOT FOUND", "image file not readable");
        return;
    }
    const char *backend = "";
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(req, "backend");
    if (jb && cJSON_IsString(jb)) backend = jb->valuestring;
    int force = 0;
    cJSON *jf = cJSON_GetObjectItemCaseSensitive(req, "force");
    if (jf && cJSON_IsBool(jf) && cJSON_IsTrue(jf)) force = 1;

    /* Snapshot strings into stack-buffers before cJSON_Delete invalidates
     * jp/jb pointers (bug 2026-05-08: log-line vorher dangling). */
    char img_buf[256], be_buf[16];
    snprintf(img_buf, sizeof(img_buf), "%s", img);
    snprintf(be_buf,  sizeof(be_buf),  "%s", backend);
    cJSON_Delete(req);

    fprintf(stderr,
        "API: /api/firmware/flash inline image='%s' backend='%s' force=%d\n",
        img_buf, be_buf, force);

    /* Inline-Flash: synchroner Aufruf in den main-loop, blockiert bis
     * flash done.  Kein Container-Restart, rfd/HMServer bleiben am
     * /dev/mmd_*-PTY gehängt während ~60s flash-pause. */
    extern int bmcd_inline_flash_request(const char *image_path,
                                         const char *backend_name,
                                         int force, int timeout_ms,
                                         char *msg_out, size_t msg_cap);
    char rep_msg[160] = {0};
    int rc = bmcd_inline_flash_request(img_buf, be_buf, force, 120000,
                                       rep_msg, sizeof(rep_msg));

    cJSON *root = cJSON_CreateObject();
    const char *st_str = (rc == 0) ? "ok"
                       : (rc == 1) ? "skipped"
                       : (rc == 2) ? "fail" : "timeout";
    cJSON_AddStringToObject(root, "status", st_str);
    cJSON_AddStringToObject(root, "message", rep_msg);
    cJSON_AddStringToObject(root, "image", img_buf);
    cJSON_AddNumberToObject(root, "rc", rc);
    char *out = cJSON_PrintUnformatted(root);
    int http_code = (rc == 0 || rc == 1) ? 200 : (rc == 3 ? 504 : 500);
    const char *http_msg = (rc == 0) ? "OK"
                         : (rc == 1) ? "OK"
                         : (rc == 3) ? "GATEWAY TIMEOUT" : "INTERNAL";
    send_resp(cfd, http_code, http_msg, "application/json",
              out ? out : "", out ? strlen(out) : 0);
    free(out);
    cJSON_Delete(root);
}

/* POST /api/reload — graceful self-shutdown.  In a container with
 * restart-policy unless-stopped (or systemd-managed standalone), the
 * supervisor brings up a fresh bmcd that picks up the new sources.json
 * on startup.
 *
 * HA-Supervisor-Add-On Spezialfall: HA stoppt nicht automatisch das
 * Container-Restart bei Exit-Code 0 (sieht's als "intentional stop").
 * Damit run.sh den Unterschied "selbst-getriggertes Reload" vs
 * "externer Container-Shutdown" sieht, schreiben wir einen Marker.
 * run.sh's wait-n-Loop prüft den Marker und re-execed sich beim Reload
 * statt mit exit 0 zu enden.
 *
 * Implementation: send the response, drop marker, SIGTERM self. */
static void handle_reload(int fd)
{
    const char *body =
        "{\"status\":\"reloading\","
         "\"note\":\"bmcd is exiting; container init re-execs to apply new config\"}";
    send_json(fd, 202, "ACCEPTED", body, strlen(body));
    int mf = open("/var/run/bmcd-reload-requested",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mf >= 0) {
        (void)write(mf, "reload\n", 7);
        close(mf);
    }
    fprintf(stderr, "API: /api/reload — marker dropped, sending SIGTERM to self\n");
    kill(getpid(), SIGTERM);
}

/* ─── frame log tail (existing) ──────────────────────────────────────── */

static void handle_log_tail(int fd, int n_req)
{
    if (n_req <= 0)        n_req = 50;
    if (n_req > RING_SIZE) n_req = RING_SIZE;

    /* alloc a generous text buffer */
    size_t cap = (size_t)n_req * 280 + 512;
    char *out = malloc(cap);
    if (!out) { send_text(fd, 500, "INTERNAL", "oom"); return; }
    size_t len = 0;

    pthread_mutex_lock(&g.ring_lock);
    unsigned avail = g.ring_total < RING_SIZE ? g.ring_total : RING_SIZE;
    unsigned start = (g.ring_pos + RING_SIZE - (unsigned)n_req) % RING_SIZE;
    unsigned want  = (unsigned)n_req < avail ? (unsigned)n_req : avail;

    for (unsigned i = 0; i < want && len + 256 < cap; ++i) {
        struct ring_entry *e = &g.ring[(start + i) % RING_SIZE];
        if (e->channel[0] == 0) continue;
        len += (size_t)snprintf(out + len, cap - len,
            "%llu.%06llu %-7s dst=0x%02x cnt=%3u plen=%2u cmd=0x%02x data=",
            (unsigned long long)(e->ts_us / 1000000u),
            (unsigned long long)(e->ts_us % 1000000u),
            e->channel, e->dst, e->cnt, e->plen, e->cmd);
        for (size_t j = 0; j < e->dlen && len + 4 < cap; ++j)
            len += (size_t)snprintf(out + len, cap - len, "%02x", e->data[j]);
        if (len + 2 < cap) { out[len++] = '\n'; out[len] = 0; }
    }
    pthread_mutex_unlock(&g.ring_lock);

    if (len == 0) {
        const char *msg = "(no frames captured yet)\n";
        send_text(fd, 200, "OK", msg);
    } else {
        send_resp(fd, 200, "OK", "text/plain; charset=utf-8", out, len);
    }
    free(out);
}

/* ─── HTTP request parsing ────────────────────────────────────────────── */

struct req {
    char  method[8];
    char  path[256];
    char  query[256];
    char  ingress_path[256];   /* X-Ingress-Path: für <base href> in WebUI */
    int   content_length;
    char  body[8192];
    size_t body_len;
};

static int read_request(int fd, struct req *r)
{
    char  hdr[4096];
    size_t n = 0;
    while (n + 1 < sizeof(hdr)) {
        ssize_t k = read(fd, hdr + n, sizeof(hdr) - 1 - n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) break;
        n += (size_t)k;
        hdr[n] = 0;
        if (strstr(hdr, "\r\n\r\n")) break;
    }
    if (n == 0) return -1;
    hdr[n] = 0;

    /* method + path + query */
    char *eol = strstr(hdr, "\r\n");
    if (!eol) return -1;
    *eol = 0;
    char tmp_path[300];
    if (sscanf(hdr, "%7s %255s", r->method, tmp_path) != 2) return -1;
    char *q = strchr(tmp_path, '?');
    if (q) {
        *q = 0;
        snprintf(r->query, sizeof(r->query), "%s", q + 1);
    } else r->query[0] = 0;
    snprintf(r->path, sizeof(r->path), "%s", tmp_path);

    /* Content-Length */
    r->content_length = 0;
    const char *cl = strcasestr(eol + 2, "Content-Length:");
    if (cl) r->content_length = atoi(cl + 15);

    /* X-Ingress-Path — HA-Supervisor setzt diesen Header bei Ingress-Reverse-
     * Proxy (z.B. "/api/hassio_ingress/abc123").  Wir injizieren ihn als
     * <base href> ins HTML, damit relative URLs im Browser korrekt auflösen.
     * Ohne Header (Direkt-Zugriff am Pi5) bleibt das Feld leer. */
    r->ingress_path[0] = 0;
    const char *ip = strcasestr(eol + 2, "X-Ingress-Path:");
    if (ip) {
        ip += 15;
        while (*ip == ' ' || *ip == '\t') ip++;
        size_t i = 0;
        while (*ip && *ip != '\r' && *ip != '\n'
               && i + 1 < sizeof(r->ingress_path)) {
            r->ingress_path[i++] = *ip++;
        }
        r->ingress_path[i] = 0;
    }

    /* body — what we already read after "\r\n\r\n", plus drain remainder */
    char *body_start = strstr(eol + 2, "\r\n\r\n");
    r->body_len = 0;
    if (body_start) {
        body_start += 4;
        size_t already = (size_t)((hdr + n) - body_start);
        if (already > sizeof(r->body) - 1) already = sizeof(r->body) - 1;
        memcpy(r->body, body_start, already);
        r->body_len = already;
    }
    while ((int)r->body_len < r->content_length &&
           r->body_len + 1 < sizeof(r->body)) {
        ssize_t k = read(fd, r->body + r->body_len,
                         sizeof(r->body) - 1 - r->body_len);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; break; }
        r->body_len += (size_t)k;
    }
    r->body[r->body_len] = 0;
    return 0;
}

static int qs_int(const char *qs, const char *key, int def)
{
    if (!qs || !*qs) return def;
    size_t kl = strlen(key);
    const char *p = qs;
    while (*p) {
        if (!strncmp(p, key, kl) && p[kl] == '=')
            return atoi(p + kl + 1);
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return def;
}

void api_handle_accept(void)
{
    if (g.listen_fd < 0) return;
    int cfd = accept(g.listen_fd, NULL, NULL);
    if (cfd < 0) return;

    /* keep client I/O blocking — easier */
    int fl = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, fl & ~O_NONBLOCK);

    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

    struct req r = {0};
    if (read_request(cfd, &r) < 0) { close(cfd); return; }

    if (!strcmp(r.method, "OPTIONS")) {
        send_resp(cfd, 204, "NO CONTENT", "text/plain", "", 0);
    } else if (!strcmp(r.method, "GET") &&
               (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html"))) {
        handle_root(cfd, r.ingress_path);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/health")) {
        handle_health(cfd);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/status")) {
        handle_status(cfd);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/config")) {
        handle_config_get(cfd);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/config")) {
        handle_config_post(cfd, r.body, r.body_len);
    } else if (!strcmp(r.method, "GET")  && !strcmp(r.path, "/api/sources")) {
        handle_sources_get(cfd);
    } else if (!strcmp(r.method, "PUT")  && !strcmp(r.path, "/api/sources")) {
        handle_sources_put(cfd, r.body);
    } else if (!strncmp(r.path, "/api/sources/", 13)) {
        const char *id = r.path + 13;
        if      (!strcmp(r.method, "POST"))   handle_sources_post_id(cfd, id, r.body);
        else if (!strcmp(r.method, "DELETE")) handle_sources_delete_id(cfd, id);
        else send_text(cfd, 405, "METHOD NOT ALLOWED", "use POST or DELETE");
    } else if (!strcmp(r.path, "/api/slots")) {
        if      (!strcmp(r.method, "GET")) handle_slots_get(cfd);
        else if (!strcmp(r.method, "PUT")) handle_slots_put(cfd, r.body);
        else send_text(cfd, 405, "METHOD NOT ALLOWED", "use GET or PUT");
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/discover")) {
        handle_discover(cfd);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/effective")) {
        handle_effective(cfd);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reload")) {
        handle_reload(cfd);
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/log/tail")) {
        handle_log_tail(cfd, qs_int(r.query, "n", 50));
    } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/firmware/inventory")) {
        handle_firmware_inventory(cfd);
    } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/firmware/flash")) {
        handle_firmware_flash(cfd, r.body);
    } else {
        send_text(cfd, 404, "NOT FOUND", "no such endpoint");
    }
    close(cfd);
}
