/*
 * Renesas Serial Communication Interface
 *
 * Copyright (c) 2018 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RENESAS_SCI_H
#define HW_CHAR_RENESAS_SCI_H

#include "chardev/char-fe.h"
#include "hw/sysbus.h"

#define TYPE_RENESAS_SCI "renesas-sci"
#define RSCI(obj) OBJECT_CHECK(RSCIState, (obj), TYPE_RENESAS_SCI)

enum {
    ERI = 0,
    RXI = 1,
    TXI = 2,
    TEI = 3,
    SCI_NR_IRQ = 4
};

typedef struct {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion memory;
    QEMUTimer timer;
    CharBackend chr;
    qemu_irq irq[SCI_NR_IRQ];

    uint16_t smr;
    uint16_t brr;
    uint16_t scr;
    uint8_t tdr;
    uint16_t ssr;
    uint8_t rdr;
    uint16_t scmr;
    uint16_t semr;

    uint16_t read_ssr;
    int64_t trtime;
    int64_t rx_next;
    uint64_t input_freq;
} RSCIState;

#endif
