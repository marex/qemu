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
#include "cpu.h"
#include "hw/arm/renesas-rcar3.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "sysemu/qtest.h"
#include "sysemu/device_tree.h"

typedef struct RenesasSALVATORX {
    MachineState parent_obj;

    RenesasRCar3State soc;

    bool secure;
    bool virt;

    struct arm_boot_info binfo;
} RenesasSALVATORX;

#define TYPE_SALVATORX_MACHINE   MACHINE_TYPE_NAME("renesas-rcar3-salvator-x")
#define SALVATORX_MACHINE(obj) \
    OBJECT_CHECK(RenesasSALVATORX, (obj), TYPE_SALVATORX_MACHINE)


static bool salvatorx_get_secure(Object *obj, Error **errp)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(obj);

    return s->secure;
}

static void salvatorx_set_secure(Object *obj, bool value, Error **errp)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(obj);

    s->secure = value;
}

static bool salvatorx_get_virt(Object *obj, Error **errp)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(obj);

    return s->virt;
}

static void salvatorx_set_virt(Object *obj, bool value, Error **errp)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(obj);

    s->virt = value;
}

static void renesas_salvatorx_init(MachineState *machine)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(machine);
    uint64_t ram_size = machine->ram_size;

    /* Create the memory region to pass to the SoC */
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

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RENESAS_RCAR3);

    object_property_set_link(OBJECT(&s->soc), "ddr-ram", OBJECT(machine->ram),
                             &error_abort);
    object_property_set_bool(OBJECT(&s->soc), "secure", s->secure,
                             &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "virtualization", s->virt,
                             &error_fatal);

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* TODO create and connect IDE devices for ide_drive_get() */

    uint64_t bank_size = ram_size / RENESAS_RCAR3_HIGH_RAM_MAX_BANKS;
    s->binfo.ram_size = (bank_size > RENESAS_RCAR3_LOW_RAM_MAX_SIZE) ?
                        RENESAS_RCAR3_LOW_RAM_MAX_SIZE : bank_size;
    s->binfo.loader_start = RENESAS_RCAR3_LOW_RAM_START + 0x10000000ull;
    s->binfo.dtb_start = RENESAS_RCAR3_LOW_RAM_START + 0x0c000000ull;
    arm_load_kernel(s->soc.boot_cpu_ptr, machine, &s->binfo);
}

static void renesas_salvatorx_machine_instance_init(Object *obj)
{
    RenesasSALVATORX *s = SALVATORX_MACHINE(obj);

    /* Default to secure mode being disabled */
    s->secure = false;
    object_property_add_bool(obj, "secure", salvatorx_get_secure,
                             salvatorx_set_secure);
    object_property_set_description(obj, "secure",
                                    "Set on/off to enable/disable the ARM "
                                    "Security Extensions (TrustZone)");

    /* Default to virt (EL2) being disabled */
    s->virt = false;
    object_property_add_bool(obj, "virtualization", salvatorx_get_virt,
                             salvatorx_set_virt);
    object_property_set_description(obj, "virtualization",
                                    "Set on/off to enable/disable emulating a "
                                    "guest CPU which implements the ARM "
                                    "Virtualization Extensions");
}

static void renesas_salvatorx_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Renesas R-Car3 Salvator-X board with 4xA57 and 4xA53";
    mc->init = renesas_salvatorx_init;
    mc->block_default_type = IF_IDE;
    mc->units_per_default_bus = 1;
    mc->ignore_memory_transaction_failures = true;
    mc->max_cpus = RENESAS_RCAR3_NUM_A57_CPUS + RENESAS_RCAR3_NUM_A53_CPUS;
    mc->default_cpus = RENESAS_RCAR3_NUM_A57_CPUS + RENESAS_RCAR3_NUM_A53_CPUS;
    mc->default_ram_id = "ddr-ram";
}

static const TypeInfo renesas_salvatorx_machine_init_typeinfo = {
    .name       = MACHINE_TYPE_NAME("renesas-rcar3-salvator-x"),
    .parent     = TYPE_MACHINE,
    .class_init = renesas_salvatorx_machine_class_init,
    .instance_init = renesas_salvatorx_machine_instance_init,
    .instance_size = sizeof(RenesasSALVATORX),
};

static void renesas_salvatorx_machine_init_register_types(void)
{
    type_register_static(&renesas_salvatorx_machine_init_typeinfo);
}

type_init(renesas_salvatorx_machine_init_register_types)
