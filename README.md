# bmcond — userspace radio transport for HomeMatic

`bmcond` (busmatic-concentrator) is a small userspace daemon that
bridges a HomeMatic / HomeMatic-IP radio module to `multimacd` over a
plain PTY.  It replaces the piVCCU kernel-driver chain
(`hb_rf_usb_2.ko`, `hb_rf_eth.ko`, `generic_raw_uart.ko`) with a
process running entirely in userspace.

```
   ┌──────────┐    ┌───────┐    ┌─────────────────┐
   │ multimacd│───>│  PTY  │<───│ bmcond          │───>  EFM32 / CC1101
   │ (eq-3)   │    │ /dev/ │    │ libusb / UDP /  │      (HmIP-RFUSB,
   └──────────┘    │ raw-  │    │ TCP / UART      │       HB-RF-USB(2),
                   │ uart  │    └─────────────────┘       HB-RF-ETH,
                   └───────┘                              RPI-RF-MOD,
                                                          CULFW32,
                                                          HM-MOD-RPI-PCB)
```

Pure C, single binary, links against glibc, libusb-1.0 and libcjson.
No kernel-module dependency.  No frame parsing — `bmcond` is byte-
transparent between the transport and the PTY.  The Mac-Layer
(LLMAC, DUTY-cycle, CSMA-CA, AES, retransmit) is owned by
`multimacd`.

## Why a separate transport process

The piVCCU/debmatic stack ships `hb_rf_usb_2` and `hb_rf_eth` as
DKMS kernel modules.  They expose `/dev/raw-uart` to userspace and
own the USB / Ethernet connection to the radio.  That works, but:

- DKMS rebuilds against every host-kernel update; a missed rebuild
  takes the radio offline.
- Only one connection per ETH-attached radio is possible — the kernel
  driver holds it exclusively, which makes diagnostics (sniffing,
  firmware updates) awkward.
- Inside containers the kernel driver still needs the right `.ko`
  files on the host plus `CAP_SYS_MODULE` to load them.

`bmcond` does all of this in userspace via libusb-direct, UDP or
TCP.  The result is the same `/dev/raw-uart` char device-shaped
interface that `multimacd` already knows how to open — only it's
a PTY symlink served by an unprivileged process.

## Supported radio modules

| Module | Transport | bmcond flag |
| --- | --- | --- |
| eQ-3 HmIP-RFUSB (`1b1f:c020`) | libusb-direct | `-U 1b1f:c020` |
| eQ-3 HmIP-RFUSB | CDC-UART via `cp210x` | `-t /dev/ttyUSB0` |
| HB-RF-USB v1 (FT232RL) + RPI-RF-MOD | libusb-direct, CBUS3-reset | `-U <vid:pid>` |
| HB-RF-USB-2 (CP2102N) + RPI-RF-MOD | libusb-direct, GPIO.0-reset | `-U <vid:pid>` |
| HM-MOD-RPI-PCB | Raspberry Pi UART | `-t /dev/ttyAMA0` |
| CULFW32 / HMUARTLGW-Dual | TCP | `-N host:port` |
| HB-RF-ETH (Alexander Reinert) | UDP (port 3008) | `-E host` |
| RFNetHM (HB-RF-ETH-compatible clone) | UDP (port 3008) | `-E host` |

USB-side per-chip quirks (CP2102N vs FTDI; reset-pin number;
polarity; pulse duration) live in `transport_usb.c::usb_radio_quirks[]`.

## Build

```sh
apt install build-essential libc6-dev libusb-1.0-0-dev libcjson-dev
make concentrator        # → bin/busmatic-concentrator
make tools               # → bin/copro_diag, bin/hmip_probe, bin/eth_soak
```

## Run

The canonical use-case — local HmIP-RFUSB stick at the USB port —
needs no transport flag at all; libusb-direct on `1b1f:c020` is the
default:

```sh
busmatic-concentrator --raw-uart=/dev/raw-uart -H FFD6B4 -S TEQ2822427 -F 4.4.18 -C -v
```

For other adapters, pass the appropriate flag explicitly:

```sh
# HM-MOD-RPI-PCB on Pi's onboard UART
busmatic-concentrator -t /dev/ttyAMA0 --raw-uart=/dev/raw-uart
# RFNetHM box on the LAN
busmatic-concentrator -E 192.168.1.50 --raw-uart=/dev/raw-uart
# CULFW32 advertised via mDNS
busmatic-concentrator -N culfw32.local:2327 --raw-uart=/dev/raw-uart
```

`-h` shows the full option list.  `--raw-uart=PATH` is mandatory:
it tells `bmcond` where to symlink the PTY slave so `multimacd`
can open it as its `Coprocessor Device Path`.

`-C` is optional; if set, `bmcond` writes
`/etc/config/rfd.conf` (so `rfd` finds the modulator) and
`/var/run/bmcd-config.json` (a status snapshot for the JSON API).
Both are non-destructive — existing files are backed up to
`*.bmcd-pre` first.

## JSON status API

`bmcond` listens on `:9126` and serves a small read-only/read-write
HTTP+JSON admin surface — useful for headlessCCU's WebUI sidebar,
ops scripts, or curl-based smoke tests:

| Endpoint | What |
| --- | --- |
| `GET /api/health` | liveness probe |
| `GET /api/status` | version, uptime, config-snapshot |
| `GET /api/effective` | merged runtime view (sources + slots + claim/verified caps) |
| `GET /api/discover` | USB enumerate + mDNS scan for advertised radios |
| `GET /api/sources` | `bmcond.sources.json` content |
| `PUT /api/sources` | replace whole sources doc |
| `GET /api/slots` | current `slots.{bidcos,hmip}` assignment |
| `POST /api/reload` | request a clean restart (writes marker, exits) |
| `GET /api/firmware/inventory` | scan `/firmware/<HW>/*.eq3` |
| `POST /api/firmware/flash` | flash an `.eq3` to the radio (sync) |
| `GET /api/log/tail` | recent log lines |

See `docs/sources_schema.md` for the persisted-source format.

## Tools

Standalone CLIs in `bin/` after `make tools`:

- `copro_diag` — radio inventory, FW probe, `.eq3` flash.  Speaks all
  the transports `bmcond` does (`copro_diag eth=host:3008 --flash file.eq3`).
- `hmip_probe` — HmIP-capability differential probe (which dst-layer
  + cmd-id pairs a given firmware accepts).
- `eth_soak` — liveness soak for `transport_eth` / `transport_rfnethm`.

## Where this is used

- [tostmann/headlessCCU](https://github.com/tostmann/headlessCCU) —
  bundles `bmcond` together with `multimacd`, `rfd`, `HMIPServer.jar`
  and a small JSON-RPC stub into a single Home-Assistant Add-On
  (or plain Docker container).
- Standalone, paired with debmatic on a Raspberry Pi: drop the piVCCU
  DKMS modules, point `multimacd.conf`'s `Coprocessor Device Path`
  at `bmcond`'s PTY symlink, done.

## Layout

```
concentrator.c        main(); arg-parsing; PTY+transport wiring
api.[ch]              JSON status API on :9126
sources.[ch]          bmcond.sources.json persistence (cJSON)
confgen.[ch]          -C: writes rfd.conf + /var/run/bmcd-config.json
backend.[ch]          Backend abstraction
hardware.[ch]         hw_identify(): /sys/bus/usb-serial probing
radio.[ch]            Radio module interface
radio_dualcopro.[ch]  Boot-to-App handshake (hw identification)
radio_id.h            Identity-snapshot type (HMID/serial/firmware/sgtin)
copro_query.[ch]      Co-CPU update protocol (SGTIN/FW probe, .eq3 flash)
eq3_image.[ch]        .eq3 firmware-image parser (per-chunk LEN+CRC)
frame.[ch]            DualCoPro wrapper codec (used by copro_query / eq3_image)
transport.h           Transport vtable
transport_uart.c      Local UART
transport_tcp.c       TCP connect (CULFW32 mDNS-discovery)
transport_usb.c       libusb-direct, per-VID:PID quirks
transport_eth.c       HB-RF-ETH UDP transport
transport_rfnethm.c   RFNetHM (HB-RF-ETH-compatible clone) UDP transport
tools/copro_diag.c    radio inventory + .eq3 flash CLI
tools/hmip_probe.c    HmIP capability-differential probe
tools/eth_soak.c      transport_eth liveness soak
docs/                 sources_schema.md + sources.example.json
```

## License

GPL-2.0-or-later — see `LICENSE`.

## Author

Dirk Tostmann ([@tostmann](https://github.com/tostmann)).
