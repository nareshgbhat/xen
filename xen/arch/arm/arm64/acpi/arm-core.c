/*
 *  ARM64 Specific Low-Level ACPI Boot Support
 *
 *  Copyright (C) 2014, Naresh Bhat <naresh.bhat@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <xen/init.h>
#include <xen/acpi.h>

#include <asm/acpi.h>

/*
 * We never plan to use RSDT on arm/arm64 as its deprecated in spec but this
 * variable is still required by the ACPI core
 */

int acpi_disabled;
EXPORT_SYMBOL(acpi_disabled);

int acpi_noirq;                         /* skip ACPI IRQ initialization */
int acpi_pci_disabled;          /* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

struct acpi_arm_root acpi_arm_rsdp_info;     /* info about RSDP from FDT */

/* GIC cpu interface base address on ARM */
static u64 acpi_lapic_addr __initdata;

static int __init acpi_parse_madt(struct acpi_table_header *table)
{
        struct acpi_table_madt *madt = NULL;

        madt = (struct acpi_table_madt *)table;
        if (!madt) {
                printk("Unable to map MADT\n");
                return 1;
        }

        if (madt->address) {
                acpi_lapic_addr = (u64) madt->address;

                printk("Local APIC address 0x%08x\n", madt->address);
        }

        return 0;
}

static void __init early_acpi_process_madt(void)
{
        /* should I introduce CONFIG_ARM_LOCAL_APIC like x86 does? */
        acpi_table_parse(ACPI_SIG_MADT, acpi_parse_madt);
}

/*
 * acpi_boot_table_init() and acpi_boot_init()
 *  called from setup_arch(), always.
 *      1. checksums all tables
 *      2. enumerates lapics
 *      3. enumerates io-apics
 *
 * acpi_table_init() is separated to allow reading SRAT without
 * other side effects.
 */
int __init acpi_boot_table_init(void)
{
        int error;

        /*
         * If acpi_disabled, bail out
         */
        if (acpi_disabled)
                return 1;

        /*
         * Initialize the ACPI boot-time table parser.
         */
        error = acpi_table_init();

        if (error) {
                disable_acpi();
                return error;
        }
        return 0;
}
int __init early_acpi_boot_init(void)
{
        /*
         * If acpi_disabled, bail out
         */
        if (acpi_disabled)
                return 1;

        /*
         * Process the Multiple APIC Description Table (MADT), if present
         */
        early_acpi_process_madt();

        return 0;
}

