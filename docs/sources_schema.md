# bmcond.sources.json — Schema Specification

`bmcond.sources.json` is the persistence layer for **which radio sources
exist** and **which source serves which radio function**.  It is the
single source of truth that feeds confgen.c's `rfd.conf` /
`InterfacesList.xml` / `hm_mode` generation, drives the runtime backend
wiring, and is what the WebUI reads/writes through the JSON-API on
`:9126`.

This document is the contract between the API, the persistence layer,
and the WebUI.  Schema changes require a `schema_version` bump.

## File location

| Deployment | Path | Override |
|---|---|---|
| HA Add-On / docker bundle (headlessCCU) | `/data/etc-config/bmcond.sources.json` | `BMCOND_SOURCES_JSON` env |
| Bare-metal host (`bmcond.service`)      | `/etc/bmcond/sources.json`             | `BMCOND_SOURCES_JSON` env |
| Source tree (testing/CI)                | `/tmp/bmcond.sources.json`             | `BMCOND_SOURCES_JSON` env |

The path resolves at startup; bmcond does NOT search.  If the file does
not exist, bmcond looks for legacy `${BMCOND_CFG_PATH}` (Shell-source
format) and migrates it on first write (see *Migration*).

## Top-level structure

```json
{
  "schema_version": 1,
  "sources": [ … ],
  "slots":   { … }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `schema_version` | int | yes | Currently `1`.  bmcond refuses to **write** files with a higher version than it knows; **reads** raise a warning but continue read-only. |
| `sources`        | array of Source | yes | Persistent inventory.  May be empty. |
| `slots`          | object | yes | Function-to-source assignment.  Keys must be from the known set; unknown keys logged as warning, ignored. |

## Source object

```json
{
  "id":           "rfusb-eq3-c020",
  "transport":    "usb=1b1f:c020",
  "label":        "HmIP-RFUSB",
  "capabilities": ["bidcos", "hmip"],
  "persistent":   true,
  "discovered_via": "libusb",
  "notes":        ""
}
```

| Field | Type | Required | Validation | Purpose |
|---|---|---|---|---|
| `id` | string | yes | `^[a-z0-9_-]{1,32}$`, unique within `sources[]` | Stable handle.  Auto-derived for discovered sources, free for custom. |
| `transport` | string | yes | parsable by existing transport parser | Form-string: `usb=VID:PID`, `rfusb=/dev/ttyXXX`, `tcp=host:port`, `udp=host:port`, `mock` |
| `label` | string | no | ≤64 chars | Display name in UI; defaults to `id` |
| `capabilities` | array of string | yes | non-empty subset of `["bidcos","hmip"]` | What this source can serve |
| `persistent` | bool | no | default `false` | `true` = survives discovery runs (custom-defined source).  `false` = ephemeral, only in memory after discovery (auto-detected stick). |
| `discovered_via` | string | no | `libusb` \| `mdns` \| `manual` | Informational; UI may color-code |
| `notes` | string | no | ≤256 chars | Free-form user annotation |

### Capability rules

- `bidcos` is set for any DualCoPro radio (HmIP-RFUSB, HM-MOD-RPI-PCB,
  RPi-RF-MOD via HB-RF-USB), HMUARTLGW classic, CULFW32, HM-LGW2.
- `hmip` is set ONLY for DualCoPro radios with paired HmIP firmware
  state.  CULFW32 and pure BidCoS gateways do NOT get `hmip` capability
  (no AES-CCM-128 co-processor).
- Custom sources may declare any capability subset; bmcond does not
  cross-check against the transport string.  Misconfiguration shows up
  at runtime as a non-functional slot, not as a save error.

### `id` derivation for auto-discovered sources

| Source class | ID pattern | Example | Stability |
|---|---|---|---|
| USB (libusb) | `usb-<vid>-<pid>` | `usb-1b1f-c020` | Stable across re-plug.  Multi-stick of same VID:PID gets `-a`/`-b` suffix in plug order (caveat: not stable across reboot if udev plug order changes — see *Open issues*). |
| mDNS-discovered TCP | `mdns-<sanitized-hostname>` | `mdns-cul32-hm` | Stable as long as the hostname is stable. |
| Manual / custom | user-chosen | `cul32-roof` | User's responsibility. |

## Slots object

```json
{
  "bidcos": "rfusb-eq3-c020",
  "hmip":   "rfusb-eq3-c020"
}
```

Known slot keys (forward-compatible — unknown keys are tolerated):

| Slot | Type | Cardinality | Behavior when null |
|---|---|---|---|
| `bidcos` | string (source id) or `null` | exclusive (max 1 source) | rfd starts but BidCoS-listener idle; no BidCoS pairing possible |
| `hmip`   | string (source id) or `null` | exclusive (max 1 source) | HMIPServer.jar does **not** start; InterfacesList.xml omits HmIP slot; `/dev/mmd_hmip` not created |
| `bidcos_lgw2_out` | object — *reserved, not yet implemented* | n/a | Future: per-BidCoS-source LGW2-Output (TCP-server emulating HM-LGW2).  See `lgw2_emu_in_bmcond` memory. |

### Slot validation

On `PUT /api/slots` (or atomic save), bmcond validates:

1. Slot value (if non-null) MUST reference an existing `id` in `sources[]`.
2. The referenced source MUST have the slot's function in its
   `capabilities` array.

Violations → `400 Bad Request` with the offending slot in the response body.

### Slot consistency on source delete

`DELETE /api/sources/<id>` succeeds even if `<id>` is currently slotted.
Response includes the auto-cleared slots:

```json
{ "deleted": "cul32-roof", "cleared_slots": ["bidcos"] }
```

Rationale: fail-safe over fail-loud — a UI that deleted a source
shouldn't have to re-PUT the slots first.  The backend takes care of
the consistency.

## Lifecycle: discovery vs persistence vs effective state

There are **three distinct state layers** that the API surfaces.  They
are **not auto-merged** — the user drives the transition between them.

### Schema state — `bmcond.sources.json`

Everything in this file.  Survives reboot.  Only contains:
- Sources the user has explicitly accepted (`persistent: true`).
- USB-discovered sticks with `persistent: false` are kept in memory by
  the running bmcond but NOT written to the file (re-discovered on
  every boot via libusb-enumerate).
- The current `slots` assignment.

### Discovery pool — ephemeral, user-curated

Result of `GET /api/discover`:
- libusb-enumerate against `usb_radio_quirks[]` from `transport_usb.c`
- mDNS browse for the well-known service types (see *mDNS Service
  Discovery Table* below).
- Returns `Source` candidate objects with `persistent: false` and a
  `discovered_via` tag.

**Discovery is user-triggered**, not automatic on boot.  The result is
a *pool of candidates*, not a list that gets merged into `sources[]`.
The user reviews the pool, optionally edits label/capabilities, then
imports a candidate via `POST /api/sources/<id>` to make it a permanent
schema entry.  Candidates the user does not import simply vanish on the
next discover run.

USB-stick discovery is the one exception — sticks plugged into the host
are auto-listed in the running bmcond's source-set so the user does not
have to "import" obvious local hardware.  But these still need a slot
assignment via `PUT /api/slots` to actually be used.

### Effective state — `GET /api/effective`

What bmcond is actually running right now: which backends are up, which
PTYs are open, frame counters per backend, **and which capabilities have
been *verified* by live probe** (vs. merely claimed via service-type
default).  The slot-to-source mapping in `effective` may diverge from
the slot in schema state when a slotted source was unplugged at runtime.

### Capability verification — claimed vs. observed

The `capabilities` field on a Source object is a **claim**: derived
from service-type defaults (mDNS table below), TXT-record overrides
(future), or user declaration in custom-source dialogs.  It is NOT
proof that the source actually delivers those frame streams.

In particular, **most BidCoS-capable wire formats also frame
DualCoPro**, which *can* carry HmIP — but actually carrying HmIP
requires AES-CCM-128 capable firmware on the radio side.  CULFW32 over
`_hmuartlgw._tcp:2327` (Dual) and `_raw-uart._udp:3008` both wrap
DualCoPro frames, but the underlying CC1101 has no AES co-processor;
HmIP traffic via these paths is sniff-only at best, not pair-capable.

bmcond verifies capabilities at runtime, distinct from the schema-side
`capabilities` claim:

- **`bidcos` verified** iff the live `COMMON_IDENTIFY` exchange in
  `radio_dualcopro.c` returned an `*_App` tag.  BidCoS-RX is universal
  on every DualCoPro-App firmware, so the boot-probe response is
  authoritative.
- **`hmip` verified** iff `stats.rx_hmip > 0` since boot — i.e., at
  least one valid HmIP-frame has been demodulated by the radio.  This
  is **not** an active probe but a passive observation.  Reason: USB-
  Layer probes (`GET_ADAPTER_MIC`, garbage `SEND_PROTOCOL_FRAME`) yield
  identical responses on CC1101-only HM-MOD-RPI-PCB and full-stack
  HmIP-RFUSB — see `tools/hmip_probe.c` and the dual-stick test result
  in `docs/probe_results/2026-05-05_dual-stick.txt`.  The DualCoPro_App
  firmware initialises the HmIP stack at the USB layer regardless of
  whether the radio actually has CC1200/AES-CCM hardware.  The
  hardware differentiator only appears at the RF-TX layer (which
  requires a paired actor and AES keys to probe actively).

  Traffic-based verify works across all transport classes:  CC1101-only
  hardware physically cannot demodulate HmIP modulation, so its
  `rx_hmip` stays 0 forever; HmIP-capable hardware shows `rx_hmip > 0`
  as soon as a paired actor transmits.

The WebUI color-codes accordingly:
- claim ⊆ verified → green (works as advertised)
- claim ⊋ verified → amber with tooltip ("HmIP claimed, not yet
  observed — paired actor needs to transmit, or hardware lacks CC1200")
- never set claim from verified result implicitly; the user keeps
  authority over the persisted capability declaration.

### UI merge view

The WebUI shows three visually distinct lists:

1. **Saved sources** (from schema) — green/persistent
2. **Active hardware** (from effective) — connection-OK indicator
3. **Discovery pool** (from a recent `GET /api/discover`) — visible only
   after the user clicks the Discover button; candidates have an
   "import" button.

Lists 1 and 2 are typically rendered as a single unified row per source
(matched by `id`).  List 3 is rendered separately, as the import inbox.

## mDNS Service Discovery Table

bmcond browses the following service types (all with `.local` domain):

| Service-Type     | Default-Port | Wire-Format         | Default-cap (claim) | Notes |
|---|---|---|---|---|
| `_culfw._tcp`     | 2323         | culfw CLI (ASCII)   | none — host-marker  | Phase-2: a host advertising `_culfw` is also potentially a BidCoS gateway via the legacy `Ar`/`As`-command path on this same port.  Not used as a Source in v1; rendered as host-grouping hint. |
| `_hmuartlgw._tcp` | 2325         | HMUARTLGW classic   | `["bidcos"]`        | Instance-name `CULFW32-Legacy` when from CULFW32. |
| `_hmuartlgw._tcp` | 2327         | DualCoPro-wrapper   | `["bidcos"]`        | Instance-name `CULFW32-Dual` when from CULFW32.  HmIP claim only after live verify. |
| `_raw-uart._udp`  | 3008         | hb-rf-eth (Reinert) | `["bidcos"]`        | UDP.  Matches Reinert's `hb_rf_eth.ko` `HB_RF_ETH_PORT 3008`.  CULFW32 advertises this too (see `transport_hbrfeth_udp/src/hbrfeth_listener.cpp`).  HmIP requires AES-CCM-128 on the wrapper side. |

Ports above are **defaults** — the actual port comes from the mDNS SRV
record, which bmcond MUST honor.

Service-type → capability mapping is a **default fallback**.  If a TXT
record on the service carries explicit overrides, those win.  Resolution
order (first match):

1. **`caps=bidcos,hmip`** — explicit comma-list of advertised capabilities.
2. **`bidcos=…` / `hmip=…`** — comma-list of supported directions (`tx`,
   `rx`, `rx-sniff`, …).  bmcond claims a capability iff its value
   includes the `tx` token; rx-only / sniff doesn't qualify (the source
   can't accept setValue from rfd).
3. **service-type default** (table above).

| TXT key       | Value example         | Effect |
|---|---|---|
| `caps`        | `bidcos,hmip`         | overrides default-cap claim list |
| `bidcos`      | `tx,rx`               | claim `bidcos` iff `tx` present |
| `hmip`        | `rx-sniff` / `tx,rx`  | claim `hmip` iff `tx` present |
| `wire`        | `dualcopro` / `hbrfeth` | informational, for UI label |
| `model`       | `RPI-RF-MOD`          | informational, appended to label |
| `fw` / `fwver`| `4.4.18` / `1.5.328`  | informational, in `notes` |

**CULFW32 example** (live verified 2026-05-05):
```
=;eth0;IPv4;CULFW32-Legacy;_hmuartlgw._tcp;local;culfw32-8e9e5c.local;10.10.11.28;2325;
  "mode=legacy" "model=HM-MOD-RPI-PCB" "fw=1.4.1" "bidcos=tx,rx"
=;eth0;IPv4;CULFW32-Dual;_hmuartlgw._tcp;local;culfw32-8e9e5c.local;10.10.11.28;2327;
  "mode=dual" "model=RPI-RF-MOD" "fw=4.4.18" "bidcos=tx,rx" "hmip=rx-sniff"
```

`bidcos=tx,rx` claims `bidcos`; `hmip=rx-sniff` does NOT claim `hmip`
(no `tx` token).  Result: both CULFW32 endpoints get
`capabilities: ["bidcos"]` only.

### Manual TCP/UDP source entry

The user can always add a custom source via the UI dialog or
`POST /api/sources/<id>` directly:

```json
{
  "id": "lab-cul868-roof",
  "transport": "tcp=cul868-roof.local:2325",
  "label": "Legacy CUL868 (Dachboden)",
  "capabilities": ["bidcos"],
  "persistent": true,
  "discovered_via": "manual",
  "notes": "klassischer CUL mit a-culfw, BidCoS-only"
}
```

Manual entries do not require mDNS announcement on the target side and
work against any reachable host:port (e.g. ser2net bridges, legacy CULs,
remote piVCCU instances).

## API endpoint surface

| Method + Path | Reads/Writes | Body | Notes |
|---|---|---|---|
| `GET /api/sources` | reads schema | — | Returns `{ "sources": [...], "slots": {...} }` from sources.json |
| `PUT /api/sources` | writes schema | full `{ "sources": [...], "slots": {...} }` | **Atomic full-replace**.  Primary endpoint for the WebUI's Apply button. |
| `POST /api/sources/<id>` | writes schema | single Source object | Convenience: add/upsert one source.  Does not touch `slots`. |
| `DELETE /api/sources/<id>` | writes schema | — | Removes source; auto-clears any slot referencing it. |
| `GET /api/slots` | reads schema | — | Returns just `{"bidcos": ..., "hmip": ...}` |
| `PUT /api/slots` | writes schema | `{"bidcos": ..., "hmip": ...}` | Convenience: change slots without re-sending sources. |
| `GET /api/discover` | reads runtime | — | **User-triggered only.**  Returns a candidate pool; does NOT auto-merge into schema.  Idempotent.  May take 1-3s (mDNS browse). |
| `POST /api/reload` | runtime action | — | Tear down all backends, re-run confgen against current schema, bring backends back up.  Returns 202 + `{"status":"reloading"}`; clients should poll `GET /api/status` for completion. |
| `GET /api/effective` | reads runtime | — | Current live state.  Per backend includes: `connected: bool`, `verified_capabilities: [...]` (from live COMMON_IDENTIFY + HmIP-probe), frame counters.  WebUI uses this for status dots and the claimed-vs-verified capability tooltip. |

**Existing endpoints that don't change**: `GET /api/health`,
`GET /api/status`, `GET /api/log/tail`, `POST /api/config` (legacy
Shell-source path, still served for backward compat with debmatic add-on).

### Save vs Reload — UX distinction

`PUT /api/sources` writes the file but does NOT teardown backends.
Existing radios keep running with old config.  This is intentional:
- enables "save draft" / "edit then apply"
- avoids spurious teardowns on every UI keystroke

The WebUI's Apply button calls **both** in sequence:
1. `PUT /api/sources` (save schema)
2. `POST /api/reload` (apply to runtime)

A "Save without Reload" affordance is optional and can come later.

## Validation rule summary (consolidated)

| Rule | Code | Failure |
|---|---|---|
| `schema_version == 1` | 400 | unknown version requested by client |
| `id` matches `^[a-z0-9_-]{1,32}$` | 400 | bad id format |
| `id` unique in `sources[]` | 409 | conflicting id |
| `transport` parses successfully | 400 | malformed transport string |
| `capabilities` non-empty subset of `{bidcos,hmip}` | 400 | empty or unknown capability |
| Slot value references existing source id | 400 | dangling slot |
| Slot value's source has the slot's capability | 400 | capability mismatch |
| `notes` ≤ 256 chars, `label` ≤ 64 chars | 400 | overflow |

## Migration from legacy Shell-source format

On first run, if `bmcond.sources.json` does not exist but
`${BMCOND_CFG_PATH}` exists with `BMCOND_BIDCOS_TRANSPORT=…` etc.:

1. bmcond reads the legacy keys.
2. Constructs a synthetic `sources[]`:
   - one source per unique transport string
   - id pattern: `legacy-<sanitized-transport>`
   - capabilities derived from which BMCOND_*_TRANSPORT key referenced it
   - `persistent: true`, `discovered_via: "manual"`
3. Constructs `slots` to point at those synthetic sources.
4. Writes the new sources.json.
5. Leaves the legacy file in place (for downgrade safety).

Subsequent boots ignore the legacy file when sources.json exists.

## Examples

### Example 1 — single HmIP-RFUSB (default)

```json
{
  "schema_version": 1,
  "sources": [
    { "id": "usb-1b1f-c020",
      "transport": "usb=1b1f:c020",
      "label": "HmIP-RFUSB",
      "capabilities": ["bidcos", "hmip"],
      "persistent": false,
      "discovered_via": "libusb" }
  ],
  "slots": { "bidcos": "usb-1b1f-c020", "hmip": "usb-1b1f-c020" }
}
```

### Example 2 — split: CULFW32 for BidCoS range, HmIP-RFUSB for HmIP

```json
{
  "schema_version": 1,
  "sources": [
    { "id": "usb-1b1f-c020",
      "transport": "usb=1b1f:c020",
      "label": "HmIP-RFUSB",
      "capabilities": ["bidcos", "hmip"],
      "persistent": false,
      "discovered_via": "libusb" },
    { "id": "cul32-roof",
      "transport": "tcp=cul32-roof.local:2327",
      "label": "CULFW32 (Dachantenne)",
      "capabilities": ["bidcos"],
      "persistent": true,
      "discovered_via": "manual",
      "notes": "auf der Dachantenne, optimaler RX" }
  ],
  "slots": { "bidcos": "cul32-roof", "hmip": "usb-1b1f-c020" }
}
```

### Example 3 — HmIP-only (no BidCoS actors)

```json
{
  "schema_version": 1,
  "sources": [
    { "id": "usb-1b1f-c020",
      "transport": "usb=1b1f:c020",
      "label": "HmIP-RFUSB",
      "capabilities": ["bidcos", "hmip"],
      "persistent": false,
      "discovered_via": "libusb" }
  ],
  "slots": { "bidcos": null, "hmip": "usb-1b1f-c020" }
}
```

### Example 4 — BidCoS-only (HM-classic via CULFW32)

```json
{
  "schema_version": 1,
  "sources": [
    { "id": "cul32-lab",
      "transport": "tcp=cul32-hm.local:2327",
      "label": "CULFW32",
      "capabilities": ["bidcos"],
      "persistent": true,
      "discovered_via": "manual" }
  ],
  "slots": { "bidcos": "cul32-lab", "hmip": null }
}
```

confgen will write rfd.conf without HmIP, run.sh skips HMIPServer.jar.

## Open issues / decisions deferred

1. **Multi-stick same-VID:PID stable IDs**: USB bus/device path is not
   stable across reboot, but `usb-<vid>-<pid>-a/-b` plug-order suffixes
   aren't either.  Possible fix: probe stick serial number via descriptor
   (CP2102N + FTDI both expose iSerial) and use `usb-<vid>-<pid>-<serial>`.
   Defer until someone actually has two of the same stick.
2. **JSON parser library**: cJSON (~30 KB, glibc-only) vs parson (~15 KB,
   single-file).  Decide at start of API impl (Task #47).
3. **Config-file write atomicity**: write to `bmcond.sources.json.tmp`,
   `fsync`, `rename`.  Standard pattern; just call out here as a
   reminder for the impl.
4. **LGW2-out slot schema**: future work, not in v1.  See
   `lgw2_emu_in_bmcond` memory in the project memory directory for the
   sketched UI checkbox flow.
5. **Schema versioning policy**: backwards-compat additions (new optional
   fields) do NOT bump `schema_version`.  Removing or renaming fields,
   or changing semantics, does.  Bumps are rare and require migration
   code in bmcond.
