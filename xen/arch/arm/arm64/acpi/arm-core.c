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

enum acpi_irq_model_id acpi_irq_model = ACPI_IRQ_MODEL_PLATFORM;

struct acpi_arm_root acpi_arm_rsdp_info;     /* info about RSDP from FDT */

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

       return err;
}
