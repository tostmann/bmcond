// SPDX-License-Identifier: GPL-2.0-or-later
/* USB-Transport für libusb-1.0 — direkter Zugriff auf:
 *   1) eQ-3 HmIP-RFUSB           (CP2102N + EFM32G220F64,  1B1F:C020)
 *   2) ELV/A.R. HB-RF-USB v1     (FT232RL + RPi-RF-MOD,    0403:6F70)
 *   3) ELV/A.R. HB-RF-USB-2 v2   (CP2102N + RPi-RF-MOD,    10C4:8C07/8D81/8D91/8E4A)
 *
 * Alle drei sehen aus libusb-Sicht gleich aus: Vendor-Class-Interface mit
 * je einem Bulk-IN/-OUT-Pair für Daten, plus Vendor-Control-Endpunkt 0
 * für Setup. Unterschied liegt im Vendor-Protocol (CP210x vs FTDI) und
 * in der GPIO-Strecke vom Chip zum Modul-RESET-Pin.
 *
 *   Concentrator-select-Loop ── socketpair[0] (t->fd) ── socketpair[1] ── 2 Worker-Threads ── libusb-bulk-EP ── Chip
 *
 * Der Concentrator kennt nur fd-basierte Transports. socketpair-Bridge
 * tarnt libusb-Async als char-device: rx_thread pollt Bulk-IN, tx_thread
 * liest aus socketpair und feuert Bulk-OUT.
 *
 * ── Probe-Tabelle ──
 *
 * VID/PID → {chip, reset_pin, reset_polarity, …}.  Liefert auto den passenden
 * Setup- + Reset-Code; CLI braucht nur "-U <vid>:<pid>" (keine Treiber-Wahl).
 *
 * ── Reset-Strategie ──
 *
 * Eq-3 HmIP-RFUSB: kein GPIO routet auf EFM-RESET; Reset nur via
 *   USB-power-cycle / EFM-WDT.  reset_pin = -1 → no-op.
 *
 * HB-RF-USB v1 (FT232RL): CBUS3 → RESET (active-low am Modul).
 *   FTDI-Bitbang via SET_BITMODE 0x0B, mode=0x20.
 *
 * HB-RF-USB-2 (CP2102N): GPIO.0 → NPN-Inverter (Q4) → HM_RST_INV → Modul.
 *   Wegen Inverter ist GPIO.0=high ⇔ Modul-Reset asserted (effektiv
 *   active-high aus Chip-Sicht).  WRITE_LATCH 0xFF:0x37E1 mit 2-byte
 *   payload [mask, value].
 *
 * ── LED-Pinout (nur dokumentiert, nicht implementiert) ──
 *
 * HB-RF-USB v1 (FT232RL CBUS-Mux):
 *   CBUS0 → B_LED   (FTDI-EEPROM-Funktion-belegt, theor. via bitbang treibbar)
 *   CBUS1 → G_LED
 *   CBUS2 → R_LED
 *   CBUS3 → RESET   ▶ unser Pin
 *   CBUS4 → PWREN   (auto-USB-Suspend-Gating; Bitbang-Mode betrifft CBUS0..3 nicht CBUS4 → Modul bleibt während Reset-Pulse versorgt)
 *
 * HB-RF-USB-2 (CP2102N GPIO.0..3):
 *   GPIO.0 → HM_RST  ▶ unser Pin (über NPN-Inverter)
 *   GPIO.1 → R_LED
 *   GPIO.2 → G_LED
 *   GPIO.3 → B_LED
 *
 * LED-Steuerung wäre trivial einbaubar (CP2102N: WRITE_LATCH mask=0x0E;
 * FTDI: bitbang CBUS0..2), aber LEDs zeigen Modul-Status / USB-Aktivität
 * an und sind kein RF-Funktionsteil — wir lassen die EEPROM-Default-
 * Funktion stehen.
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "transport_usb.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

/* ── Chip-Vendor-Request-Codes ───────────────────────────────────────── */

/* CP210x (eQ-3 HmIP-RFUSB + HB-RF-USB-2) */
#define CP210X_IFC_ENABLE       0x00
#define CP210X_SET_LINE_CTL     0x03
#define CP210X_SET_MHS          0x07
#define CP210X_PURGE            0x12
#define CP210X_SET_FLOW         0x13
#define CP210X_SET_BAUDRATE     0x1E
#define CP210X_VENDOR_SPECIFIC  0xFF
#define CP210X_WRITE_LATCH      0x37E1
#define CP210X_REQTYPE_HOST_TO_INTERFACE  0x41

/* FTDI FT232R (HB-RF-USB v1) */
#define FTDI_SIO_RESET          0x00
#define FTDI_SIO_MODEM_CTRL     0x01
#define FTDI_SIO_SET_FLOW_CTRL  0x02
#define FTDI_SIO_SET_BAUD_RATE  0x03
#define FTDI_SIO_SET_DATA       0x04
#define FTDI_SIO_SET_LATENCY    0x09
#define FTDI_SIO_SET_BITMODE    0x0B
#define FTDI_REQTYPE_HOST_TO_DEVICE  0x40

/* FTDI Bitbang-Modes */
#define FTDI_BITMODE_RESET      0x00  /* zurück auf EEPROM-Default-Funktion */
#define FTDI_BITMODE_CBUS       0x20

/* ── Bulk IO Tuning ─────────────────────────────────────────────────── */
#define USB_BULK_RX_BUF         512
#define USB_BULK_RX_TIMEOUT_MS  200
#define USB_BULK_TX_TIMEOUT_MS  1000

/* ── Probe-Tabelle ──────────────────────────────────────────────────── */

enum chip_kind {
    CHIP_CP2102N = 0,
    CHIP_FTDI    = 1,
};

struct usb_radio_quirk {
    unsigned       vid;
    unsigned       pid;
    enum chip_kind chip;
    int            reset_pin;          /* -1 = kein Reset-GPIO; sonst Bit-Index am Chip */
    int            reset_active_high;  /* 0 = drive low to assert reset, 1 = drive high */
    int            reset_pulse_ms;
    const char    *kind_hint;
};

static const struct usb_radio_quirk usb_radio_quirks[] = {
    /* eQ-3 HmIP-RFUSB — CP2102N + EFM32G220F64. Kein Chip-GPIO routet auf
     * EFM-RESET; Reset nur via USB-Power-Cycle. Daher reset_pin=-1. */
    { 0x1b1f, 0xc020, CHIP_CP2102N, -1, 0,  0, "eQ-3 HmIP-RFUSB"     },

    /* HB-RF-USB v1 — FT232RL, CBUS3 → Modul-RESET (active-low am Modul). */
    { 0x0403, 0x6f70, CHIP_FTDI,     3, 0, 50, "HB-RF-USB"           },

    /* HB-RF-USB-2 — CP2102N, GPIO.0 → Q4-NPN-Inverter → HM_RST_INV → Modul.
     * Effektiv aus Chip-Sicht: GPIO.0=high asserts reset. */
    { 0x10c4, 0x8c07, CHIP_CP2102N,  0, 1, 50, "HB-RF-USB-2"         },
    { 0x10c4, 0x8d81, CHIP_CP2102N,  0, 1, 50, "HB-RF-USB-2 (A.R.)"  },
    { 0x10c4, 0x8d91, CHIP_CP2102N,  0, 1, 50, "HB-RF-USB-2 (A.R.)"  },
    { 0x10c4, 0x8e4a, CHIP_CP2102N,  0, 1, 50, "HB-RF-USB-2 (A.R.)"  },
};

static const struct usb_radio_quirk *quirk_lookup(unsigned vid, unsigned pid)
{
    for (size_t i = 0; i < sizeof(usb_radio_quirks)/sizeof(usb_radio_quirks[0]); ++i) {
        if (usb_radio_quirks[i].vid == vid && usb_radio_quirks[i].pid == pid)
            return &usb_radio_quirks[i];
    }
    return NULL;
}

/* ── Per-Instance State ─────────────────────────────────────────────── */

struct usb_priv {
    /* Identität */
    unsigned                       vid, pid;
    int                            baud;
    enum chip_kind                 chip;
    const struct usb_radio_quirk  *quirk;       /* darf NULL bei Unknown-VID/PID sein → CP2102N-Default */

    /* USB-State */
    libusb_context        *ctx;
    libusb_device_handle  *handle;
    uint8_t                ep_in;
    uint8_t                ep_out;
    uint16_t               ep_in_max_pkt;        /* für FTDI-Status-Byte-Stripping */
    int                    iface;
    int                    detached_kernel;

    /* Bridge-State */
    int                    sv_app;
    int                    sv_worker;
    pthread_t              rx_tid, tx_tid;
    int                    rx_running, tx_running;
    volatile int           stop_flag;
};

/* ── CP210x setup ───────────────────────────────────────────────────── */

static int cp210x_ctrl_out(libusb_device_handle *h, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            const uint8_t *data, uint16_t wLen)
{
    int r = libusb_control_transfer(h,
        CP210X_REQTYPE_HOST_TO_INTERFACE,
        bRequest, wValue, wIndex,
        (unsigned char *)data, wLen,
        2000);
    return (r < 0) ? -1 : 0;
}

static int cp210x_setup(struct usb_priv *p)
{
    if (cp210x_ctrl_out(p->handle, CP210X_IFC_ENABLE, 0x0001, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[CP210x]: IFC_ENABLE failed\n"); return -1;
    }
    if (cp210x_ctrl_out(p->handle, CP210X_SET_MHS, 0x0303, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[CP210x]: SET_MHS failed\n"); return -1;
    }
    if (cp210x_ctrl_out(p->handle, CP210X_SET_LINE_CTL, 0x0800, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[CP210x]: SET_LINE_CTL failed\n"); return -1;
    }
    static const uint8_t flow_no_fc[16] = {
        0x01, 0x00, 0x00, 0x00,   /* ulControlHandshake = 1 (DTR pin via SET_MHS) */
        0x40, 0x00, 0x00, 0x00,   /* ulFlowReplace      = 0x40 (RTS_ACTIVE) */
        0x80, 0x00, 0x00, 0x00,   /* ulXonLimit         = 0x80 */
        0x40, 0x00, 0x00, 0x00,   /* ulXoffLimit        = 0x40 */
    };
    if (cp210x_ctrl_out(p->handle, CP210X_SET_FLOW, 0, 0, flow_no_fc, sizeof(flow_no_fc)) < 0) {
        fprintf(stderr, "USB[CP210x]: SET_FLOW failed\n"); return -1;
    }
    uint32_t baud = (uint32_t)p->baud;
    uint8_t bd[4] = {
        (uint8_t)(baud), (uint8_t)(baud >> 8),
        (uint8_t)(baud >> 16), (uint8_t)(baud >> 24)
    };
    if (cp210x_ctrl_out(p->handle, CP210X_SET_BAUDRATE, 0, 0, bd, sizeof(bd)) < 0) {
        fprintf(stderr, "USB[CP210x]: SET_BAUDRATE failed\n"); return -1;
    }
    if (cp210x_ctrl_out(p->handle, CP210X_PURGE, 0x000F, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[CP210x]: PURGE failed\n"); return -1;
    }
    return 0;
}

/* CP2102N WRITE_LATCH: 2-byte payload [mask, value], pro Bit eines.
 * mask=Bit gesetzt ⇒ wird verändert; value=Bit gesetzt ⇒ neuer State high. */
static int cp210x_gpio_write(struct usb_priv *p, uint8_t mask, uint8_t value)
{
    uint8_t data[2] = { mask, value };
    int r = libusb_control_transfer(p->handle,
        CP210X_REQTYPE_HOST_TO_INTERFACE,
        CP210X_VENDOR_SPECIFIC, CP210X_WRITE_LATCH, p->iface,
        data, sizeof(data), 2000);
    return (r < 0) ? -1 : 0;
}

/* ── FTDI setup ─────────────────────────────────────────────────────── */

static int ftdi_ctrl_out(libusb_device_handle *h, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex,
                          const uint8_t *data, uint16_t wLen)
{
    int r = libusb_control_transfer(h,
        FTDI_REQTYPE_HOST_TO_DEVICE,
        bRequest, wValue, wIndex,
        (unsigned char *)data, wLen,
        2000);
    return (r < 0) ? -1 : 0;
}

/* FT232R baud-divisor (48 MHz / 16 / baud, sub-divisor in oberen 3 bits).
 * Für 115200 ist das Ergebnis sauber: 48M/16/115200 = 26.04 → 26 / sub=0. */
static int ftdi_baud_divisor_232r(int baud, uint16_t *value, uint16_t *index)
{
    static const uint8_t sub_to_encoded[8] = {
        /* sub-divisor 0/8..7/8 → 3-bit encoding */
        0, 3, 2, 4, 1, 5, 6, 7
    };
    if (baud <= 0) return -1;
    /* 16x oversampling */
    int divisor16 = (48000000 + 8 * baud) / (16 * baud);  /* gerundet */
    int integer = divisor16;
    int sub = 0;  /* nur exakte Treffer ohne sub-divisor; reicht für 115200/9600/… */

    /* Spezialfälle für 1/2-divisor (nur 1.5/0.5 erlaubt — sonst out-of-spec) */
    if (integer < 2) return -1;

    *value = (uint16_t)((integer & 0x3FFF) | (sub_to_encoded[sub] << 14));
    /* High-Index-Bit nur wenn sub-divisor das benötigt; für unsere
     * 115200 (sub=0) bleibt index=0. */
    *index = 0;
    return 0;
}

static int ftdi_setup(struct usb_priv *p)
{
    /* SIO_RESET wValue=0 → reset all (purge RX+TX, reset bitmode auf default) */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_RESET, 0x0000, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SIO_RESET failed\n"); return -1;
    }
    /* SET_BITMODE mode=0 mask=0 → ensure normal UART (kein Bitbang-State von vorigem Run) */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_SET_BITMODE,
                      (uint16_t)((FTDI_BITMODE_RESET << 8) | 0x00), 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SET_BITMODE(reset) failed\n"); return -1;
    }
    /* SIO_SET_BAUD_RATE */
    uint16_t bd_value, bd_index;
    if (ftdi_baud_divisor_232r(p->baud, &bd_value, &bd_index) < 0) {
        fprintf(stderr, "USB[FTDI]: unsupported baud %d\n", p->baud); return -1;
    }
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_SET_BAUD_RATE, bd_value, bd_index, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SET_BAUD_RATE failed\n"); return -1;
    }
    /* SIO_SET_DATA: 8 data bits, no parity, 1 stop bit, no break.
     *   wValue = (databits & 0x0F) | (parity & 0x07)<<8 | (stop & 0x03)<<11 | break<<14
     *          = 0x0008 für 8N1. */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_SET_DATA, 0x0008, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SET_DATA failed\n"); return -1;
    }
    /* SIO_SET_FLOW_CTRL: wIndex high byte = 0x00 (no flow ctl) */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_SET_FLOW_CTRL, 0x0000, 0x0000, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SET_FLOW_CTRL failed\n"); return -1;
    }
    /* SIO_SET_LATENCY 2 ms (default ist 16 ms — zu hoch für unser 100 ms Frame-Timing) */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_SET_LATENCY, 2, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: SET_LATENCY failed\n"); return -1;
    }
    /* SIO_MODEM_CTRL: DTR=1 RTS=1 (wValue: low=value, high=mask) */
    if (ftdi_ctrl_out(p->handle, FTDI_SIO_MODEM_CTRL, 0x0303, 0, NULL, 0) < 0) {
        fprintf(stderr, "USB[FTDI]: MODEM_CTRL failed\n"); return -1;
    }
    return 0;
}

/* FTDI CBUS-Bitbang-Pulse: pin auf LOW oder HIGH treiben, andere CBUS-Pins
 * bleiben Input (high-Z). Direction-Mask: nur unser Pin ist Output. */
static int ftdi_cbus_set(struct usb_priv *p, int pin, int high)
{
    if (pin < 0 || pin > 3) return -1;
    uint8_t dir   = (uint8_t)(1 << pin);
    uint8_t value = high ? dir : 0x00;
    /* mask high nibble = direction (CBUS3..CBUS0), low nibble = value */
    uint8_t mask  = (uint8_t)((dir << 4) | (value & 0x0F));
    uint16_t wValue = (uint16_t)((FTDI_BITMODE_CBUS << 8) | mask);
    return ftdi_ctrl_out(p->handle, FTDI_SIO_SET_BITMODE, wValue, 0, NULL, 0);
}

static int ftdi_cbus_release(struct usb_priv *p)
{
    /* Mode=0x00 mask=0x00 → bitbang ausschalten, EEPROM-Default-Funktion zurück
     * (CBUS3 wird wieder dem konfigurierten Modus überlassen, evtl. RESET#-Auto). */
    return ftdi_ctrl_out(p->handle, FTDI_SIO_SET_BITMODE,
                         (uint16_t)((FTDI_BITMODE_RESET << 8) | 0x00), 0, NULL, 0);
}

/* ── Reset-Pulse (chip-agnostic, dispatch über Quirk) ───────────────── */

static int usb_reset_pulse(struct usb_priv *p)
{
    if (!p->quirk || p->quirk->reset_pin < 0) return 0;  /* no-op (HmIP-RFUSB) */

    int pin    = p->quirk->reset_pin;
    int active = p->quirk->reset_active_high;   /* 1 ⇒ high asserts reset */
    int ms     = p->quirk->reset_pulse_ms;
    int rc     = 0;

    fprintf(stderr, "USB[%04x:%04x]: reset pulse (%s, pin=%d, %s, %d ms)\n",
            p->vid, p->pid, p->quirk->kind_hint, pin,
            active ? "active-high" : "active-low", ms);

    if (p->chip == CHIP_FTDI) {
        /* assert: drive pin to "active" level */
        if (ftdi_cbus_set(p, pin, active ? 1 : 0) < 0) {
            fprintf(stderr, "USB[FTDI]: CBUS assert failed\n"); rc = -1;
        }
        usleep(ms * 1000);
        /* release: drive pin to "idle" level (then exit bitbang) */
        if (ftdi_cbus_set(p, pin, active ? 0 : 1) < 0) {
            fprintf(stderr, "USB[FTDI]: CBUS release failed\n"); rc = -1;
        }
        usleep(5 * 1000);   /* kurz halten bevor wir bitbang verlassen */
        if (ftdi_cbus_release(p) < 0) {
            fprintf(stderr, "USB[FTDI]: CBUS release-mode failed\n"); rc = -1;
        }
    } else {
        /* CP2102N WRITE_LATCH: nur unser Bit anfassen, andere LEDs/GPIOs unverändert */
        uint8_t mask  = (uint8_t)(1 << pin);
        uint8_t v_assert  = active ? mask : 0x00;
        uint8_t v_release = active ? 0x00 : mask;
        if (cp210x_gpio_write(p, mask, v_assert) < 0) {
            fprintf(stderr, "USB[CP2102N]: GPIO assert failed\n"); rc = -1;
        }
        usleep(ms * 1000);
        if (cp210x_gpio_write(p, mask, v_release) < 0) {
            fprintf(stderr, "USB[CP2102N]: GPIO release failed\n"); rc = -1;
        }
    }
    /* Modul-Boot-Time abwarten (HM-MOD/RPi-RF-MOD: ~150 ms bis erste Frames) */
    usleep(200 * 1000);
    return rc;
}

/* ── Endpoint discovery ─────────────────────────────────────────────── */

static int find_bulk_endpoints(libusb_device *dev, int *iface_out,
                                uint8_t *ep_in_out, uint8_t *ep_out_out,
                                uint16_t *ep_in_max_pkt_out)
{
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return -1;
    int rc = -1;
    for (int i = 0; i < cfg->bNumInterfaces && rc < 0; ++i) {
        const struct libusb_interface *itf = &cfg->interface[i];
        if (itf->num_altsetting < 1) continue;
        const struct libusb_interface_descriptor *id = &itf->altsetting[0];
        uint8_t in = 0, out = 0;
        uint16_t in_max = 0;
        for (int e = 0; e < id->bNumEndpoints; ++e) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & 0x80) {
                if (!in) { in = ep->bEndpointAddress; in_max = ep->wMaxPacketSize; }
            } else {
                if (!out) out = ep->bEndpointAddress;
            }
        }
        if (in && out) {
            *iface_out         = id->bInterfaceNumber;
            *ep_in_out         = in;
            *ep_out_out        = out;
            *ep_in_max_pkt_out = in_max ? in_max : 64;
            rc = 0;
        }
    }
    libusb_free_config_descriptor(cfg);
    return rc;
}

/* ── Worker threads ─────────────────────────────────────────────────── */

/* FTDI fügt vor jedem USB-Bulk-Paket 2 Modem-Status-Bytes ein.  Bei 64-Byte-
 * Packet-Size sind das 64+64+…+rest pro libusb_bulk_transfer; je 64-Byte-
 * Block die ersten 2 Bytes droppen. */
static int ftdi_strip_status_bytes(uint8_t *buf, int got, int max_pkt)
{
    if (max_pkt <= 2) return 0;
    int dst = 0;
    for (int src = 0; src < got; src += max_pkt) {
        int chunk = (got - src < max_pkt) ? (got - src) : max_pkt;
        if (chunk <= 2) continue;
        memmove(buf + dst, buf + src + 2, chunk - 2);
        dst += chunk - 2;
    }
    return dst;
}

static void *rx_thread(void *arg)
{
    struct usb_priv *p = arg;
    uint8_t buf[USB_BULK_RX_BUF];
    while (!p->stop_flag) {
        int got = 0;
        int r = libusb_bulk_transfer(p->handle, p->ep_in,
                                     buf, sizeof(buf),
                                     &got, USB_BULK_RX_TIMEOUT_MS);
        if (p->stop_flag) break;
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_PIPE) {
            shutdown(p->sv_worker, SHUT_RDWR);
            break;
        }
        if (r < 0) { usleep(10 * 1000); continue; }

        if (got > 0 && p->chip == CHIP_FTDI) {
            got = ftdi_strip_status_bytes(buf, got, p->ep_in_max_pkt);
        }
        if (got > 0) {
            ssize_t off = 0;
            while (off < got && !p->stop_flag) {
                ssize_t w = write(p->sv_worker, buf + off, got - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    p->stop_flag = 1;
                    break;
                }
                off += w;
            }
        }
    }
    return NULL;
}

static void *tx_thread(void *arg)
{
    struct usb_priv *p = arg;
    uint8_t buf[1024];
    while (!p->stop_flag) {
        ssize_t n = read(p->sv_worker, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        int off = 0;
        while (off < n && !p->stop_flag) {
            int sent = 0;
            int r = libusb_bulk_transfer(p->handle, p->ep_out,
                                         buf + off, (int)(n - off),
                                         &sent, USB_BULK_TX_TIMEOUT_MS);
            if (r == 0 && sent > 0) { off += sent; continue; }
            if (r == LIBUSB_ERROR_TIMEOUT) continue;
            fprintf(stderr, "USB: bulk OUT failed: %s\n", libusb_error_name(r));
            p->stop_flag = 1;
            break;
        }
    }
    return NULL;
}

/* ── transport_ops ──────────────────────────────────────────────────── */

static int usb_open_op(struct transport *t)
{
    struct usb_priv *p = t->priv;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    int fl = fcntl(sv[0], F_GETFL, 0);
    if (fl >= 0) fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);

    if (libusb_init(&p->ctx) < 0) {
        close(sv[0]); close(sv[1]); errno = EIO; return -1;
    }
    p->handle = libusb_open_device_with_vid_pid(p->ctx,
                                                 (uint16_t)p->vid,
                                                 (uint16_t)p->pid);
    if (!p->handle) {
        fprintf(stderr, "USB: %04x:%04x not found / no permission\n", p->vid, p->pid);
        libusb_exit(p->ctx); p->ctx = NULL;
        close(sv[0]); close(sv[1]); errno = ENODEV; return -1;
    }

    libusb_device *dev = libusb_get_device(p->handle);
    if (find_bulk_endpoints(dev, &p->iface, &p->ep_in, &p->ep_out, &p->ep_in_max_pkt) < 0) {
        fprintf(stderr, "USB: no bulk IN/OUT pair on %04x:%04x\n", p->vid, p->pid);
        goto fail_close;
    }

    if (libusb_kernel_driver_active(p->handle, p->iface) == 1) {
        if (libusb_detach_kernel_driver(p->handle, p->iface) < 0) {
            fprintf(stderr, "USB: detach kernel driver failed\n");
            goto fail_close;
        }
        p->detached_kernel = 1;
    }
    if (libusb_claim_interface(p->handle, p->iface) < 0) {
        fprintf(stderr, "USB: claim_interface(%d) failed\n", p->iface);
        goto fail_close;
    }

    /* Chip-spezifisches Setup */
    int setup_rc;
    if (p->chip == CHIP_FTDI) setup_rc = ftdi_setup(p);
    else                       setup_rc = cp210x_setup(p);
    if (setup_rc < 0) {
        libusb_release_interface(p->handle, p->iface);
        goto fail_close;
    }

    /* Reset-Pulse falls Quirk vorhanden */
    if (usb_reset_pulse(p) < 0) {
        fprintf(stderr, "USB: reset pulse partial failure (continuing)\n");
        /* nicht-fatal — UART funktioniert auch ohne explizit gepulsten Modul-Reset */
    }

    p->sv_app    = sv[0];
    p->sv_worker = sv[1];
    p->stop_flag = 0;
    if (pthread_create(&p->rx_tid, NULL, rx_thread, p) != 0) {
        fprintf(stderr, "USB: rx_thread create failed\n");
        libusb_release_interface(p->handle, p->iface);
        goto fail_close;
    }
    p->rx_running = 1;
    if (pthread_create(&p->tx_tid, NULL, tx_thread, p) != 0) {
        fprintf(stderr, "USB: tx_thread create failed\n");
        p->stop_flag = 1;
        shutdown(p->sv_worker, SHUT_RDWR);
        pthread_join(p->rx_tid, NULL);
        p->rx_running = 0;
        libusb_release_interface(p->handle, p->iface);
        goto fail_close;
    }
    p->tx_running = 1;

    t->fd = p->sv_app;
    return 0;

fail_close:
    if (p->handle) {
        if (p->detached_kernel) libusb_attach_kernel_driver(p->handle, p->iface);
        libusb_close(p->handle);
        p->handle = NULL;
    }
    if (p->ctx) { libusb_exit(p->ctx); p->ctx = NULL; }
    close(sv[0]); close(sv[1]);
    p->detached_kernel = 0;
    errno = EIO;
    return -1;
}

static void usb_close_op(struct transport *t)
{
    struct usb_priv *p = t->priv;
    if (!p) return;

    p->stop_flag = 1;

    if (p->sv_worker >= 0) {
        shutdown(p->sv_worker, SHUT_RDWR);
        close(p->sv_worker);
        p->sv_worker = -1;
    }

    if (p->rx_running) { pthread_join(p->rx_tid, NULL); p->rx_running = 0; }
    if (p->tx_running) { pthread_join(p->tx_tid, NULL); p->tx_running = 0; }

    if (p->sv_app >= 0) {
        close(p->sv_app);
        p->sv_app = -1;
    }
    t->fd = -1;

    if (p->handle) {
        libusb_release_interface(p->handle, p->iface);
        if (p->detached_kernel) {
            libusb_attach_kernel_driver(p->handle, p->iface);
            p->detached_kernel = 0;
        }
        libusb_close(p->handle);
        p->handle = NULL;
    }
    if (p->ctx) {
        libusb_exit(p->ctx);
        p->ctx = NULL;
    }
}

static void usb_free_op(struct transport *t)
{
    if (!t) return;
    if (t->fd >= 0 || ((struct usb_priv *)t->priv)->handle) {
        usb_close_op(t);
    }
    free(t->priv);
    free(t);
}

static const struct transport_ops usb_ops = {
    .open  = usb_open_op,
    .close = usb_close_op,
    .free  = usb_free_op,
};

/* ── Constructors ───────────────────────────────────────────────────── */

struct transport *transport_usb_new(unsigned vid, unsigned pid, int baud)
{
    if (vid == 0 || pid == 0 || vid > 0xFFFF || pid > 0xFFFF) {
        errno = EINVAL; return NULL;
    }
    if (baud <= 0) baud = 115200;
    struct transport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    struct usb_priv *p = calloc(1, sizeof(*p));
    if (!p) { free(t); return NULL; }
    p->vid = vid;
    p->pid = pid;
    p->baud = baud;
    p->sv_app = -1;
    p->sv_worker = -1;
    p->quirk = quirk_lookup(vid, pid);
    p->chip = p->quirk ? p->quirk->chip : CHIP_CP2102N;  /* unknown VID/PID → CP210x-Default */
    t->priv = p;
    t->ops  = &usb_ops;
    t->fd   = -1;
    snprintf(t->target, sizeof(t->target), "%04x:%04x%s%s",
             vid, pid,
             p->quirk ? " " : "",
             p->quirk ? p->quirk->kind_hint : "");
    snprintf(t->label,  sizeof(t->label),  "usb");
    return t;
}

struct transport *transport_usb_new_str(const char *vid_pid, int baud)
{
    if (!vid_pid) { errno = EINVAL; return NULL; }
    while (*vid_pid == ' ' || *vid_pid == '\t') ++vid_pid;
    unsigned vid = 0, pid = 0;
    char extra = 0;
    if (sscanf(vid_pid, "%x:%x%c", &vid, &pid, &extra) != 2) {
        errno = EINVAL; return NULL;
    }
    return transport_usb_new(vid, pid, baud);
}

/* ─── Discovery ──────────────────────────────────────────────────────── */

int transport_usb_discover(struct usb_discovery_hit *out, int max)
{
    if (!out || max <= 0) { errno = EINVAL; return -1; }

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { errno = EIO; return -1; }

    libusb_device **list = NULL;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0) { libusb_exit(ctx); errno = EIO; return -1; }

    int n_hits = 0;
    for (ssize_t i = 0; i < cnt && n_hits < max; ++i) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) < 0) continue;
        const struct usb_radio_quirk *q = quirk_lookup(desc.idVendor, desc.idProduct);
        if (!q) continue;

        struct usb_discovery_hit *h = &out[n_hits];
        memset(h, 0, sizeof(*h));
        h->vid = desc.idVendor;
        h->pid = desc.idProduct;
        h->kind_hint = q->kind_hint;

        /* bus-port-Pfad — stable solange Topology unverändert. */
        uint8_t ports[8];
        int np = libusb_get_port_numbers(list[i], ports, sizeof(ports));
        int len = snprintf(h->bus_port, sizeof(h->bus_port), "%u",
                           libusb_get_bus_number(list[i]));
        for (int k = 0; k < np && len < (int)sizeof(h->bus_port) - 4; ++k)
            len += snprintf(h->bus_port + len, sizeof(h->bus_port) - (size_t)len,
                            "%c%u", k == 0 ? '-' : '.', ports[k]);

        /* iSerial — open read-only, kein Detach/Claim.  Permission-Fehler
         * (EACCES) tolerieren, einfach iserial leer lassen. */
        if (desc.iSerialNumber) {
            libusb_device_handle *dh = NULL;
            if (libusb_open(list[i], &dh) == 0 && dh) {
                unsigned char buf[64];
                int r = libusb_get_string_descriptor_ascii(
                    dh, desc.iSerialNumber, buf, sizeof(buf));
                if (r > 0) {
                    if ((size_t)r >= sizeof(h->iserial)) r = sizeof(h->iserial) - 1;
                    memcpy(h->iserial, buf, (size_t)r);
                    h->iserial[r] = 0;
                }
                libusb_close(dh);
            }
        }
        n_hits++;
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return n_hits;
}
