/*
 * Device Tree
 *
 * Copyright (C) 2012 Citrix Systems, Inc.
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/init.h>
#include <xen/device_tree.h>
#include <xen/kernel.h>
#include <xen/lib.h>
#include <xen/libfdt/libfdt.h>
#include <xen/mm.h>
#include <xen/stdarg.h>
#include <xen/string.h>
#include <xen/cpumask.h>
#include <xen/ctype.h>
#include <xen/lib.h>
#include <asm/acpi.h>

struct dt_early_info __initdata early_info;
const void *device_tree_flattened;
dt_irq_xlate_func dt_irq_xlate;
/* Host device tree */
struct dt_device_node *dt_host;
/* Interrupt controller node*/
const struct dt_device_node *dt_interrupt_controller;

/**
 * struct dt_alias_prop - Alias property in 'aliases' node
 * @link: List node to link the structure in aliases_lookup list
 * @alias: Alias property name
 * @np: Pointer to device_node that the alias stands for
 * @id: Index value from end of alias name
 * @stem: Alias string without the index
 *
 * The structure represents one alias property of 'aliases' node as
 * an entry in aliases_lookup list.
 */
struct dt_alias_prop {
    struct list_head link;
    const char *alias;
    struct dt_device_node *np;
    int id;
    char stem[0];
};

static LIST_HEAD(aliases_lookup);

// #define DEBUG_DT

#ifdef DEBUG_DT
# define dt_dprintk(fmt, args...) printk(XENLOG_DEBUG fmt, ##args)
static void dt_dump_addr(const char *s, const __be32 *addr, int na)
{
    dt_dprintk("%s", s);
    while ( na-- )
        dt_dprintk(" %08x", be32_to_cpu(*(addr++)));
    dt_dprintk("\n");
}
#else
# define dt_dprintk(fmt, args...) do {} while ( 0 )
static void dt_dump_addr(const char *s, const __be32 *addr, int na) { }
#endif

#define DT_BAD_ADDR ((u64)-1)

/* Max address size we deal with */
#define DT_MAX_ADDR_CELLS 4
#define DT_CHECK_ADDR_COUNT(na) ((na) > 0 && (na) <= DT_MAX_ADDR_CELLS)
#define DT_CHECK_COUNTS(na, ns) (DT_CHECK_ADDR_COUNT(na) && (ns) > 0)

/* Callbacks for bus specific translators */
struct dt_bus
{
    const char *name;
    const char *addresses;
    bool_t (*match)(const struct dt_device_node *node);
    void (*count_cells)(const struct dt_device_node *child,
                        int *addrc, int *sizec);
    u64 (*map)(__be32 *addr, const __be32 *range, int na, int ns, int pna);
    int (*translate)(__be32 *addr, u64 offset, int na);
    unsigned int (*get_flags)(const __be32 *addr);
};

static bool_t __init device_tree_node_matches(const void *fdt, int node,
                                              const char *match)
{
    const char *name;
    size_t match_len;

    name = fdt_get_name(fdt, node, NULL);
    match_len = strlen(match);

    /* Match both "match" and "match@..." patterns but not
       "match-foo". */
    return strncmp(name, match, match_len) == 0
        && (name[match_len] == '@' || name[match_len] == '\0');
}

static bool_t __init device_tree_node_compatible(const void *fdt, int node,
                                                 const char *match)
{
    int len, l;
    int mlen;
    const void *prop;

    mlen = strlen(match);

    prop = fdt_getprop(fdt, node, "compatible", &len);
    if ( prop == NULL )
        return 0;

    while ( len > 0 ) {
        if ( !dt_compat_cmp(prop, match) )
            return 1;
        l = strlen(prop) + 1;
        prop += l;
        len -= l;
    }

    return 0;
}

static void __init device_tree_get_reg(const __be32 **cell, u32 address_cells,
                                       u32 size_cells, u64 *start, u64 *size)
{
    *start = dt_next_cell(address_cells, cell);
    *size = dt_next_cell(size_cells, cell);
}

void dt_get_range(const __be32 **cell, const struct dt_device_node *np,
                  u64 *address, u64 *size)
{
    *address = dt_next_cell(dt_n_addr_cells(np), cell);
    *size = dt_next_cell(dt_n_size_cells(np), cell);
}

void dt_set_cell(__be32 **cellp, int size, u64 val)
{
    int cells = size;

    while ( size-- )
    {
        (*cellp)[size] = cpu_to_fdt32(val);
        val >>= 32;
    }

    (*cellp) += cells;
}

void dt_set_range(__be32 **cellp, const struct dt_device_node *np,
                  u64 address, u64 size)
{
    dt_set_cell(cellp, dt_n_addr_cells(np), address);
    dt_set_cell(cellp, dt_n_size_cells(np), size);
}

static u32 __init device_tree_get_u32(const void *fdt, int node,
                                      const char *prop_name, u32 dflt)
{
    const struct fdt_property *prop;

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop || prop->len < sizeof(u32) )
        return dflt;

    return fdt32_to_cpu(*(uint32_t*)prop->data);
}

/**
 * device_tree_for_each_node - iterate over all device tree nodes
 * @fdt: flat device tree.
 * @func: function to call for each node.
 * @data: data to pass to @func.
 *
 * Any nodes nested at DEVICE_TREE_MAX_DEPTH or deeper are ignored.
 *
 * Returns 0 if all nodes were iterated over successfully.  If @func
 * returns a value different from 0, that value is returned immediately.
 */
static int __init device_tree_for_each_node(const void *fdt,
                                            device_tree_node_func func,
                                            void *data)
{
    int node;
    int depth;
    u32 address_cells[DEVICE_TREE_MAX_DEPTH];
    u32 size_cells[DEVICE_TREE_MAX_DEPTH];
    int ret;

    for ( node = 0, depth = 0;
          node >=0 && depth >= 0;
          node = fdt_next_node(fdt, node, &depth) )
    {
        const char *name = fdt_get_name(fdt, node, NULL);

        if ( depth >= DEVICE_TREE_MAX_DEPTH )
        {
            printk("Warning: device tree node `%s' is nested too deep\n",
                   name);
            continue;
        }

        address_cells[depth] = device_tree_get_u32(fdt, node, "#address-cells",
                                depth > 0 ? address_cells[depth-1] : 0);
        size_cells[depth] = device_tree_get_u32(fdt, node, "#size-cells",
                                depth > 0 ? size_cells[depth-1] : 0);


        ret = func(fdt, node, name, depth,
                   address_cells[depth-1], size_cells[depth-1], data);
        if ( ret != 0 )
            return ret;
    }
    return 0;
}

/**
 * device_tree_bootargs - return the bootargs (the Xen command line)
 * @fdt flat device tree.
 */
const char *device_tree_bootargs(const void *fdt)
{
    int node;
    const struct fdt_property *prop;

    node = fdt_path_offset(fdt, "/chosen");
    if ( node < 0 )
        return NULL;

    prop = fdt_get_property(fdt, node, "xen,xen-bootargs", NULL);
    if ( prop == NULL )
    {
        struct dt_mb_module *dom0_mod = NULL;

        if ( early_info.modules.nr_mods >= MOD_KERNEL )
            dom0_mod = &early_info.modules.module[MOD_KERNEL];

        if (fdt_get_property(fdt, node, "xen,dom0-bootargs", NULL) ||
            ( dom0_mod && dom0_mod->cmdline[0] ) )
            prop = fdt_get_property(fdt, node, "bootargs", NULL);
    }
    if ( prop == NULL )
        return NULL;

    return prop->data;
}

static int dump_node(const void *fdt, int node, const char *name, int depth,
                     u32 address_cells, u32 size_cells, void *data)
{
    char prefix[2*DEVICE_TREE_MAX_DEPTH + 1] = "";
    int i;
    int prop;

    for ( i = 0; i < depth; i++ )
        safe_strcat(prefix, "  ");

    if ( name[0] == '\0' )
        name = "/";
    printk("%s%s:\n", prefix, name);

    for ( prop = fdt_first_property_offset(fdt, node);
          prop >= 0;
          prop = fdt_next_property_offset(fdt, prop) )
    {
        const struct fdt_property *p;

        p = fdt_get_property_by_offset(fdt, prop, NULL);

        printk("%s  %s\n", prefix, fdt_string(fdt, fdt32_to_cpu(p->nameoff)));
    }

    return 0;
}

/**
 * device_tree_dump - print a text representation of a device tree
 * @fdt: flat device tree to print
 */
void __init device_tree_dump(const void *fdt)
{
    device_tree_for_each_node(fdt, dump_node, NULL);
}


static void __init process_memory_node(const void *fdt, int node,
                                       const char *name,
                                       u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    int i;
    int banks;
    const __be32 *cell;
    paddr_t start, size;
    u32 reg_cells = address_cells + size_cells;

    if ( address_cells < 1 || size_cells < 1 )
    {
        printk("fdt: node `%s': invalid #address-cells or #size-cells",
               name);
        return;
    }

    prop = fdt_get_property(fdt, node, "reg", NULL);
    if ( !prop )
    {
        printk("fdt: node `%s': missing `reg' property\n", name);
        return;
    }

    cell = (const __be32 *)prop->data;
    banks = fdt32_to_cpu(prop->len) / (reg_cells * sizeof (u32));

    for ( i = 0; i < banks && early_info.mem.nr_banks < NR_MEM_BANKS; i++ )
    {
        device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
        early_info.mem.bank[early_info.mem.nr_banks].start = start;
        early_info.mem.bank[early_info.mem.nr_banks].size = size;
        early_info.mem.nr_banks++;
    }
}

static void __init process_multiboot_node(const void *fdt, int node,
                                          const char *name,
                                          u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    const __be32 *cell;
    int nr;
    struct dt_mb_module *mod;
    int len;

    if ( fdt_node_check_compatible(fdt, node, "xen,linux-zimage") == 0 )
        nr = MOD_KERNEL;
    else if ( fdt_node_check_compatible(fdt, node, "xen,linux-initrd") == 0)
        nr = MOD_INITRD;
    else if ( fdt_node_check_compatible(fdt, node, "xen,xsm-policy") == 0 )
        nr = MOD_XSM;
    else
        panic("%s not a known xen multiboot type\n", name);

    mod = &early_info.modules.module[nr];

    prop = fdt_get_property(fdt, node, "reg", &len);
    if ( !prop )
        panic("node %s missing `reg' property\n", name);

    if ( len < dt_cells_to_size(address_cells + size_cells) )
        panic("fdt: node `%s': `reg` property length is too short\n",
                    name);

    cell = (const __be32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells,
                        &mod->start, &mod->size);

    prop = fdt_get_property(fdt, node, "bootargs", &len);
    if ( prop )
    {
        if ( len > sizeof(mod->cmdline) )
            panic("module %d command line too long\n", nr);

        safe_strcpy(mod->cmdline, prop->data);
    }
    else
        mod->cmdline[0] = 0;

    if ( nr > early_info.modules.nr_mods )
        early_info.modules.nr_mods = nr;
}

static void __init process_chosen_node(const void *fdt, int node,
                                       const char *name,
                                       u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    struct dt_mb_module *mod = &early_info.modules.module[MOD_INITRD];
    paddr_t start, end;
    int len;

    printk("\nACPI: Checking for ACPI in /chosen node");

    /* Scan ACPI choosen node information */
    prop = fdt_get_property(fdt, node, "linux,acpi-start", &len);
    if ( !prop )
       /* no ACPI present */
       return;
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,acpi-start property has invalid length %d\n", len);
        return;
    }
    start = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    prop = fdt_get_property(fdt, node, "linux,acpi-len", &len);
    if ( !prop )
    {
        printk("linux,acpi-len not present but -start was\n");
        return;
    }
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,acpi-len property has invalid length %d\n", len);
        return;
    }
    end = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    printk("\nACPI: Start info is %"PRIpaddr"-%"PRIpaddr" bytes \n", start, end);

    /* Update the global RSDP structure */
    acpi_arm_rsdp_info.phys_address = start;
    acpi_arm_rsdp_info.size = end;

    printk("Checking for initrd in /chosen\n");

    prop = fdt_get_property(fdt, node, "linux,initrd-start", &len);
    if ( !prop )
        /* No initrd present. */
        return;
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-start property has invalid length %d\n", len);
        return;
    }
    start = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    prop = fdt_get_property(fdt, node, "linux,initrd-end", &len);
    if ( !prop )
    {
        printk("linux,initrd-end not present but -start was\n");
        return;
    }
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-end property has invalid length %d\n", len);
        return;
    }
    end = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    if ( start >= end )
    {
        printk("linux,initrd limits invalid: %"PRIpaddr" >= %"PRIpaddr"\n",
                  start, end);
        return;
    }

    printk("Initrd %"PRIpaddr"-%"PRIpaddr"\n", start, end);

    mod->start = start;
    mod->size = end - start;

    early_info.modules.nr_mods = MAX(MOD_INITRD, early_info.modules.nr_mods);
}

static int __init early_scan_node(const void *fdt,
                                  int node, const char *name, int depth,
                                  u32 address_cells, u32 size_cells,
                                  void *data)
{
    if ( device_tree_node_matches(fdt, node, "memory") )
        process_memory_node(fdt, node, name, address_cells, size_cells);
    else if ( device_tree_node_compatible(fdt, node, "xen,multiboot-module" ) )
        process_multiboot_node(fdt, node, name, address_cells, size_cells);
    else if ( depth == 1 && device_tree_node_matches(fdt, node, "chosen") )
        process_chosen_node(fdt, node, name, address_cells, size_cells);

    return 0;
}

static void __init early_print_info(void)
{
    struct dt_mem_info *mi = &early_info.mem;
    struct dt_module_info *mods = &early_info.modules;
    int i, nr_rsvd;

    for ( i = 0; i < mi->nr_banks; i++ )
        printk("RAM: %"PRIpaddr" - %"PRIpaddr"\n",
                     mi->bank[i].start,
                     mi->bank[i].start + mi->bank[i].size - 1);
    printk("\n");
    for ( i = 1 ; i < mods->nr_mods + 1; i++ )
        printk("MODULE[%d]: %"PRIpaddr" - %"PRIpaddr" %s\n",
                     i,
                     mods->module[i].start,
                     mods->module[i].start + mods->module[i].size,
                     mods->module[i].cmdline);
    nr_rsvd = fdt_num_mem_rsv(device_tree_flattened);
    for ( i = 0; i < nr_rsvd; i++ )
    {
        paddr_t s, e;
        if ( fdt_get_mem_rsv(device_tree_flattened, i, &s, &e) < 0 )
            continue;
        /* fdt_get_mem_rsv returns length */
        e += s;
        printk(" RESVD[%d]: %"PRIpaddr" - %"PRIpaddr"\n",
                     i, s, e);
    }
    printk("\n");
}

/**
 * device_tree_early_init - initialize early info from a DTB
 * @fdt: flattened device tree binary
 *
 * Returns the size of the DTB.
 */
size_t __init device_tree_early_init(const void *fdt, paddr_t paddr)
{
    struct dt_mb_module *mod;
    int ret;

    ret = fdt_check_header(fdt);
    if ( ret < 0 )
        panic("No valid device tree\n");

    mod = &early_info.modules.module[MOD_FDT];
    mod->start = paddr;
    mod->size = fdt_totalsize(fdt);

    early_info.modules.nr_mods = max(MOD_FDT, early_info.modules.nr_mods);

    device_tree_for_each_node((void *)fdt, early_scan_node, NULL);
    early_print_info();

    return fdt_totalsize(fdt);
}

static void __init *unflatten_dt_alloc(unsigned long *mem, unsigned long size,
                                       unsigned long align)
{
    void *res;

    *mem = ROUNDUP(*mem, align);
    res = (void *)*mem;
    *mem += size;

    return res;
}

/* Find a property with a given name for a given node and return it. */
const struct dt_property *dt_find_property(const struct dt_device_node *np,
                                           const char *name, u32 *lenp)
{
    const struct dt_property *pp;

    if ( !np )
        return NULL;

    for ( pp = np->properties; pp; pp = pp->next )
    {
        if ( dt_prop_cmp(pp->name, name) == 0 )
        {
            if ( lenp )
                *lenp = pp->length;
            break;
        }
    }

    return pp;
}

const void *dt_get_property(const struct dt_device_node *np,
                            const char *name, u32 *lenp)
{
    const struct dt_property *pp = dt_find_property(np, name, lenp);

    return pp ? pp->value : NULL;
}

bool_t dt_property_read_u32(const struct dt_device_node *np,
                         const char *name, u32 *out_value)
{
    u32 len;
    const __be32 *val;

    val = dt_get_property(np, name, &len);
    if ( !val || len < sizeof(*out_value) )
        return 0;

    *out_value = be32_to_cpup(val);

    return 1;
}


bool_t dt_property_read_u64(const struct dt_device_node *np,
                         const char *name, u64 *out_value)
{
    u32 len;
    const __be32 *val;

    val = dt_get_property(np, name, &len);
    if ( !val || len < sizeof(*out_value) )
        return 0;

    *out_value = dt_read_number(val, 2);

    return 1;
}
int dt_property_read_string(const struct dt_device_node *np,
                            const char *propname, const char **out_string)
{
    const struct dt_property *pp = dt_find_property(np, propname, NULL);

    if ( !pp )
        return -EINVAL;
    if ( !pp->value )
        return -ENODATA;
    if ( strnlen(pp->value, pp->length) >= pp->length )
        return -EILSEQ;

    *out_string = pp->value;

    return 0;
}

bool_t dt_device_is_compatible(const struct dt_device_node *device,
                               const char *compat)
{
    const char* cp;
    u32 cplen, l;

    cp = dt_get_property(device, "compatible", &cplen);
    if ( cp == NULL )
        return 0;
    while ( cplen > 0 )
    {
        if ( dt_compat_cmp(cp, compat) == 0 )
            return 1;
        l = strlen(cp) + 1;
        cp += l;
        cplen -= l;
    }

    return 0;
}

bool_t dt_machine_is_compatible(const char *compat)
{
    const struct dt_device_node *root;
    bool_t rc = 0;

    root = dt_find_node_by_path("/");
    if ( root )
    {
        rc = dt_device_is_compatible(root, compat);
    }
    return rc;
}

struct dt_device_node *dt_find_node_by_name(struct dt_device_node *from,
                                            const char *name)
{
    struct dt_device_node *np;
    struct dt_device_node *dt;

    dt = from ? from->allnext : dt_host;
    dt_for_each_device_node(dt, np)
        if ( np->name && (dt_node_cmp(np->name, name) == 0) )
            break;

    return np;
}

struct dt_device_node *dt_find_node_by_type(struct dt_device_node *from,
                                            const char *type)
{
    struct dt_device_node *np;
    struct dt_device_node *dt;

    dt = from ? from->allnext : dt_host;
    dt_for_each_device_node(dt, np)
        if ( np->type && (dt_node_cmp(np->type, type) == 0) )
            break;

    return np;
}

struct dt_device_node *dt_find_node_by_path(const char *path)
{
    struct dt_device_node *np;

    dt_for_each_device_node(dt_host, np)
        if ( np->full_name && (dt_node_cmp(np->full_name, path) == 0) )
            break;

    return np;
}

struct dt_device_node *dt_find_node_by_alias(const char *alias)
{
    const struct dt_alias_prop *app;

    list_for_each_entry( app, &aliases_lookup, link )
    {
        if ( !strcmp(app->alias, alias) )
            return app->np;
    }

    return NULL;
}

bool_t dt_match_node(const struct dt_device_match *matches,
                     const struct dt_device_node *node)
{
    if ( !matches )
        return 0;

    while ( matches->path || matches->type || matches->compatible )
    {
        bool_t match = 1;

        if ( matches->path )
            match &= dt_node_path_is_equal(node, matches->path);

        if ( matches->type )
            match &= dt_device_type_is_equal(node, matches->type);

        if ( matches->compatible )
            match &= dt_device_is_compatible(node, matches->compatible);

        if ( match )
            return match;

        matches++;
    }

    return 0;
}

const struct dt_device_node *dt_get_parent(const struct dt_device_node *node)
{
    if ( !node )
        return NULL;

    return node->parent;
}

struct dt_device_node *
dt_find_compatible_node(struct dt_device_node *from,
                        const char *type,
                        const char *compatible)
{
    struct dt_device_node *np;
    struct dt_device_node *dt;

    dt = from ? from->allnext : dt_host;
    dt_for_each_device_node(dt, np)
    {
        if ( type
             && !(np->type && (dt_node_cmp(np->type, type) == 0)) )
            continue;
        if ( dt_device_is_compatible(np, compatible) )
            break;
    }

    return np;
}

struct dt_device_node *
dt_find_matching_node(struct dt_device_node *from,
                      const struct dt_device_match *matches)
{
    struct dt_device_node *np;
    struct dt_device_node *dt;

    dt = from ? from->allnext : dt_host;
    dt_for_each_device_node(dt, np)
    {
        if ( dt_match_node(matches, np) )
            return np;
    }

    return NULL;
}

int dt_n_addr_cells(const struct dt_device_node *np)
{
    const __be32 *ip;

    do {
        if ( np->parent )
            np = np->parent;
        ip = dt_get_property(np, "#address-cells", NULL);
        if ( ip )
            return be32_to_cpup(ip);
    } while ( np->parent );
    /* No #address-cells property for the root node */
    return DT_ROOT_NODE_ADDR_CELLS_DEFAULT;
}

int dt_n_size_cells(const struct dt_device_node *np)
{
    const __be32 *ip;

    do {
        if ( np->parent )
            np = np->parent;
        ip = dt_get_property(np, "#size-cells", NULL);
        if ( ip )
            return be32_to_cpup(ip);
    } while ( np->parent );
    /* No #address-cells property for the root node */
    return DT_ROOT_NODE_SIZE_CELLS_DEFAULT;
}

/*
 * Default translator (generic bus)
 */
static bool_t dt_bus_default_match(const struct dt_device_node *node)
{
    /* Root node doesn't have "ranges" property */
    if ( node->parent == NULL )
        return 1;

    /* The default bus is only used when the "ranges" property exists.
     * Otherwise we can't translate the address
     */
    return (dt_get_property(node, "ranges", NULL) != NULL);
}

static void dt_bus_default_count_cells(const struct dt_device_node *dev,
                                int *addrc, int *sizec)
{
    if ( addrc )
        *addrc = dt_n_addr_cells(dev);
    if ( sizec )
        *sizec = dt_n_size_cells(dev);
}

static u64 dt_bus_default_map(__be32 *addr, const __be32 *range,
                              int na, int ns, int pna)
{
    u64 cp, s, da;

    cp = dt_read_number(range, na);
    s = dt_read_number(range + na + pna, ns);
    da = dt_read_number(addr, na);

    dt_dprintk("DT: default map, cp=%llx, s=%llx, da=%llx\n",
               (unsigned long long)cp, (unsigned long long)s,
               (unsigned long long)da);

    /*
     * If the number of address cells is larger than 2 we assume the
     * mapping doesn't specify a physical address. Rather, the address
     * specifies an identifier that must match exactly.
     */
    if ( na > 2 && memcmp(range, addr, na * 4) != 0 )
        return DT_BAD_ADDR;

    if ( da < cp || da >= (cp + s) )
        return DT_BAD_ADDR;
    return da - cp;
}

static int dt_bus_default_translate(__be32 *addr, u64 offset, int na)
{
    u64 a = dt_read_number(addr, na);

    memset(addr, 0, na * 4);
    a += offset;
    if ( na > 1 )
        addr[na - 2] = cpu_to_be32(a >> 32);
    addr[na - 1] = cpu_to_be32(a & 0xffffffffu);

    return 0;
}
static unsigned int dt_bus_default_get_flags(const __be32 *addr)
{
    /* TODO: Return the type of memory (device, ...) for caching
     * attribute during mapping */
    return 0;
}

/*
 * Array of bus specific translators
 */
static const struct dt_bus dt_busses[] =
{
    /* Default */
    {
        .name = "default",
        .addresses = "reg",
        .match = dt_bus_default_match,
        .count_cells = dt_bus_default_count_cells,
        .map = dt_bus_default_map,
        .translate = dt_bus_default_translate,
        .get_flags = dt_bus_default_get_flags,
    },
};

static const struct dt_bus *dt_match_bus(const struct dt_device_node *np)
{
    int i;

    for ( i = 0; i < ARRAY_SIZE(dt_busses); i++ )
        if ( !dt_busses[i].match || dt_busses[i].match(np) )
            return &dt_busses[i];

    return NULL;
}

static const __be32 *dt_get_address(const struct dt_device_node *dev,
                                    int index, u64 *size,
                                    unsigned int *flags)
{
    const __be32 *prop;
    u32 psize;
    const struct dt_device_node *parent;
    const struct dt_bus *bus;
    int onesize, i, na, ns;

    /* Get parent & match bus type */
    parent = dt_get_parent(dev);
    if ( parent == NULL )
        return NULL;

    bus = dt_match_bus(parent);
    if ( !bus )
        return NULL;
    bus->count_cells(dev, &na, &ns);

    if ( !DT_CHECK_ADDR_COUNT(na) )
        return NULL;

    /* Get "reg" or "assigned-addresses" property */
    prop = dt_get_property(dev, bus->addresses, &psize);
    if ( prop == NULL )
        return NULL;
    psize /= 4;

    onesize = na + ns;
    for ( i = 0; psize >= onesize; psize -= onesize, prop += onesize, i++ )
    {
        if ( i == index )
        {
            if ( size )
                *size = dt_read_number(prop + na, ns);
            if ( flags )
                *flags = bus->get_flags(prop);
            return prop;
        }
    }
    return NULL;
}

static int dt_translate_one(const struct dt_device_node *parent,
                            const struct dt_bus *bus,
                            const struct dt_bus *pbus,
                            __be32 *addr, int na, int ns,
                            int pna, const char *rprop)
{
    const __be32 *ranges;
    unsigned int rlen;
    int rone;
    u64 offset = DT_BAD_ADDR;

    ranges = dt_get_property(parent, rprop, &rlen);
    if ( ranges == NULL )
    {
        printk(XENLOG_ERR "DT: no ranges; cannot translate\n");
        return 1;
    }
    if ( rlen == 0 )
    {
        offset = dt_read_number(addr, na);
        memset(addr, 0, pna * 4);
        dt_dprintk("DT: empty ranges; 1:1 translation\n");
        goto finish;
    }

    dt_dprintk("DT: walking ranges...\n");

    /* Now walk through the ranges */
    rlen /= 4;
    rone = na + pna + ns;
    for ( ; rlen >= rone; rlen -= rone, ranges += rone )
    {
        offset = bus->map(addr, ranges, na, ns, pna);
        if ( offset != DT_BAD_ADDR )
            break;
    }
    if ( offset == DT_BAD_ADDR )
    {
        dt_dprintk("DT: not found !\n");
        return 1;
    }
    memcpy(addr, ranges + na, 4 * pna);

finish:
    dt_dump_addr("DT: parent translation for:", addr, pna);
    dt_dprintk("DT: with offset: %llx\n", (unsigned long long)offset);

    /* Translate it into parent bus space */
    return pbus->translate(addr, offset, pna);
}

/*
 * Translate an address from the device-tree into a CPU physical address,
 * this walks up the tree and applies the various bus mappings on the
 * way.
 *
 * Note: We consider that crossing any level with #size-cells == 0 to mean
 * that translation is impossible (that is we are not dealing with a value
 * that can be mapped to a cpu physical address). This is not really specified
 * that way, but this is traditionally the way IBM at least do things
 */
static u64 __dt_translate_address(const struct dt_device_node *dev,
                                  const __be32 *in_addr, const char *rprop)
{
    const struct dt_device_node *parent = NULL;
    const struct dt_bus *bus, *pbus;
    __be32 addr[DT_MAX_ADDR_CELLS];
    int na, ns, pna, pns;
    u64 result = DT_BAD_ADDR;

    dt_dprintk("DT: ** translation for device %s **\n", dev->full_name);

    /* Get parent & match bus type */
    parent = dt_get_parent(dev);
    if ( parent == NULL )
        goto bail;
    bus = dt_match_bus(parent);
    if ( !bus )
        goto bail;

    /* Count address cells & copy address locally */
    bus->count_cells(dev, &na, &ns);
    if ( !DT_CHECK_COUNTS(na, ns) )
    {
        printk(XENLOG_ERR "dt_parse: Bad cell count for device %s\n",
                  dev->full_name);
        goto bail;
    }
    memcpy(addr, in_addr, na * 4);

    dt_dprintk("DT: bus is %s (na=%d, ns=%d) on %s\n",
               bus->name, na, ns, parent->full_name);
    dt_dump_addr("DT: translating address:", addr, na);

    /* Translate */
    for ( ;; )
    {
        /* Switch to parent bus */
        dev = parent;
        parent = dt_get_parent(dev);

        /* If root, we have finished */
        if ( parent == NULL )
        {
            dt_dprintk("DT: reached root node\n");
            result = dt_read_number(addr, na);
            break;
        }

        /* Get new parent bus and counts */
        pbus = dt_match_bus(parent);
        if ( pbus == NULL )
        {
            printk("DT: %s is not a valid bus\n", parent->full_name);
            break;
        }
        pbus->count_cells(dev, &pna, &pns);
        if ( !DT_CHECK_COUNTS(pna, pns) )
        {
            printk(XENLOG_ERR "dt_parse: Bad cell count for parent %s\n",
                   dev->full_name);
            break;
        }

        dt_dprintk("DT: parent bus is %s (na=%d, ns=%d) on %s\n",
                   pbus->name, pna, pns, parent->full_name);

        /* Apply bus translation */
        if ( dt_translate_one(dev, bus, pbus, addr, na, ns, pna, rprop) )
            break;

        /* Complete the move up one level */
        na = pna;
        ns = pns;
        bus = pbus;

        dt_dump_addr("DT: one level translation:", addr, na);
    }

bail:
    return result;
}

/* dt_device_address - Translate device tree address and return it */
int dt_device_get_address(const struct dt_device_node *dev, int index,
                          u64 *addr, u64 *size)
{
    const __be32 *addrp;
    unsigned int flags;

    addrp = dt_get_address(dev, index, size, &flags);
    if ( addrp == NULL )
        return -EINVAL;

    if ( !addr )
        return -EINVAL;

    *addr = __dt_translate_address(dev, addrp, "ranges");

    if ( *addr == DT_BAD_ADDR )
        return -EINVAL;

    return 0;
}

/**
 * dt_find_node_by_phandle - Find a node given a phandle
 * @handle: phandle of the node to find
 *
 * Returns a node pointer.
 */
static struct dt_device_node *dt_find_node_by_phandle(dt_phandle handle)
{
    struct dt_device_node *np;

    dt_for_each_device_node(dt_host, np)
        if ( np->phandle == handle )
            break;

    return np;
}

/**
 * dt_irq_find_parent - Given a device node, find its interrupt parent node
 * @child: pointer to device node
 *
 * Returns a pointer to the interrupt parent node, or NULL if the interrupt
 * parent could not be determined.
 */
static const struct dt_device_node *
dt_irq_find_parent(const struct dt_device_node *child)
{
    const struct dt_device_node *p;
    const __be32 *parp;

    do
    {
        parp = dt_get_property(child, "interrupt-parent", NULL);
        if ( parp == NULL )
            p = dt_get_parent(child);
        else
            p = dt_find_node_by_phandle(be32_to_cpup(parp));
        child = p;
    } while ( p && dt_get_property(p, "#interrupt-cells", NULL) == NULL );

    return p;
}

unsigned int dt_number_of_irq(const struct dt_device_node *device)
{
    const struct dt_device_node *p;
    const __be32 *intspec, *tmp;
    u32 intsize, intlen;

    dt_dprintk("dt_irq_number: dev=%s\n", device->full_name);

    /* Get the interrupts property */
    intspec = dt_get_property(device, "interrupts", &intlen);
    if ( intspec == NULL )
        return 0;
    intlen /= sizeof(*intspec);

    dt_dprintk(" intspec=%d intlen=%d\n", be32_to_cpup(intspec), intlen);

    /* Look for the interrupt parent. */
    p = dt_irq_find_parent(device);
    if ( p == NULL )
        return 0;

    /* Get size of interrupt specifier */
    tmp = dt_get_property(p, "#interrupt-cells", NULL);
    if ( tmp == NULL )
        return 0;
    intsize = be32_to_cpu(*tmp);

    dt_dprintk(" intsize=%d intlen=%d\n", intsize, intlen);

    return (intlen / intsize);
}

unsigned int dt_number_of_address(const struct dt_device_node *dev)
{
    const __be32 *prop;
    u32 psize;
    const struct dt_device_node *parent;
    const struct dt_bus *bus;
    int onesize, na, ns;

    /* Get parent & match bus type */
    parent = dt_get_parent(dev);
    if ( parent == NULL )
        return 0;

    bus = dt_match_bus(parent);
    if ( !bus )
        return 0;
    bus->count_cells(dev, &na, &ns);

    if ( !DT_CHECK_COUNTS(na, ns) )
        return 0;

    /* Get "reg" or "assigned-addresses" property */
    prop = dt_get_property(dev, bus->addresses, &psize);
    if ( prop == NULL )
        return 0;

    psize /= 4;
    onesize = na + ns;

    return (psize / onesize);
}

/**
 * dt_irq_map_raw - Low level interrupt tree parsing
 * @parent:     the device interrupt parent
 * @intspec:    interrupt specifier ("interrupts" property of the device)
 * @ointsize:   size of the passed in interrupt specifier
 * @addr:       address specifier (start of "reg" property of the device)
 * @oirq:       structure dt_raw_irq filled by this function
 *
 * Returns 0 on success and a negative number on error
 *
 * This function is a low-level interrupt tree walking function. It
 * can be used to do a partial walk with synthesized reg and interrupts
 * properties, for example when resolving PCI interrupts when no device
 * node exist for the parent.
 */
static int dt_irq_map_raw(const struct dt_device_node *parent,
                          const __be32 *intspec, u32 ointsize,
                          const __be32 *addr,
                          struct dt_raw_irq *oirq)
{
    const struct dt_device_node *ipar, *tnode, *old = NULL, *newpar = NULL;
    const __be32 *tmp, *imap, *imask;
    u32 intsize = 1, addrsize, newintsize = 0, newaddrsize = 0;
    u32 imaplen;
    int match, i;

    dt_dprintk("dt_irq_map_raw: par=%s,intspec=[0x%08x 0x%08x...],ointsize=%d\n",
               parent->full_name, be32_to_cpup(intspec),
               be32_to_cpup(intspec + 1), ointsize);

    ipar = parent;

    /* First get the #interrupt-cells property of the current cursor
     * that tells us how to interpret the passed-in intspec. If there
     * is none, we are nice and just walk up the tree
     */
    do {
        tmp = dt_get_property(ipar, "#interrupt-cells", NULL);
        if ( tmp != NULL )
        {
            intsize = be32_to_cpu(*tmp);
            break;
        }
        tnode = ipar;
        ipar = dt_irq_find_parent(ipar);
    } while ( ipar );
    if ( ipar == NULL )
    {
        dt_dprintk(" -> no parent found !\n");
        goto fail;
    }

    dt_dprintk("dt_irq_map_raw: ipar=%s, size=%d\n", ipar->full_name, intsize);

    if ( ointsize != intsize )
        return -EINVAL;

    /* Look for this #address-cells. We have to implement the old linux
     * trick of looking for the parent here as some device-trees rely on it
     */
    old = ipar;
    do {
        tmp = dt_get_property(old, "#address-cells", NULL);
        tnode = dt_get_parent(old);
        old = tnode;
    } while ( old && tmp == NULL );

    old = NULL;
    addrsize = (tmp == NULL) ? 2 : be32_to_cpu(*tmp);

    dt_dprintk(" -> addrsize=%d\n", addrsize);

    /* Now start the actual "proper" walk of the interrupt tree */
    while ( ipar != NULL )
    {
        /* Now check if cursor is an interrupt-controller and if it is
         * then we are done
         */
        if ( dt_get_property(ipar, "interrupt-controller", NULL) != NULL )
        {
            dt_dprintk(" -> got it !\n");
            if ( intsize > DT_MAX_IRQ_SPEC )
            {
                dt_dprintk(" -> intsize(%u) greater than DT_MAX_IRQ_SPEC(%u)\n",
                           intsize, DT_MAX_IRQ_SPEC);
                goto fail;
            }
            for ( i = 0; i < intsize; i++ )
                oirq->specifier[i] = dt_read_number(intspec + i, 1);
            oirq->size = intsize;
            oirq->controller = ipar;
            return 0;
        }

        /* Now look for an interrupt-map */
        imap = dt_get_property(ipar, "interrupt-map", &imaplen);
        /* No interrupt map, check for an interrupt parent */
        if ( imap == NULL )
        {
            dt_dprintk(" -> no map, getting parent\n");
            newpar = dt_irq_find_parent(ipar);
            goto skiplevel;
        }
        imaplen /= sizeof(u32);

        /* Look for a mask */
        imask = dt_get_property(ipar, "interrupt-map-mask", NULL);

        /* If we were passed no "reg" property and we attempt to parse
         * an interrupt-map, then #address-cells must be 0.
         * Fail if it's not.
         */
        if ( addr == NULL && addrsize != 0 )
        {
            dt_dprintk(" -> no reg passed in when needed !\n");
            goto fail;
        }

        /* Parse interrupt-map */
        match = 0;
        while ( imaplen > (addrsize + intsize + 1) && !match )
        {
            /* Compare specifiers */
            match = 1;
            for ( i = 0; i < addrsize && match; ++i )
            {
                __be32 mask = imask ? imask[i] : cpu_to_be32(0xffffffffu);
                match = ((addr[i] ^ imap[i]) & mask) == 0;
            }
            for ( ; i < (addrsize + intsize) && match; ++i )
            {
                __be32 mask = imask ? imask[i] : cpu_to_be32(0xffffffffu);
                match = ((intspec[i-addrsize] ^ imap[i]) & mask) == 0;
            }
            imap += addrsize + intsize;
            imaplen -= addrsize + intsize;

            dt_dprintk(" -> match=%d (imaplen=%d)\n", match, imaplen);

            /* Get the interrupt parent */
            newpar = dt_find_node_by_phandle(be32_to_cpup(imap));
            imap++;
            --imaplen;

            /* Check if not found */
            if ( newpar == NULL )
            {
                dt_dprintk(" -> imap parent not found !\n");
                goto fail;
            }

            /* Get #interrupt-cells and #address-cells of new
             * parent
             */
            tmp = dt_get_property(newpar, "#interrupt-cells", NULL);
            if ( tmp == NULL )
            {
                dt_dprintk(" -> parent lacks #interrupt-cells!\n");
                goto fail;
            }
            newintsize = be32_to_cpu(*tmp);
            tmp = dt_get_property(newpar, "#address-cells", NULL);
            newaddrsize = (tmp == NULL) ? 0 : be32_to_cpu(*tmp);

            dt_dprintk(" -> newintsize=%d, newaddrsize=%d\n",
                       newintsize, newaddrsize);

            /* Check for malformed properties */
            if ( imaplen < (newaddrsize + newintsize) )
                goto fail;

            imap += newaddrsize + newintsize;
            imaplen -= newaddrsize + newintsize;

            dt_dprintk(" -> imaplen=%d\n", imaplen);
        }
        if ( !match )
            goto fail;

        old = newpar;
        addrsize = newaddrsize;
        intsize = newintsize;
        intspec = imap - intsize;
        addr = intspec - addrsize;

    skiplevel:
        /* Iterate again with new parent */
        dt_dprintk(" -> new parent: %s\n", dt_node_full_name(newpar));
        ipar = newpar;
        newpar = NULL;
    }
fail:
    return -EINVAL;
}

int dt_device_get_raw_irq(const struct dt_device_node *device, int index,
                          struct dt_raw_irq *out_irq)
{
    const struct dt_device_node *p;
    const __be32 *intspec, *tmp, *addr;
    u32 intsize, intlen;
    int res = -EINVAL;

    dt_dprintk("dt_device_get_raw_irq: dev=%s, index=%d\n",
               device->full_name, index);

    /* Get the interrupts property */
    intspec = dt_get_property(device, "interrupts", &intlen);
    if ( intspec == NULL )
        return -EINVAL;
    intlen /= sizeof(*intspec);

    dt_dprintk(" intspec=%d intlen=%d\n", be32_to_cpup(intspec), intlen);

    /* Get the reg property (if any) */
    addr = dt_get_property(device, "reg", NULL);

    /* Look for the interrupt parent. */
    p = dt_irq_find_parent(device);
    if ( p == NULL )
        return -EINVAL;

    /* Get size of interrupt specifier */
    tmp = dt_get_property(p, "#interrupt-cells", NULL);
    if ( tmp == NULL )
        goto out;
    intsize = be32_to_cpu(*tmp);

    dt_dprintk(" intsize=%d intlen=%d\n", intsize, intlen);

    /* Check index */
    if ( (index + 1) * intsize > intlen )
        goto out;

    /* Get new specifier and map it */
    res = dt_irq_map_raw(p, intspec + index * intsize, intsize,
                         addr, out_irq);
    if ( res )
        goto out;
out:
    return res;
}

int dt_irq_translate(const struct dt_raw_irq *raw,
                     struct dt_irq *out_irq)
{
    ASSERT(dt_irq_xlate != NULL);

    /* TODO: Retrieve the right irq_xlate. This is only work for the gic */

    return dt_irq_xlate(raw->specifier, raw->size,
                        &out_irq->irq, &out_irq->type);
}

int dt_device_get_irq(const struct dt_device_node *device, int index,
                      struct dt_irq *out_irq)
{
    struct dt_raw_irq raw;
    int res;

    res = dt_device_get_raw_irq(device, index, &raw);

    if ( res )
        return res;

    return dt_irq_translate(&raw, out_irq);
}

bool_t dt_device_is_available(const struct dt_device_node *device)
{
    const char *status;
    u32 statlen;

    status = dt_get_property(device, "status", &statlen);
    if ( status == NULL )
        return 1;

    if ( statlen > 0 )
    {
        if ( !strcmp(status, "okay") || !strcmp(status, "ok") )
            return 1;
    }

    return 0;
}

static int __dt_parse_phandle_with_args(const struct dt_device_node *np,
                                        const char *list_name,
                                        const char *cells_name,
                                        int cell_count, int index,
                                        struct dt_phandle_args *out_args)
{
    const __be32 *list, *list_end;
    int rc = 0, cur_index = 0;
    u32 size, count = 0;
    struct dt_device_node *node = NULL;
    dt_phandle phandle;

    /* Retrieve the phandle list property */
    list = dt_get_property(np, list_name, &size);
    if ( !list )
        return -ENOENT;
    list_end = list + size / sizeof(*list);

    /* Loop over the phandles until all the requested entry is found */
    while ( list < list_end )
    {
        rc = -EINVAL;
        count = 0;

        /*
         * If phandle is 0, then it is an empty entry with no
         * arguments.  Skip forward to the next entry.
         * */
        phandle = be32_to_cpup(list++);
        if ( phandle )
        {
            /*
             * Find the provider node and parse the #*-cells
             * property to determine the argument length.
             *
             * This is not needed if the cell count is hard-coded
             * (i.e. cells_name not set, but cell_count is set),
             * except when we're going to return the found node
             * below.
             */
            if ( cells_name || cur_index == index )
            {
                node = dt_find_node_by_phandle(phandle);
                if ( !node )
                {
                    printk(XENLOG_ERR "%s: could not find phandle\n",
                           np->full_name);
                    goto err;
                }
            }

            if ( cells_name )
            {
                if ( !dt_property_read_u32(node, cells_name, &count) )
                {
                    printk("%s: could not get %s for %s\n",
                           np->full_name, cells_name, node->full_name);
                    goto err;
                }
            }
            else
                count = cell_count;

            /*
             * Make sure that the arguments actually fit in the
             * remaining property data length
             */
            if ( list + count > list_end )
            {
                printk(XENLOG_ERR "%s: arguments longer than property\n",
                       np->full_name);
                goto err;
            }
        }

        /*
         * All of the error cases above bail out of the loop, so at
         * this point, the parsing is successful. If the requested
         * index matches, then fill the out_args structure and return,
         * or return -ENOENT for an empty entry.
         */
        rc = -ENOENT;
        if ( cur_index == index )
        {
            if (!phandle)
                goto err;

            if ( out_args )
            {
                int i;

                WARN_ON(count > MAX_PHANDLE_ARGS);
                if (count > MAX_PHANDLE_ARGS)
                    count = MAX_PHANDLE_ARGS;
                out_args->np = node;
                out_args->args_count = count;
                for ( i = 0; i < count; i++ )
                    out_args->args[i] = be32_to_cpup(list++);
            }

            /* Found it! return success */
            return 0;
        }

        node = NULL;
        list += count;
        cur_index++;
    }

    /*
     * Returning result will be one of:
     * -ENOENT : index is for empty phandle
     * -EINVAL : parsing error on data
     * [1..n]  : Number of phandle (count mode; when index = -1)
     */
    rc = index < 0 ? cur_index : -ENOENT;
err:
    return rc;
}

struct dt_device_node *dt_parse_phandle(const struct dt_device_node *np,
                                        const char *phandle_name, int index)
{
    struct dt_phandle_args args;

    if (index < 0)
        return NULL;

    if (__dt_parse_phandle_with_args(np, phandle_name, NULL, 0,
                                     index, &args))
        return NULL;

    return args.np;
}


int dt_parse_phandle_with_args(const struct dt_device_node *np,
                               const char *list_name,
                               const char *cells_name, int index,
                               struct dt_phandle_args *out_args)
{
    if ( index < 0 )
        return -EINVAL;
    return __dt_parse_phandle_with_args(np, list_name, cells_name, 0,
                                        index, out_args);
}

/**
 * unflatten_dt_node - Alloc and populate a device_node from the flat tree
 * @fdt: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @p: pointer to node in flat tree
 * @dad: Parent struct device_node
 * @allnextpp: pointer to ->allnext from last allocated device_node
 * @fpsize: Size of the node path up at the current depth.
 */
static unsigned long __init unflatten_dt_node(const void *fdt,
                                              unsigned long mem,
                                              unsigned long *p,
                                              struct dt_device_node *dad,
                                              struct dt_device_node ***allnextpp,
                                              unsigned long fpsize)
{
    struct dt_device_node *np;
    struct dt_property *pp, **prev_pp = NULL;
    char *pathp;
    u32 tag;
    unsigned int l, allocl;
    int has_name = 0;
    int new_format = 0;

    tag = be32_to_cpup((__be32 *)(*p));
    if ( tag != FDT_BEGIN_NODE )
    {
        printk(XENLOG_WARNING "Weird tag at start of node: %x\n", tag);
        return mem;
    }
    *p += 4;
    pathp = (char *)*p;
    l = allocl = strlen(pathp) + 1;
    *p = ROUNDUP(*p + l, 4);

    /* version 0x10 has a more compact unit name here instead of the full
     * path. we accumulate the full path size using "fpsize", we'll rebuild
     * it later. We detect this because the first character of the name is
     * not '/'.
     */
    if ( (*pathp) != '/' )
    {
        new_format = 1;
        if ( fpsize == 0 )
        {
            /* root node: special case. fpsize accounts for path
             * plus terminating zero. root node only has '/', so
             * fpsize should be 2, but we want to avoid the first
             * level nodes to have two '/' so we use fpsize 1 here
             */
            fpsize = 1;
            allocl = 2;
        }
        else
        {
            /* account for '/' and path size minus terminal 0
             * already in 'l'
             */
            fpsize += l;
            allocl = fpsize;
        }
    }

    np = unflatten_dt_alloc(&mem, sizeof(struct dt_device_node) + allocl,
                            __alignof__(struct dt_device_node));
    if ( allnextpp )
    {
        memset(np, 0, sizeof(*np));
        np->full_name = ((char *)np) + sizeof(struct dt_device_node);
        /* By default dom0 owns the device */
        np->used_by = 0;
        /* By default the device is not protected */
        np->is_protected = false;
        INIT_LIST_HEAD(&np->domain_list);

        if ( new_format )
        {
            char *fn = np->full_name;
            /* rebuild full path for new format */
            if ( dad && dad->parent )
            {
                strlcpy(fn, dad->full_name, allocl);
#ifdef DEBUG_DT
                if ( (strlen(fn) + l + 1) != allocl )
                {
                    dt_dprintk("%s: p: %d, l: %d, a: %d\n",
                               pathp, (int)strlen(fn),
                               l, allocl);
                }
#endif
                fn += strlen(fn);
            }
            *(fn++) = '/';
            memcpy(fn, pathp, l);
        }
        else
            memcpy(np->full_name, pathp, l);
        prev_pp = &np->properties;
        **allnextpp = np;
        *allnextpp = &np->allnext;
        if ( dad != NULL )
        {
            np->parent = dad;
            /* we temporarily use the next field as `last_child'*/
            if ( dad->next == NULL )
                dad->child = np;
            else
                dad->next->sibling = np;
            dad->next = np;
        }
    }
    /* process properties */
    while ( 1 )
    {
        u32 sz, noff;
        const char *pname;

        tag = be32_to_cpup((__be32 *)(*p));
        if ( tag == FDT_NOP )
        {
            *p += 4;
            continue;
        }
        if ( tag != FDT_PROP )
            break;
        *p += 4;
        sz = be32_to_cpup((__be32 *)(*p));
        noff = be32_to_cpup((__be32 *)((*p) + 4));
        *p += 8;
        if ( fdt_version(fdt) < 0x10 )
            *p = ROUNDUP(*p, sz >= 8 ? 8 : 4);

        pname = fdt_string(fdt, noff);
        if ( pname == NULL )
        {
            dt_dprintk("Can't find property name in list!\n");
            break;
        }
        if ( strcmp(pname, "name") == 0 )
            has_name = 1;
        l = strlen(pname) + 1;
        pp = unflatten_dt_alloc(&mem, sizeof(struct dt_property),
                                __alignof__(struct dt_property));
        if ( allnextpp )
        {
            /* We accept flattened tree phandles either in
             * ePAPR-style "phandle" properties, or the
             * legacy "linux,phandle" properties.  If both
             * appear and have different values, things
             * will get weird.  Don't do that. */
            if ( (strcmp(pname, "phandle") == 0) ||
                 (strcmp(pname, "linux,phandle") == 0) )
            {
                if ( np->phandle == 0 )
                    np->phandle = be32_to_cpup((__be32*)*p);
            }
            /* And we process the "ibm,phandle" property
             * used in pSeries dynamic device tree
             * stuff */
            if ( strcmp(pname, "ibm,phandle") == 0 )
                np->phandle = be32_to_cpup((__be32 *)*p);
            pp->name = pname;
            pp->length = sz;
            pp->value = (void *)*p;
            *prev_pp = pp;
            prev_pp = &pp->next;
        }
        *p = ROUNDUP((*p) + sz, 4);
    }
    /* with version 0x10 we may not have the name property, recreate
     * it here from the unit name if absent
     */
    if ( !has_name )
    {
        char *p1 = pathp, *ps = pathp, *pa = NULL;
        int sz;

        while ( *p1 )
        {
            if ( (*p1) == '@' )
                pa = p1;
            if ( (*p1) == '/' )
                ps = p1 + 1;
            p1++;
        }
        if ( pa < ps )
            pa = p1;
        sz = (pa - ps) + 1;
        pp = unflatten_dt_alloc(&mem, sizeof(struct dt_property) + sz,
                                __alignof__(struct dt_property));
        if ( allnextpp )
        {
            pp->name = "name";
            pp->length = sz;
            pp->value = pp + 1;
            /*
             * The device tree creation code assume that the property
             * "name" is not a fake.
             * To avoid a big divergence with Linux code, only remove
             * property link. In this case we will lose a bit of memory
             */
#if 0
            *prev_pp = pp;
            prev_pp = &pp->next;
#endif
            np->name = pp->value;
            memcpy(pp->value, ps, sz - 1);
            ((char *)pp->value)[sz - 1] = 0;
            dt_dprintk("fixed up name for %s -> %s\n", pathp,
                       (char *)pp->value);
        }
    }
    if ( allnextpp )
    {
        *prev_pp = NULL;
        np->name = (np->name) ? : dt_get_property(np, "name", NULL);
        np->type = dt_get_property(np, "device_type", NULL);

        if ( !np->name )
            np->name = "<NULL>";
        if ( !np->type )
            np->type = "<NULL>";
    }
    while ( tag == FDT_BEGIN_NODE || tag == FDT_NOP )
    {
        if ( tag == FDT_NOP )
            *p += 4;
        else
            mem = unflatten_dt_node(fdt, mem, p, np, allnextpp, fpsize);
        tag = be32_to_cpup((__be32 *)(*p));
    }
    if ( tag != FDT_END_NODE )
    {
        printk(XENLOG_WARNING "Weird tag at end of node: %x\n", tag);
        return mem;
    }

    *p += 4;
    return mem;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens a device-tree, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * @fdt: The fdt to expand
 * @mynodes: The device_node tree created by the call
 */
static void __init __unflatten_device_tree(const void *fdt,
                                           struct dt_device_node **mynodes)
{
    unsigned long start, mem, size;
    struct dt_device_node **allnextp = mynodes;

    dt_dprintk(" -> unflatten_device_tree()\n");

    dt_dprintk("Unflattening device tree:\n");
    dt_dprintk("magic: %#08x\n", fdt_magic(fdt));
    dt_dprintk("size: %#08x\n", fdt_totalsize(fdt));
    dt_dprintk("version: %#08x\n", fdt_version(fdt));

    /* First pass, scan for size */
    start = ((unsigned long)fdt) + fdt_off_dt_struct(fdt);
    size = unflatten_dt_node(fdt, 0, &start, NULL, NULL, 0);
    size = (size | 3) + 1;

    dt_dprintk("  size is %#lx allocating...\n", size);

    /* Allocate memory for the expanded device tree */
    mem = (unsigned long)_xmalloc (size + 4, __alignof__(struct dt_device_node));

    ((__be32 *)mem)[size / 4] = cpu_to_be32(0xdeadbeef);

    dt_dprintk("  unflattening %lx...\n", mem);

    /* Second pass, do actual unflattening */
    start = ((unsigned long)fdt) + fdt_off_dt_struct(fdt);
    unflatten_dt_node(fdt, mem, &start, NULL, &allnextp, 0);
    if ( be32_to_cpup((__be32 *)start) != FDT_END )
        printk(XENLOG_WARNING "Weird tag at end of tree: %08x\n",
                  *((u32 *)start));
    if ( be32_to_cpu(((__be32 *)mem)[size / 4]) != 0xdeadbeef )
        printk(XENLOG_WARNING "End of tree marker overwritten: %08x\n",
                  be32_to_cpu(((__be32 *)mem)[size / 4]));
    *allnextp = NULL;

    dt_dprintk(" <- unflatten_device_tree()\n");
}

static void dt_alias_add(struct dt_alias_prop *ap,
                         struct dt_device_node *np,
                         int id, const char *stem, int stem_len)
{
    ap->np = np;
    ap->id = id;
    strlcpy(ap->stem, stem, stem_len + 1);
    list_add_tail(&ap->link, &aliases_lookup);
    dt_dprintk("adding DT alias:%s: stem=%s id=%d node=%s\n",
               ap->alias, ap->stem, ap->id, dt_node_full_name(np));
}

/**
 * dt_alias_scan - Scan all properties of 'aliases' node
 *
 * The function scans all the properties of 'aliases' node and populate
 * the the global lookup table with the properties.  It returns the
 * number of alias_prop found, or error code in error case.
 */
static void __init dt_alias_scan(void)
{
    const struct dt_property *pp;
    const struct dt_device_node *aliases;

    aliases = dt_find_node_by_path("/aliases");
    if ( !aliases )
        return;

    dt_for_each_property_node( aliases, pp )
    {
        const char *start = pp->name;
        const char *end = start + strlen(start);
        struct dt_device_node *np;
        struct dt_alias_prop *ap;
        int id, len;

        /* Skip those we do not want to proceed */
        if ( !strcmp(pp->name, "name") ||
             !strcmp(pp->name, "phandle") ||
             !strcmp(pp->name, "linux,phandle") )
            continue;

        np = dt_find_node_by_path(pp->value);
        if ( !np )
            continue;

        /* walk the alias backwards to extract the id and work out
         * the 'stem' string */
        while ( isdigit(*(end-1)) && end > start )
            end--;
        len = end - start;

        id = simple_strtoll(end, NULL, 10);

        /* Allocate an alias_prop with enough space for the stem */
        ap = _xmalloc(sizeof(*ap) + len + 1, 4);
        if ( !ap )
            continue;
        ap->alias = start;
        dt_alias_add(ap, np, id, start, len);
    }
}

struct dt_device_node * __init
dt_find_interrupt_controller(const struct dt_device_match *matches)
{
    struct dt_device_node *np = NULL;

    while ( (np = dt_find_matching_node(np, matches)) )
    {
        if ( !dt_find_property(np, "interrupt-controller", NULL) )
            continue;

        if ( dt_get_parent(np) )
            break;
    }

    return np;
}

void __init dt_unflatten_host_device_tree(void)
{
    __unflatten_device_tree(device_tree_flattened, &dt_host);
    dt_alias_scan();
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
