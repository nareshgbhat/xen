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
u32 acpi_rsdt_forced;

int acpi_noirq;                        /* skip ACPI IRQ initialization */
int acpi_strict;
int acpi_disabled;
EXPORT_SYMBOL(acpi_disabled);

int acpi_pci_disabled;         /* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

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
