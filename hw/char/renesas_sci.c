/*
 * Renesas Serial Communication Interface
 *
 * Datasheet: RX62N Group, RX621 Group User's Manual: Hardware
 *            (Rev.1.40 R01UH0033EJ0140)
 *
 * Copyright (c) 2019 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/registerfields.h"
#include "hw/qdev-properties.h"
#include "hw/char/renesas_sci.h"
#include "migration/vmstate.h"

/* Common SCI register map */
REG8(SMR, 0)
  FIELD(SMR, CKS,  0, 2)
  FIELD(SMR, MP,   2, 1) /* RX62N SCI only */
  FIELD(SMR, STOP, 3, 1)
  FIELD(SMR, PM,   4, 1)
  FIELD(SMR, PE,   5, 1)
  FIELD(SMR, CHR,  6, 1)
  FIELD(SMR, CM,   7, 1)
REG8(BRR, 1)
REG8(SCR, 2)
  FIELD(SCR, CKE,  0, 2)
  FIELD(SCR, TEIE, 2, 1) /* RX62N SCI position */
  FIELD(SCR, MPIE, 3, 1)
  FIELD(SCR, RE,   4, 1)
  FIELD(SCR, TE,   5, 1)
  FIELD(SCR, RIE,  6, 1)
  FIELD(SCR, TIE,  7, 1)
REG8(TDR, 3)
REG8(SSR, 4) /* RX62N SCI only, including bits */
  FIELD(SSR, MPBT, 0, 1)
  FIELD(SSR, MPB,  1, 1)
  FIELD(SSR, TEND, 2, 1)
  FIELD(SSR, ERR,  3, 3)
    FIELD(SSR, PER,  3, 1)
    FIELD(SSR, FER,  4, 1)
    FIELD(SSR, ORER, 5, 1)
  FIELD(SSR, RDRF, 6, 1)
  FIELD(SSR, TDRE, 7, 1)
REG8(RDR, 5)

/* RX62N SCI register map */
REG8(SCMR, 6)
  FIELD(SCMR, SMIF, 0, 1)
  FIELD(SCMR, SINV, 2, 1)
  FIELD(SCMR, SDIR, 3, 1)
  FIELD(SCMR, BCP2, 7, 1)
REG8(SEMR, 7)
  FIELD(SEMR, ACS0, 0, 1)
  FIELD(SEMR, ABCS, 4, 1)

/* RCar3 SCIF register map */
REG16(SCSMR, 0)
REG16(SCSCR, 2)
  FIELD(SCSCR, TOIE, 2, 1)
  FIELD(SCSCR, REIE, 3, 1)
  FIELD(SCSCR, TEIE, 11, 1)
REG16(SCFSR, 4)
  FIELD(SCFSR, DR, 0, 1)
  FIELD(SCFSR, RDF, 1, 1)
  FIELD(SCFSR, PER, 2, 1)
  FIELD(SCFSR, FER, 3, 1)
  FIELD(SCFSR, BRK, 4, 1)
  FIELD(SCFSR, TDFE, 5, 1)
  FIELD(SCFSR, TEND, 6, 1)
  FIELD(SCFSR, ER, 7, 1)
  FIELD(SCFSR, FERC, 8, 4)
  FIELD(SCFSR, PERC, 12, 4)
REG8(SCFCR, 6)
  FIELD(SCFCR, LOOP, 0, 1)
  FIELD(SCFCR, RFRST, 1, 1)
  FIELD(SCFCR, TFRST, 2, 1)
  FIELD(SCFCR, MCE, 3, 1)
  FIELD(SCFCR, TTRG, 4, 2)
  FIELD(SCFCR, RTRG, 6, 2)
  FIELD(SCFCR, RSTRG, 8, 3)
REG16(SCFDR, 7)
  FIELD(SCFDR, R, 0, 5)
  FIELD(SCFDR, T, 8, 5)
REG16(SCSPTR, 8)
  FIELD(SCSPTR, SPB2DT, 0, 1)
  FIELD(SCSPTR, SPB2IO, 1, 1)
  FIELD(SCSPTR, SCKDT, 2, 1)
  FIELD(SCSPTR, SCKIO, 3, 1)
  FIELD(SCSPTR, CTSDT, 4, 1)
  FIELD(SCSPTR, CTSIO, 5, 1)
  FIELD(SCSPTR, RTSDT, 6, 1)
  FIELD(SCSPTR, RTSIO, 7, 1)
REG8(SCLSR, 9)
  FIELD(SCLSR, ORER, 0, 1)
  FIELD(SCLSR, TO, 2, 1)
REG16(DL, 12)
  FIELD(DL, DL, 0, 16)
REG16(CKS, 13)
  FIELD(CKS, XIN, 14, 1)
  FIELD(CKS, CKS, 15, 1)

static int can_receive(void *opaque)
{
    RSCIState *sci = RSCI(opaque);
    if (sci->rx_next > qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        return 0;
    } else {
        return FIELD_EX8(sci->scr, SCR, RE);
    }
}

static void receive(void *opaque, const uint8_t *buf, int size)
{
    RSCIState *sci = RSCI(opaque);
    sci->rx_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sci->trtime;
    // FIXME
    if (FIELD_EX16(sci->ssr, SCFSR, DR) || size > 1) {
        sci->ssr = FIELD_DP16(sci->ssr, SCLSR, ORER, 1);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 1);
        }
    } else {
        sci->rdr = buf[0];
        sci->ssr = FIELD_DP16(sci->ssr, SCFSR, DR, 1);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_irq_pulse(sci->irq[RXI]);
        }
    }
}

static void sci_send_byte(RSCIState *sci)
{
    if (qemu_chr_fe_backend_connected(&sci->chr)) {
        qemu_chr_fe_write_all(&sci->chr, &sci->tdr, 1);
    }
    timer_mod(&sci->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sci->trtime);
    sci->ssr = FIELD_DP8(sci->ssr, SSR, TEND, 0);
    sci->ssr = FIELD_DP8(sci->ssr, SSR, TDRE, 1);
    qemu_set_irq(sci->irq[TEI], 0);
    if (FIELD_EX8(sci->scr, SCR, TIE)) {
        qemu_irq_pulse(sci->irq[TXI]);
    }
}

static void sci_txend(void *opaque)
{
    RSCIState *sci = RSCI(opaque);

    if (!FIELD_EX8(sci->ssr, SSR, TDRE)) {
        sci_send_byte(sci);
    } else {
        sci->ssr = FIELD_DP8(sci->ssr, SSR, TEND, 1);
        if (FIELD_EX8(sci->scr, SCR, TEIE)) {
            qemu_set_irq(sci->irq[TEI], 1);
        }
    }
}

static void scif_send_byte(RSCIState *sci)
{
    if (qemu_chr_fe_backend_connected(&sci->chr)) {
        qemu_chr_fe_write_all(&sci->chr, &sci->tdr, 1);
    }
    timer_mod(&sci->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + sci->trtime);
    sci->ssr = FIELD_DP16(sci->ssr, SCFSR, TEND, 0);
    sci->ssr = FIELD_DP16(sci->ssr, SCFSR, TDFE, 1);
    qemu_set_irq(sci->irq[TEI], 0);
    if (FIELD_EX8(sci->scr, SCR, TIE)) {
        qemu_irq_pulse(sci->irq[TXI]);
    }
}

static void scif_txend(void *opaque)
{
    RSCIState *sci = RSCI(opaque);

    if (!FIELD_EX16(sci->ssr, SCFSR, TDFE)) {
        scif_send_byte(sci);
    } else {
        sci->ssr = FIELD_DP16(sci->ssr, SCFSR, TEND, 1);
        if (FIELD_EX16(sci->scr, SCSCR, TEIE)) {
            qemu_set_irq(sci->irq[TEI], 1);
        }
    }
}

static void update_trtime(RSCIState *sci)
{
    /* char per bits */
    sci->trtime = 8 - FIELD_EX8(sci->smr, SMR, CHR);
    sci->trtime += FIELD_EX8(sci->smr, SMR, PE);
    sci->trtime += FIELD_EX8(sci->smr, SMR, STOP) + 1;
    /* x bit transmit time (32 * divrate * brr) / base freq */
    sci->trtime *= 32 * sci->brr;
    sci->trtime *= 1 << (2 * FIELD_EX8(sci->smr, SMR, CKS));
    sci->trtime *= NANOSECONDS_PER_SECOND;
    sci->trtime /= sci->input_freq;
}

static bool sci_is_tr_enabled(RSCIState *sci)
{
    return FIELD_EX8(sci->scr, SCR, TE) || FIELD_EX8(sci->scr, SCR, RE);
}

static void sci_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    switch (offset) {
    case A_SMR:
        if (!sci_is_tr_enabled(sci)) {
            sci->smr = val;
            update_trtime(sci);
        }
        break;
    case A_BRR:
        if (!sci_is_tr_enabled(sci)) {
            sci->brr = val;
            update_trtime(sci);
        }
        break;
    case A_RDR:
        qemu_log_mask(LOG_GUEST_ERROR, "reneas_sci: RDR is read only.\n");
        break;
    case A_SCMR:
        sci->scmr = val; break;
    case A_SEMR: /* SEMR */
        sci->semr = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_sci: Register 0x%" HWADDR_PRIX " "
                                 "not implemented\n",
                      offset);
    }
}

static uint64_t sci_read(void *opaque, hwaddr offset, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    switch (offset) {
    case A_SMR:
        return sci->smr;
    case A_BRR:
        return sci->brr;
    case A_SCR:
        return sci->scr;
    case A_TDR:
        return sci->tdr;
    case A_SSR:
        sci->read_ssr = sci->ssr;
        return sci->ssr;
    case A_SCMR:
        return sci->scmr;
    case A_SEMR:
        return sci->semr;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_sci: Register 0x%" HWADDR_PRIX
                      " not implemented.\n", offset);
    }
    return UINT64_MAX;
}

static void scif_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    /* SCIF has 4-byte wide registers */
    offset >>= 2;

    switch (offset) {
    case A_SCR:
        sci->scr = val;
        if (FIELD_EX8(sci->scr, SCR, TE)) {
            sci->ssr = FIELD_DP8(sci->ssr, SCFSR, TDFE, 1);
            sci->ssr = FIELD_DP8(sci->ssr, SCFSR, TEND, 1);
            if (FIELD_EX8(sci->scr, SCR, TIE)) {
                qemu_irq_pulse(sci->irq[TXI]);
            }
        }
        if (!FIELD_EX8(sci->scr, SCR, TEIE)) {
            qemu_set_irq(sci->irq[TEI], 0);
        }
        if (!FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 0);
        }
        break;
    case A_TDR:
        sci->tdr = val;
        if (FIELD_EX8(sci->ssr, SCFSR, TEND)) {
            scif_send_byte(sci);
        } else {
            sci->ssr = FIELD_DP8(sci->ssr, SCFSR, TDFE, 0);
        }
        break;
    case A_SSR:
        if (FIELD_EX8(sci->read_ssr, SCFSR, ER) &&
            FIELD_EX8(sci->ssr, SCFSR, ER) == 0) {
            qemu_set_irq(sci->irq[ERI], 0);
        }
        break;
    default:
        sci_write(opaque, offset, val, size);
    }
}

static uint64_t scif_read(void *opaque, hwaddr offset, unsigned size)
{
    RSCIState *sci = RSCI(opaque);

    /* SCIF has 4-byte wide registers */
    offset >>= 2;

    switch (offset) {
    case A_RDR:
        sci->ssr = FIELD_DP16(sci->ssr, SCFSR, DR, 0);
        return sci->rdr;
    case A_SCFDR:
        return !!FIELD_EX16(sci->ssr, SCFSR, DR);
    default:
        return sci_read(opaque, offset, size);
    }
}

static const MemoryRegionOps sci_ops = {
    .write = sci_write,
    .read  = sci_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.max_access_size = 1,
    .valid.max_access_size = 1,
};

static const MemoryRegionOps scif_ops = {
    .write = scif_write,
    .read  = scif_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.max_access_size = 2,
    .valid.max_access_size = 2,
};

static void rsci_reset(DeviceState *dev)
{
    RSCIState *sci = RSCI(dev);
    sci->smr = sci->scr = 0x00;
    sci->brr = 0xff;
    sci->tdr = 0xff;
    sci->rdr = 0x00;
    sci->ssr = 0x84;
    sci->scmr = 0x00;
    sci->semr = 0x00;
    sci->rx_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void sci_event(void *opaque, QEMUChrEvent event)
{
    RSCIState *sci = RSCI(opaque);

    if (event == CHR_EVENT_BREAK) {
        sci->ssr = FIELD_DP8(sci->ssr, SCFSR, FER, 1);
        if (FIELD_EX8(sci->scr, SCR, RIE)) {
            qemu_set_irq(sci->irq[ERI], 1);
        }
    }
}

static void rsci_realize(DeviceState *dev, Error **errp)
{
    RSCIState *sci = RSCI(dev);

    if (sci->input_freq == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas_sci: input-freq property must be set.");
        return;
    }
    qemu_chr_fe_set_handlers(&sci->chr, can_receive, receive,
                             sci_event, NULL, sci, NULL, true);
}

static void rsci_common_init(SysBusDevice *d, RSCIState *sci)
{
    int i;

    sysbus_init_mmio(d, &sci->memory);

    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_init_irq(d, &sci->irq[i]);
    }
}

static void rsci_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    RSCIState *sci = RSCI(obj);

    memory_region_init_io(&sci->memory, OBJECT(sci), &sci_ops,
                              sci, "renesas-sci", 0x8);

    rsci_common_init(d, sci);
    timer_init_ns(&sci->timer, QEMU_CLOCK_VIRTUAL, sci_txend, sci);
}

static void rscif_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    RSCIState *sci = RSCI(obj);

    memory_region_init_io(&sci->memory, OBJECT(sci), &scif_ops,
                          sci, "renesas-scif", 0x34);

    rsci_common_init(d, sci);
    timer_init_ns(&sci->timer, QEMU_CLOCK_VIRTUAL, scif_txend, sci);
}

static const VMStateDescription vmstate_rsci = {
    .name = "renesas-sci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(trtime, RSCIState),
        VMSTATE_INT64(rx_next, RSCIState),
        VMSTATE_UINT16(smr, RSCIState),
        VMSTATE_UINT16(brr, RSCIState),
        VMSTATE_UINT16(scr, RSCIState),
        VMSTATE_UINT8(tdr, RSCIState),
        VMSTATE_UINT16(ssr, RSCIState),
        VMSTATE_UINT8(rdr, RSCIState),
        VMSTATE_UINT16(scmr, RSCIState),
        VMSTATE_UINT16(semr, RSCIState),
        VMSTATE_UINT16(read_ssr, RSCIState),
        VMSTATE_TIMER(timer, RSCIState),
        VMSTATE_END_OF_LIST()
    }
};

static Property rsci_properties[] = {
    DEFINE_PROP_UINT64("input-freq", RSCIState, input_freq, 0),
    DEFINE_PROP_CHR("chardev", RSCIState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void rsci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rsci_realize;
    dc->vmsd = &vmstate_rsci;
    dc->reset = rsci_reset;
    device_class_set_props(dc, rsci_properties);
}

static const TypeInfo renesas_sci_common_info = {
    .name = TYPE_RENESAS_SCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_init = rsci_class_init,
};

static const TypeInfo renesas_rx62n_sci_info = {
    .name = "renesas-rx-sci",
    .parent = TYPE_RENESAS_SCI,
    .instance_size = sizeof(RSCIState),
    .instance_init = rsci_init,
};

static const TypeInfo renesas_rcar3_scif_info = {
    .name = "renesas-rcar3-scif",
    .parent = TYPE_RENESAS_SCI,
    .instance_size = sizeof(RSCIState),
    .instance_init = rscif_init,
};

static void rsci_register_types(void)
{
    type_register_static(&renesas_sci_common_info);
    type_register_static(&renesas_rx62n_sci_info);
    type_register_static(&renesas_rcar3_scif_info);
}

type_init(rsci_register_types)
