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
#include <xen/stdbool.h>
#include <xen/cpumask.h>

#include <asm/cputype.h>
#include <asm/acpi.h>

/*
 * We never plan to use RSDT on arm/arm64 as its deprecated in spec but this
 * variable is still required by the ACPI core
 */
u32 acpi_rsdt_forced;

int acpi_noirq;                        /* skip ACPI IRQ initialization */
int acpi_strict;
int acpi_disabled;
EXPORT_SYMBOL(acpi_disabled);

int acpi_pci_disabled;         /* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

/* 1 to indicate PSCI is implemented */
int acpi_psci_present;

/* 1 to indicate HVC must be used instead of SMC as the PSCI conduit */
int acpi_psci_use_hvc;

/* available_cpus means enabled cpu in MADT */
static int available_cpus;

enum acpi_irq_model_id acpi_irq_model = ACPI_IRQ_MODEL_PLATFORM;

struct acpi_arm_root acpi_arm_rsdp_info;     /* info about RSDP from FDT */

/* arch-optional setting to enable display of offline cpus >= nr_cpu_ids */
unsigned int total_cpus = 0;

/**
 * acpi_register_gic_cpu_interface - register a gic cpu interface and
 * generates a logic cpu number
 * @mpidr: CPU's hardware id to register, MPIDR represented in MADT
 * @enabled: this cpu is enabled or not
 *
 * Returns the logic cpu number which maps to the gic cpu interface
 */
static int acpi_register_gic_cpu_interface(u64 mpidr, u8 enabled)
{
       int cpu;

       if (mpidr == INVALID_HWID) {
               printk("Skip invalid cpu hardware ID\n");
               return -EINVAL;
       }

       total_cpus++;
       if (!enabled)
               return -EINVAL;

       if (available_cpus >=  NR_CPUS) {
               printk("NR_CPUS limit of %d reached, Processor %d/0x%llx ignored.\n",
                       NR_CPUS, total_cpus, (long long unsigned int)mpidr);
               return -EINVAL;
       }

       /* If it is the first CPU, no need to check duplicate MPIDRs */
       if (!available_cpus)
               goto skip_mpidr_check;

       /*
        * Duplicate MPIDRs are a recipe for disaster. Scan
        * all initialized entries and check for
        * duplicates. If any is found just ignore the CPU.
        */
       for_each_present_cpu(cpu) {
               if (cpu_logical_map(cpu) == mpidr) {
                       printk("Firmware bug, duplicate CPU MPIDR: 0x%llx in MADT\n",
                              (long long unsigned int)mpidr);
                       return -EINVAL;
               }
       }

skip_mpidr_check:
       available_cpus++;

       /* allocate a logic cpu id for the new comer */
       if (cpu_logical_map(0) == mpidr) {
               /*
                * boot_cpu_init() already hold bit 0 in cpu_present_mask
                * for BSP, no need to allocte again.
                */
               cpu = 0;
       } else {
               cpu = cpumask_next_zero(-1, &cpu_present_map);
       }

       /* map the logic cpu id to cpu MPIDR */
       cpu_logical_map(cpu) = mpidr;

       set_cpu_possible(cpu, true);
       set_cpu_present(cpu, true);

       return cpu;
}

static int __init
acpi_parse_gic_cpu_interface(struct acpi_subtable_header *header,
                               const unsigned long end)
{
       struct acpi_madt_generic_interrupt *processor;

       processor = (struct acpi_madt_generic_interrupt *)header;

       if (BAD_MADT_ENTRY(processor, end))
               return -EINVAL;

       acpi_table_print_madt_entry(header);

       acpi_register_gic_cpu_interface(processor->mpidr,
               processor->flags & ACPI_MADT_ENABLED);

       return 0;
}

/*
 * Parse GIC cpu interface related entries in MADT
 * returns 0 on success, < 0 on error
 */
static int __init acpi_parse_madt_gic_cpu_interface_entries(void)
{
       int count;

       /*
        * do a partial walk of MADT to determine how many CPUs
        * we have including disabled CPUs, and get information
        * we need for SMP init
        */
       count = acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
                       acpi_parse_gic_cpu_interface, MAX_GIC_CPU_INTERFACE);

       if (!count) {
               printk("No GIC CPU interface entries present\n");
               return -ENODEV;
       } else if (count < 0) {
               printk("Error parsing GIC CPU interface entry\n");
               return count;
       }

       /* Make boot-up look pretty */
       printk("%d CPUs available, %d CPUs total\n", available_cpus,
               total_cpus);

       return 0;
}

int acpi_gsi_to_irq(u32 gsi, unsigned int *irq)
{
       *irq = -1;

       return 0;
}
EXPORT_SYMBOL_GPL(acpi_gsi_to_irq);

/*
 * success: return IRQ number (>0)
 * failure: return =< 0
 */
//int acpi_register_gsi(struct device *dev, u32 gsi, int trigger, int polarity)
unsigned int acpi_register_gsi (u32 gsi, int edge_level, int active_high_low)
{
       return -1;
}
EXPORT_SYMBOL_GPL(acpi_register_gsi);

void acpi_unregister_gsi(u32 gsi)
{
}
EXPORT_SYMBOL_GPL(acpi_unregister_gsi);

static int __init acpi_parse_fadt(struct acpi_table_header *table)
{
       struct acpi_table_fadt *fadt = (struct acpi_table_fadt *)table;

       /*
        * Revision in table header is the FADT Major version,
        * and there is a minor version of FADT which was introduced
        * by ACPI 5.1, we only deal with ACPI 5.1 or higher version
        * to get arm boot flags, or we will disable ACPI.
        */
       if (table->revision < 5 || fadt->minor_version < 1) {
               printk("FADT version is %d.%d, no PSCI support, should be 5.1 or higher\n",
                       table->revision, fadt->minor_version);
               acpi_psci_present = 0;
               disable_acpi();
               return -EINVAL;
       }

       if (acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_COMPLIANT)
               acpi_psci_present = 1;

       if (acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_USE_HVC)
               acpi_psci_use_hvc = 1;

       return 0;
}

/*
 * acpi_boot_table_init() called from setup_arch(), always.
 *      1. find RSDP and get its address, and then find XSDT
 *      2. extract all tables and checksums them all
 *
 * We can parse ACPI boot-time tables such as FADT, MADT after
 * this function is called.
 */
int __init acpi_boot_table_init(void)
{
	int error;

        /* If acpi_disabled, bail out */
        if (acpi_disabled)
                return 1;
	
        /* Initialize the ACPI boot-time table parser. */
	error = acpi_table_init();
        if (error) {
                disable_acpi();
                return error;
        }

	return 0;
}

int __init acpi_boot_init(void)
{
       int err = 0;

       /* If acpi_disabled, bail out */
       if (acpi_disabled)
               return -ENODEV;

       err = acpi_table_parse(ACPI_SIG_FADT, acpi_parse_fadt);
       if (err)
           printk("Can't find FADT\n");

       /* Get the boot CPU's MPIDR before MADT parsing */
       cpu_logical_map(0) = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;

       err = acpi_parse_madt_gic_cpu_interface_entries();

       return err;
}
