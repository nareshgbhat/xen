/*
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

#ifndef _ASM_ARM64_ACPI_H
#define _ASM_ARM64_ACPI_H

#include <xen/init.h>

#define COMPILER_DEPENDENT_INT64   long long
#define COMPILER_DEPENDENT_UINT64  unsigned long long

#define MAX_LOCAL_APIC 256
#define MAX_IO_APICS 64

/*
 * Calling conventions:
 *
 * ACPI_SYSTEM_XFACE        - Interfaces to host OS (handlers, threads)
 * ACPI_EXTERNAL_XFACE      - External ACPI interfaces
 * ACPI_INTERNAL_XFACE      - Internal ACPI interfaces
 * ACPI_INTERNAL_VAR_XFACE  - Internal variable-parameter list interfaces
 */
#define ACPI_SYSTEM_XFACE
#define ACPI_EXTERNAL_XFACE
#define ACPI_INTERNAL_XFACE
#define ACPI_INTERNAL_VAR_XFACE

/* Asm macros */
#define ACPI_ASM_MACROS
#define BREAKPOINT3
#define ACPI_DISABLE_IRQS() local_irq_disable()
#define ACPI_ENABLE_IRQS()  local_irq_enable()
#define ACPI_FLUSH_CPU_CACHE() flush_cache_all()

/* Blob handling macros */
#define	ACPI_BLOB_HEADER_SIZE	8

/* Basic configuration for ACPI */
#ifdef	CONFIG_ACPI
extern int acpi_disabled;
extern int acpi_noirq;
extern int acpi_pci_disabled;
extern int acpi_strict;
extern int acpi_psci_present;
extern int acpi_psci_use_hvc;

/* map logic cpu id to physical APIC id
 * APIC = GIC cpu interface on ARM
 */
extern volatile int arm_cpu_to_apicid[NR_CPUS];
extern int boot_cpu_apic_id;
#define cpu_physical_id(cpu) arm_cpu_to_apicid[cpu]

struct acpi_arm_root {
	paddr_t phys_address;
	unsigned long size;
};
extern struct acpi_arm_root acpi_arm_rsdp_info;

void arm_acpi_reserve_memory(void);

/* Low-level suspend routine. */
extern int (*acpi_suspend_lowlevel)(void);

extern void prefill_possible_map(void);

#define acpi_wakeup_address (0)

static inline void disable_acpi(void)
{
	acpi_disabled = 1;
	acpi_pci_disabled = 1;
	acpi_noirq = 1;
}
static inline void acpi_noirq_set(void) { acpi_noirq = 1; }
static inline void acpi_disable_pci(void)
{
	acpi_pci_disabled = 1;
	acpi_noirq_set();
}

#else	/* !CONFIG_ACPI */
#define acpi_disabled 1		/* ACPI sometimes enabled on ARM */
#define acpi_noirq 1		/* ACPI sometimes enabled on ARM */
#define acpi_pci_disabled 1	/* ACPI PCI sometimes enabled on ARM */
#define acpi_strict 1		/* no ACPI spec workarounds on ARM */
#endif

#endif /*_ASM_ARM_ACPI_H*/
