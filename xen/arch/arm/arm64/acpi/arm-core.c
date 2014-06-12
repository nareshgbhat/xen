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
#include <xen/errno.h>
#include <acpi/actbl1.h>
#include <xen/cpumask.h>
#include <xen/stdbool.h>

#include <asm/acpi.h>

/*
 * We never plan to use RSDT on arm/arm64 as its deprecated in spec but this
 * variable is still required by the ACPI core
 */

int acpi_disabled;
EXPORT_SYMBOL(acpi_disabled);

/* available_cpus here means enabled cpu in MADT */
int available_cpus;

/* Map logic cpu id to physical APIC id.
 * APIC = GIC cpu interface on ARM
 */
volatile int arm_cpu_to_apicid[NR_CPUS] = { [0 ... NR_CPUS-1] = -1 };
int boot_cpu_apic_id = -1;

int acpi_noirq;                         /* skip ACPI IRQ initialization */
int acpi_pci_disabled;          /* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

struct acpi_arm_root acpi_arm_rsdp_info;     /* info about RSDP from FDT */

/* GIC cpu interface base address on ARM */
static u64 acpi_lapic_addr __initdata;

#define BAD_MADT_ENTRY(entry, end) (                                        \
                (!entry) || (unsigned long)entry + sizeof(entry) > end ||  \
                ((struct acpi_subtable_header *)entry)->length < sizeof(entry))

#define PREFIX                  "ACPI: "

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

/* arch-optional setting to enable display of offline cpus >= nr_cpu_ids */
unsigned int total_cpus;

/* Local APIC = GIC cpu interface on ARM */
static void acpi_register_lapic(int id, u8 enabled)
{
        int cpu;

        if (id >= MAX_LOCAL_APIC) {
                printk(PREFIX "skipped apicid that is too big\n");
                return;
        }

        total_cpus++;
        if (!enabled)
                return;

        available_cpus++;

        /* allocate a logic cpu id for the new comer */
        if (boot_cpu_apic_id == id) {
                /*
                 * boot_cpu_init() already hold bit 0 in cpu_present_mask
                 * for BSP, no need to allocte again.
                 */
                cpu = 0;
        } else {
                cpu = cpumask_next_zero(-1, &cpu_present_map);
        }

        /* map the logic cpu id to APIC id */
        arm_cpu_to_apicid[cpu] = id;

        set_cpu_present(cpu, true);
        set_cpu_possible(cpu, true);
}

static int __init
acpi_parse_gic(struct acpi_subtable_header *header, const unsigned long end)
{
        struct acpi_madt_generic_interrupt *processor = NULL;

        processor = (struct acpi_madt_generic_interrupt *)header;

        if (BAD_MADT_ENTRY(processor, end))
                return 1;

        acpi_table_print_madt_entry(header);

        /*
         * We need to register disabled CPU as well to permit
         * counting disabled CPUs. This allows us to size
         * cpus_possible_map more accurately, to permit
         * to not preallocating memory for all NR_CPUS
         * when we use CPU hotplug.
         */
        acpi_register_lapic(processor->gic_id,
                            processor->flags & ACPI_MADT_ENABLED);

        return 0;
}

/* Local APIC = GIC cpu interface on ARM */
static int __init acpi_parse_madt_lapic_entries(void)
{
        int count;

        /*
         * do a partial walk of MADT to determine how many CPUs
         * we have including disabled CPUs
         */
        count = acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
                                      acpi_parse_gic, MAX_LOCAL_APIC);

        if (!count) {
                printk(PREFIX "No LAPIC entries present\n");
                /* TBD: Cleanup to allow fallback to MPS */
                return -ENODEV;
        } else if (count < 0) {
                printk(PREFIX "Error parsing LAPIC entry\n");
                /* TBD: Cleanup to allow fallback to MPS */
                return count;
        }

#ifdef CONFIG_SMP
        if (available_cpus == 0) {
                printk(PREFIX "Found 0 CPUS; assuming 1\n");
                /* FIXME: should be the real GIC id read from hardware */
                arm_cpu_to_apicid[available_cpus] = 0;
                available_cpus = 1;     /* We've got at least one of these */
        }
#endif
        /* Make boot-up look pretty */
        printk("%d CPUs available, %d CPUs total\n", available_cpus,
               total_cpus);

        return 0;
}

static void __init acpi_process_madt(void)
{
        acpi_parse_madt_lapic_entries();
}

static int __init acpi_parse_fadt(struct acpi_table_header *table)
{
        return 0;
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
int __init acpi_boot_init(void)
{
        /*
         * If acpi_disabled, bail out
         */
        if (acpi_disabled)
                return 1;

	/*
         * set sci_int and PM timer address
         */
        acpi_table_parse(ACPI_SIG_FADT, acpi_parse_fadt);

	/*
         * Process the Multiple APIC Description Table (MADT), if present
         */
        acpi_process_madt();

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

