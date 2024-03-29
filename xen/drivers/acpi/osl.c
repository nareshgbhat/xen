/*
 *  acpi_osl.c - OS-dependent functions ($Revision: 83 $)
 *
 *  Copyright (C) 2000       Andrew Henroid
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
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
 *
 */
#include <asm/io.h>
#include <xen/config.h>
#include <xen/init.h>
#include <xen/pfn.h>
#include <xen/types.h>
#include <xen/errno.h>
#include <xen/acpi.h>
#include <xen/numa.h>
#include <acpi/acmacros.h>
#include <acpi/acpiosxf.h>
#include <acpi/platform/aclinux.h>
#include <xen/spinlock.h>
#include <xen/domain_page.h>
#include <xen/efi.h>
#include <xen/vmap.h>

#define _COMPONENT		ACPI_OS_SERVICES
ACPI_MODULE_NAME("osl")

#ifdef CONFIG_ACPI_CUSTOM_DSDT
#include CONFIG_ACPI_CUSTOM_DSDT_FILE
#endif

void __init acpi_os_printf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	acpi_os_vprintf(fmt, args);
	va_end(args);
}

void __init acpi_os_vprintf(const char *fmt, va_list args)
{
	static char buffer[512];

	vsnprintf(buffer, sizeof(buffer), fmt, args);

	printk("%s", buffer);
}

#if defined(CONFIG_ARM_64) && defined(CONFIG_ACPI)
#include <asm/acpi.h>
#include <acpi/actbl.h>

void acpi_find_arm_root_pointer(acpi_physical_address *pa)
{
       /* BOZO: temporarily clunky.
        * What we do is, while using u-boot still, is use the values
        * that have already been retrieved from the FDT node
        * (/chosen/linux,acpi-start and /chosen/linux,acpi-len) which
        * contain the address of the first byte of the RSDP after it
        * has been loaded into RAM during u-boot (e.g., using something
        * like fatload mmc 0:2 42008000 my.blob), and the size of the
        * data in the complete ACPI blob.  We only do this since we have
        * to collaborate with FDT so we have to load FDT and the ACPI
        * tables in but only have one address we can use via bootm.
        * With UEFI, we should just be able to use the efi_enabled
        * branch below in acpi_os_get_root_pointer().
        */
       void *address;
       struct acpi_table_rsdp *rp;

       if (!acpi_arm_rsdp_info.phys_address && !acpi_arm_rsdp_info.size) {
               printk("(E) ACPI: failed to find rsdp info\n");
               *pa = (acpi_physical_address)NULL;
               return;
       }

       address = maddr_to_virt(acpi_arm_rsdp_info.phys_address);
       *pa = (acpi_physical_address)address;

       rp = (struct acpi_table_rsdp *)address;
       printk("(I) ACPI rsdp rp: 0x%08lx\n", (long unsigned int)rp);
       if (rp) {
               printk("(I) ACPI rsdp content:\n");
               printk("(I)    signature: %.8s\n", rp->signature);
               printk("(I)    checksum: 0x%02x\n", rp->checksum);
               printk("(I)    oem_id: %.6s\n", rp->oem_id);
               printk("(I)    revision: %d\n", rp->revision);
               printk("(I)    rsdt: 0x%08lX\n",
                               (long unsigned int)rp->rsdt_physical_address);
               printk("(I)    length: %d\n", rp->length);
               printk("(I)    xsdt: 0x%016llX\n",
                               (long long unsigned int)rp->xsdt_physical_address);
               printk("(I)    x_checksum: 0x%02x\n",
                               rp->extended_checksum);

       *pa = (acpi_physical_address)(virt_to_maddr(rp));
       } else {
               printk("(E) ACPI missing rsdp info\n");
               *pa = (acpi_physical_address)NULL;
       }

       return;
}
#endif

acpi_physical_address __init acpi_os_get_root_pointer(void)
{
	/* Using bootwrapper, Temporary fix to over come from efi linker error */
	const bool_t efi_enabled = 0;

	if (efi_enabled) {
		if (efi.acpi20 != EFI_INVALID_TABLE_ADDR)
			return efi.acpi20;
		else if (efi.acpi != EFI_INVALID_TABLE_ADDR)
			return efi.acpi;
		else {
			printk(KERN_ERR PREFIX
			       "System description tables not found\n");
			return 0;
		}
	} else {
		acpi_physical_address pa = 0;

#if defined(CONFIG_ARM_64) && defined(CONFIG_ACPI)
                acpi_find_arm_root_pointer(&pa);
#else
		acpi_find_root_pointer(&pa);
#endif
		return pa;
	}
}

void __iomem *
acpi_os_map_memory(acpi_physical_address phys, acpi_size size)
{
	if (system_state >= SYS_STATE_active) {
		unsigned long pfn = PFN_DOWN(phys);
		unsigned int offs = phys & (PAGE_SIZE - 1);

		/* The low first Mb is always mapped. */
		if ( !((phys + size - 1) >> 20) )
			return __va(phys);
		return __vmap(&pfn, PFN_UP(offs + size), 1, 1, PAGE_HYPERVISOR_NOCACHE) + offs;
	}
#ifdef CONFIG_X86
	return __acpi_map_table(phys, size);
#else
	return __va(phys);
#endif
}

void acpi_os_unmap_memory(void __iomem * virt, acpi_size size)
{
	if (system_state >= SYS_STATE_active)
		vunmap((void *)((unsigned long)virt & PAGE_MASK));
}

#ifdef CONFIG_X86
acpi_status acpi_os_read_port(acpi_io_address port, u32 * value, u32 width)
{
	u32 dummy;

	if (!value)
		value = &dummy;

	*value = 0;
	if (width <= 8) {
		*(u8 *) value = inb(port);
	} else if (width <= 16) {
		*(u16 *) value = inw(port);
	} else if (width <= 32) {
		*(u32 *) value = inl(port);
	} else {
		BUG();
	}

	return AE_OK;
}

acpi_status acpi_os_write_port(acpi_io_address port, u32 value, u32 width)
{
	if (width <= 8) {
		outb(value, port);
	} else if (width <= 16) {
		outw(value, port);
	} else if (width <= 32) {
		outl(value, port);
	} else {
		BUG();
	}

	return AE_OK;
}
#endif

acpi_status
acpi_os_read_memory(acpi_physical_address phys_addr, u32 * value, u32 width)
{
	u32 dummy;
	void __iomem *virt_addr = acpi_os_map_memory(phys_addr, width >> 3);

	if (!value)
		value = &dummy;

	switch (width) {
	case 8:
		*(u8 *) value = readb(virt_addr);
		break;
	case 16:
		*(u16 *) value = readw(virt_addr);
		break;
	case 32:
		*(u32 *) value = readl(virt_addr);
		break;
	default:
		BUG();
	}

	acpi_os_unmap_memory(virt_addr, width >> 3);

	return AE_OK;
}

acpi_status
acpi_os_write_memory(acpi_physical_address phys_addr, u32 value, u32 width)
{
	void __iomem *virt_addr = acpi_os_map_memory(phys_addr, width >> 3);

	switch (width) {
	case 8:
		writeb(value, virt_addr);
		break;
	case 16:
		writew(value, virt_addr);
		break;
	case 32:
		writel(value, virt_addr);
		break;
	default:
		BUG();
	}

	acpi_os_unmap_memory(virt_addr, width >> 3);

	return AE_OK;
}

#ifdef CONFIG_X86
#define is_xmalloc_memory(ptr) ((unsigned long)(ptr) & (PAGE_SIZE - 1))
#else
#define is_xmalloc_memory(ptr) 1
#endif

void *__init acpi_os_alloc_memory(size_t sz)
{
	void *ptr;

	if (system_state == SYS_STATE_early_boot)
		return mfn_to_virt(alloc_boot_pages(PFN_UP(sz), 1));

	ptr = xmalloc_bytes(sz);
	ASSERT(!ptr || is_xmalloc_memory(ptr));
	return ptr;
}

void *__init acpi_os_zalloc_memory(size_t sz)
{
	void *ptr;

	if (system_state != SYS_STATE_early_boot) {
		ptr = xzalloc_bytes(sz);
		ASSERT(!ptr || is_xmalloc_memory(ptr));
		return ptr;
	}
	ptr = acpi_os_alloc_memory(sz);
	return ptr ? memset(ptr, 0, sz) : NULL;
}

void __init acpi_os_free_memory(void *ptr)
{
	if (is_xmalloc_memory(ptr))
		xfree(ptr);
	else if (ptr && system_state == SYS_STATE_early_boot)
		init_boot_pages(__pa(ptr), __pa(ptr) + PAGE_SIZE);
}
