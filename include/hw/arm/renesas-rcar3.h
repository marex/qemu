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

#ifndef RENESAS_RCAR3_H
#define RENESAS_RCAR3_H

#include "hw/arm/boot.h"
#include "hw/intc/arm_gic.h"
#include "hw/char/renesas_sci.h"
#include "hw/cpu/cluster.h"
#include "hw/qdev-properties.h"
#include "target/arm/cpu.h"

#define TYPE_RENESAS_RCAR3 "renesas,zynqmp"
#define RENESAS_RCAR3(obj) OBJECT_CHECK(RenesasRCar3State, (obj), \
                                       TYPE_RENESAS_RCAR3)

#define RENESAS_RCAR3_NUM_A57_CPUS 4
#define RENESAS_RCAR3_NUM_A53_CPUS 4
#define RENESAS_RCAR3_NUM_UARTS 6

#define RENESAS_RCAR3_OCM_RAM_0_ADDRESS 0xe6300000
#define RENESAS_RCAR3_OCM_RAM_SIZE 0x100000

#define RENESAS_RCAR3_GIC_REGIONS 6

/*
 * RCar3 maps the ARM GIC regions (GICC, GICD ...) at consecutive 64k offsets
 * and under-decodes the 64k region. This mirrors the 4k regions to every 4k
 * aligned address in the 64k region. To implement each GIC region needs a
 * number of memory region aliases.
 */

#define RENESAS_RCAR3_GIC_REGION_SIZE 0x1000
#define RENESAS_RCAR3_GIC_ALIASES     (0x10000 / RENESAS_RCAR3_GIC_REGION_SIZE)

#define RENESAS_RCAR3_LOW_RAM_START       0x40000000ull
#define RENESAS_RCAR3_LOW_RAM_MAX_SIZE    0x80000000ull

#define RENESAS_RCAR3_HIGH_RAM_START(n)   \
        (0x400000000ull + ((n) * 0x100000000ull))
#define RENESAS_RCAR3_HIGH_RAM_MAX_BANKS      4ull
#define RENESAS_RCAR3_HIGH_RAM_MIN_BANK_SIZE  (512ull * 1024 * 1024)
#define RENESAS_RCAR3_HIGH_RAM_MAX_BANK_SIZE  (2048ull * 1024 * 1024)
#define RENESAS_RCAR3_HIGH_RAM_MIN_SIZE   \
        (RENESAS_RCAR3_HIGH_RAM_MAX_BANKS * \
	 RENESAS_RCAR3_HIGH_RAM_MIN_BANK_SIZE)
#define RENESAS_RCAR3_HIGH_RAM_MAX_SIZE   \
        (RENESAS_RCAR3_HIGH_RAM_MAX_BANKS * \
	 RENESAS_RCAR3_HIGH_RAM_MAX_BANK_SIZE)

typedef struct RenesasRCar3State {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    CPUClusterState a57_cluster;
    CPUClusterState a53_cluster;
    ARMCPU a57_cpu[RENESAS_RCAR3_NUM_A57_CPUS];
    ARMCPU a53_cpu[RENESAS_RCAR3_NUM_A53_CPUS];
    GICState gic;
    MemoryRegion gic_mr[RENESAS_RCAR3_GIC_REGIONS][RENESAS_RCAR3_GIC_ALIASES];

    MemoryRegion ocm_ram;
    MemoryRegion *ddr_ram;
    MemoryRegion *ram_bank[RENESAS_RCAR3_HIGH_RAM_MAX_BANKS];
    MemoryRegion ddr_ram_low;

    RSCIState uart[RENESAS_RCAR3_NUM_UARTS];

    char *boot_cpu;
    ARMCPU *boot_cpu_ptr;

    /* Has the ARM Security extensions?  */
    bool secure;
    /* Has the ARM Virtualization extensions?  */
    bool virt;
}  RenesasRCar3State;

#endif
