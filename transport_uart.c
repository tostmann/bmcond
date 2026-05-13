// SPDX-License-Identifier: GPL-2.0-or-later
/* UART-Transport — implementiert struct transport_ops für lokale
 * Char-Devices wie /dev/ttyAMA0. */

#include "transport_uart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

struct uart_priv {
    char *device;
    int   baud;
};

static int uart_open_op(struct transport *t)
{
    struct uart_priv *p = t->priv;
    int fd = open(p->device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    cfmakeraw(&tio);
    cfsetspeed(&tio, p->baud);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    /* No HW flow control. The HmIP-RFUSB schematic wires CTS/RTS between
     * EFM and CP2102N, but empirically (TIOCMGET on /dev/ttyUSB0) the EFM
     * never asserts CTS — its FW does not drive that line. With CRTSCTS
     * enabled the kernel blocks every TX. /dev/ttyAMA0 also has no flow
     * control on the Pi GPIO header. /dev/raw-uart hides this internally. */
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    tcflush(fd, TCIOFLUSH);
    t->fd = fd;
    return 0;
}

static void uart_close_op(struct transport *t)
{
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

static void uart_free_op(struct transport *t)
{
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    struct uart_priv *p = t->priv;
    if (p) {
        free(p->device);
        free(p);
    }
    free(t);
}

static const struct transport_ops uart_ops = {
    .open  = uart_open_op,
    .close = uart_close_op,
    .free  = uart_free_op,
};

struct transport *transport_uart_new(const char *device, int baud)
{
    if (!device) { errno = EINVAL; return NULL; }
    struct transport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    struct uart_priv *p = calloc(1, sizeof(*p));
    if (!p) { free(t); return NULL; }
    p->device = strdup(device);
    if (!p->device) { free(p); free(t); return NULL; }
    p->baud = baud;
    t->priv = p;
    t->ops  = &uart_ops;
    t->fd   = -1;
    snprintf(t->target, sizeof(t->target), "%s", device);
    snprintf(t->label,  sizeof(t->label),  "uart");
    return t;
}

/* GPIO-Reset via Linux uAPI v2. Konzeptionell unabhängig vom transport
 * lifecycle — concentrator ruft das einmal vor transport_open() auf. */
static int reset_via_gpio_uapi(const char *chip, int line)
{
    int chip_fd = open(chip, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) return -1;

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = line;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof(req.consumer), "busmatic-concentrator");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 0;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        int e = errno; close(chip_fd); errno = e; return -1;
    }

    struct gpio_v2_line_values vals;
    vals.mask = 1;

    vals.bits = 0;
    if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) goto err;
    usleep(50 * 1000);

    vals.bits = 1;
    if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) goto err;
    usleep(50 * 1000);

    close(req.fd);
    close(chip_fd);
    usleep(150 * 1000);
    return 0;

err: ;
    int e = errno;
    close(req.fd);
    close(chip_fd);
    errno = e;
    return -1;
}

int transport_uart_reset_via_gpio(const char *chip, int line)
{
    if (chip && line >= 0) {
        return reset_via_gpio_uapi(chip, line);
    }
    return -1;
}
