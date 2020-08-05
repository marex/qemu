/*
 * Renesas RCar3 emulation
 *
 * Copyright (C) 2020 Marek Vasut <marek.vasut+renesas@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "cpu.h"
#include "hw/arm/renesas-rcar3.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"
#include "sysemu/kvm.h"
#include "sysemu/sysemu.h"
#include "kvm_arm.h"

#define GIC_NUM_SPI_INTR 480

#define ARM_PHYS_TIMER_PPI  30
#define ARM_VIRT_TIMER_PPI  27
#define ARM_HYP_TIMER_PPI   26
#define ARM_SEC_TIMER_PPI   29
#define GIC_MAINTENANCE_PPI 25

#define GIC_BASE_ADDR       0xf1000000
#define GIC_DIST_ADDR       0xf1010000
#define GIC_CPU_ADDR        0xf1020000
#define GIC_VIFACE_ADDR     0xf1040000
#define GIC_VCPU_ADDR       0xf1060000

static const uint64_t uart_addr[RENESAS_RCAR3_NUM_UARTS] = {
    0xe6e60000, 0xe6e68000, 0xe6e88000,
    0xe6c50000, 0xe6c40000, 0xe6f30000
};

static const int uart_intr[RENESAS_RCAR3_NUM_UARTS] = {
    152, 153, 164, 23, 16, 17
};

typedef struct RenesasRCar3GICRegion {
    int region_index;
    uint32_t address;
    uint32_t offset;
    bool virt;
} RenesasRCar3GICRegion;

static const RenesasRCar3GICRegion renesas_rcar3_gic_regions[] = {
    /* Distributor */
    {
        .region_index = 0,
        .address = GIC_DIST_ADDR,
        .offset = 0,
        .virt = false
    },

    /* CPU interface */
    {
        .region_index = 1,
        .address = GIC_CPU_ADDR,
        .offset = 0,
        .virt = false
    },
    {
        .region_index = 1,
        .address = GIC_CPU_ADDR + 0x10000,
        .offset = 0x1000,
        .virt = false
    },

    /* Virtual interface */
    {
        .region_index = 2,
        .address = GIC_VIFACE_ADDR,
        .offset = 0,
        .virt = true
    },

    /* Virtual CPU interface */
    {
        .region_index = 3,
        .address = GIC_VCPU_ADDR,
        .offset = 0,
        .virt = true
    },
    {
        .region_index = 3,
        .address = GIC_VCPU_ADDR + 0x10000,
        .offset = 0x1000,
        .virt = true
    },
};

static inline int arm_gic_ppi_index(int cpu_nr, int ppi_index)
{
    return GIC_NUM_SPI_INTR + cpu_nr * GIC_INTERNAL + ppi_index;
}

static void renesas_rcar3_init(Object *obj)
{
    RenesasRCar3State *s = RENESAS_RCAR3(obj);
    int i;

    object_initialize_child(obj, "a57-cluster", &s->a57_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->a57_cluster), "cluster-id", 0);

    for (i = 0; i < RENESAS_RCAR3_NUM_A57_CPUS; i++) {
        object_initialize_child(OBJECT(&s->a57_cluster), "a57-cpu[*]",
                                &s->a57_cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a57"));
    }

    object_initialize_child(obj, "a53-cluster", &s->a53_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->a53_cluster), "cluster-id", 1);

    for (i = 0; i < RENESAS_RCAR3_NUM_A53_CPUS; i++) {
        object_initialize_child(OBJECT(&s->a53_cluster), "a53-cpu[*]",
                                &s->a53_cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a53"));
    }

    object_initialize_child(obj, "gic", &s->gic, gic_class_name());

    for (i = 0; i < RENESAS_RCAR3_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i],
                                "renesas-rcar3-scif");
    }
}

static void renesas_rcar3_realize(DeviceState *dev, Error **errp)
{
    RenesasRCar3State *s = RENESAS_RCAR3(dev);
    MemoryRegion *system_memory = get_system_memory();
    int i;
    uint64_t ram_size;
    const char *boot_cpu = s->boot_cpu ? s->boot_cpu : "a57-cpu[0]";
    qemu_irq gic_spi[GIC_NUM_SPI_INTR];

    ram_size = memory_region_size(s->ddr_ram);

    if (ram_size > RENESAS_RCAR3_HIGH_RAM_MAX_SIZE) {
        error_report("ERROR: RAM size 0x%" PRIx64 " above max supported of 0x%llx",
                     ram_size, RENESAS_RCAR3_HIGH_RAM_MAX_SIZE);
        exit(1);
    }

    if (ram_size < RENESAS_RCAR3_HIGH_RAM_MIN_SIZE) {
        error_report("ERROR: RAM size 0x%" PRIx64 " is small for Salvator-X",
                     ram_size);
        exit(1);
    }

    /* Create the DDR Memory Regions in 64bit space */
    uint64_t bank_size = ram_size / RENESAS_RCAR3_HIGH_RAM_MAX_BANKS;
    for (i = 0; i < RENESAS_RCAR3_HIGH_RAM_MAX_BANKS; i++) {
        MemoryRegion *ram_bank = g_new(MemoryRegion, 1);
        char ram_name[17];
        snprintf(ram_name, sizeof(ram_name), "dbsc4.ram64bank%u", i);
        memory_region_init_alias(ram_bank, OBJECT(dev), ram_name, s->ddr_ram,
                                 i * bank_size, bank_size);
        memory_region_add_subregion(system_memory,
                                    RENESAS_RCAR3_HIGH_RAM_START(i),
                                    ram_bank);
        s->ram_bank[i] = ram_bank;
    }

    memory_region_add_subregion(system_memory,
                                RENESAS_RCAR3_LOW_RAM_START,
                                s->ddr_ram);

    /* Create the DBSC4 SystemRAM space */
    memory_region_init_ram(&s->ocm_ram, NULL, "dbsc4.systemram",
                           RENESAS_RCAR3_OCM_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                RENESAS_RCAR3_OCM_RAM_0_ADDRESS,
                                &s->ocm_ram);

    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", GIC_NUM_SPI_INTR + 32);
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 2);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu",
                         RENESAS_RCAR3_NUM_A57_CPUS +
                         RENESAS_RCAR3_NUM_A53_CPUS);
    qdev_prop_set_bit(DEVICE(&s->gic), "has-security-extensions", s->secure);
    qdev_prop_set_bit(DEVICE(&s->gic),
                      "has-virtualization-extensions", s->virt);

    qdev_realize(DEVICE(&s->a57_cluster), NULL, &error_fatal);

    /* Realize APUs before realizing the GIC. KVM requires this.  */
    for (i = 0; i < RENESAS_RCAR3_NUM_A57_CPUS; i++) {
        const char *name;

        object_property_set_int(OBJECT(&s->a57_cpu[i]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);

        name = object_get_canonical_path_component(OBJECT(&s->a57_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /* Secondary CPUs start in PSCI powered-down state */
            object_property_set_bool(OBJECT(&s->a57_cpu[i]),
                                     "start-powered-off", true, &error_abort);
        } else {
            s->boot_cpu_ptr = &s->a57_cpu[i];
        }

        object_property_set_bool(OBJECT(&s->a57_cpu[i]), "has_el3", s->secure,
                                 NULL);
        object_property_set_bool(OBJECT(&s->a57_cpu[i]), "has_el2", s->virt,
                                 NULL);
        object_property_set_int(OBJECT(&s->a57_cpu[i]), "reset-cbar",
                                GIC_BASE_ADDR, &error_abort);
        object_property_set_int(OBJECT(&s->a57_cpu[i]), "core-count",
                                RENESAS_RCAR3_NUM_A57_CPUS, &error_abort);
        if (!qdev_realize(DEVICE(&s->a57_cpu[i]), NULL, errp)) {
            return;
        }
    }

    qdev_realize(DEVICE(&s->a53_cluster), NULL, &error_fatal);

    /* Realize APUs before realizing the GIC. KVM requires this.  */
    for (i = 0; i < RENESAS_RCAR3_NUM_A53_CPUS; i++) {
        const char *name;

        object_property_set_int(OBJECT(&s->a53_cpu[i]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);

        name = object_get_canonical_path_component(OBJECT(&s->a53_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /* Secondary CPUs start in PSCI powered-down state */
            object_property_set_bool(OBJECT(&s->a53_cpu[i]),
                                     "start-powered-off", true, &error_abort);
        } else {
            s->boot_cpu_ptr = &s->a53_cpu[i];
        }

        object_property_set_bool(OBJECT(&s->a53_cpu[i]), "has_el3", s->secure,
                                 NULL);
        object_property_set_bool(OBJECT(&s->a53_cpu[i]), "has_el2", s->virt,
                                 NULL);
        object_property_set_int(OBJECT(&s->a53_cpu[i]), "reset-cbar",
                                GIC_BASE_ADDR, &error_abort);
        object_property_set_int(OBJECT(&s->a53_cpu[i]), "core-count",
                                RENESAS_RCAR3_NUM_A53_CPUS, &error_abort);
        if (!qdev_realize(DEVICE(&s->a53_cpu[i]), NULL, errp)) {
            return;
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }

    assert(ARRAY_SIZE(renesas_rcar3_gic_regions) == RENESAS_RCAR3_GIC_REGIONS);
    for (i = 0; i < RENESAS_RCAR3_GIC_REGIONS; i++) {
        SysBusDevice *gic = SYS_BUS_DEVICE(&s->gic);
        const RenesasRCar3GICRegion *r = &renesas_rcar3_gic_regions[i];
        MemoryRegion *mr;
        uint32_t addr = r->address;
        int j;

        if (r->virt && !s->virt) {
            continue;
        }

        mr = sysbus_mmio_get_region(gic, r->region_index);
        for (j = 0; j < RENESAS_RCAR3_GIC_ALIASES; j++) {
            MemoryRegion *alias = &s->gic_mr[i][j];

            memory_region_init_alias(alias, OBJECT(s), "rcar3-gic-alias", mr,
                                     r->offset, RENESAS_RCAR3_GIC_REGION_SIZE);
            memory_region_add_subregion(system_memory, addr, alias);

            addr += RENESAS_RCAR3_GIC_REGION_SIZE;
        }
    }

    int num_cpus = RENESAS_RCAR3_NUM_A57_CPUS + RENESAS_RCAR3_NUM_A53_CPUS;
    for (i = 0; i < RENESAS_RCAR3_NUM_A57_CPUS; i++) {
        qemu_irq irq;

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i,
                           qdev_get_gpio_in(DEVICE(&s->a57_cpu[i]),
                                            ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_cpus,
                           qdev_get_gpio_in(DEVICE(&s->a57_cpu[i]),
                                            ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_cpus * 2,
                           qdev_get_gpio_in(DEVICE(&s->a57_cpu[i]),
                                            ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_cpus * 3,
                           qdev_get_gpio_in(DEVICE(&s->a57_cpu[i]),
                                            ARM_CPU_VFIQ));
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_PHYS_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a57_cpu[i]), GTIMER_PHYS, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_VIRT_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a57_cpu[i]), GTIMER_VIRT, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_HYP_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a57_cpu[i]), GTIMER_HYP, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_SEC_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a57_cpu[i]), GTIMER_SEC, irq);

        if (s->virt) {
            irq = qdev_get_gpio_in(DEVICE(&s->gic),
                                   arm_gic_ppi_index(i, GIC_MAINTENANCE_PPI));
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_cpus * 4, irq);
        }
    }

    for (i = 0; i < RENESAS_RCAR3_NUM_A53_CPUS; i++) {
        qemu_irq irq;

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 4 + i,
                           qdev_get_gpio_in(DEVICE(&s->a53_cpu[i]),
                                            ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 4 + i + num_cpus,
                           qdev_get_gpio_in(DEVICE(&s->a53_cpu[i]),
                                            ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 4 + i + num_cpus * 2,
                           qdev_get_gpio_in(DEVICE(&s->a53_cpu[i]),
                                            ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 4 + i + num_cpus * 3,
                           qdev_get_gpio_in(DEVICE(&s->a53_cpu[i]),
                                            ARM_CPU_VFIQ));
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(4 + i, ARM_PHYS_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a53_cpu[i]), GTIMER_PHYS, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(4 + i, ARM_VIRT_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a53_cpu[i]), GTIMER_VIRT, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(4 + i, ARM_HYP_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a53_cpu[i]), GTIMER_HYP, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(4 + i, ARM_SEC_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->a53_cpu[i]), GTIMER_SEC, irq);

        if (s->virt) {
            irq = qdev_get_gpio_in(DEVICE(&s->gic),
                                   arm_gic_ppi_index(4 + i, GIC_MAINTENANCE_PPI));
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 4 + i + num_cpus * 4, irq);
        }
    }

    if (!s->boot_cpu_ptr) {
        error_setg(errp, "RCar3 Boot cpu %s not found", boot_cpu);
        return;
    }

    for (i = 0; i < GIC_NUM_SPI_INTR; i++) {
        gic_spi[i] = qdev_get_gpio_in(DEVICE(&s->gic), i);
    }

    for (i = 0; i < RENESAS_RCAR3_NUM_UARTS; i++) {
        qdev_prop_set_chr(DEVICE(&s->uart[i]), "chardev", serial_hd(i));
	qdev_prop_set_uint64(DEVICE(&s->uart[i]), "input-freq", 65000000 /* FIXME */);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart[i]), 0, uart_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart[i]), 0,
                           gic_spi[uart_intr[i]]);
    }
}

static Property renesas_rcar3_props[] = {
    DEFINE_PROP_STRING("boot-cpu", RenesasRCar3State, boot_cpu),
    DEFINE_PROP_BOOL("secure", RenesasRCar3State, secure, false),
    DEFINE_PROP_BOOL("virtualization", RenesasRCar3State, virt, false),
    DEFINE_PROP_LINK("ddr-ram", RenesasRCar3State, ddr_ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_END_OF_LIST()
};

static void renesas_rcar3_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, renesas_rcar3_props);
    dc->realize = renesas_rcar3_realize;
    /* Reason: Uses serial_hds in realize function, thus can't be used twice */
    dc->user_creatable = false;
}

static const TypeInfo renesas_rcar3_type_info = {
    .name = TYPE_RENESAS_RCAR3,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RenesasRCar3State),
    .instance_init = renesas_rcar3_init,
    .class_init = renesas_rcar3_class_init,
};

static void renesas_rcar3_register_types(void)
{
    type_register_static(&renesas_rcar3_type_info);
}

type_init(renesas_rcar3_register_types)
