/*
 * Flash memory access on SA11x0 based devices
 * 
 * (C) 2000 Nicolas Pitre <nico@cam.org>
 * 
 * $Id: sa1100-flash.c,v 1.47 2004/11/01 13:44:36 rmk Exp $
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/device.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/concat.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/io.h>
#include <asm/sizes.h>

#include <asm/arch/h3600.h>

#ifndef CONFIG_ARCH_SA1100
#error This is for SA1100 architecture only
#endif

/*
 * This isnt complete yet, so...
 */
#define CONFIG_MTD_SA1100_STATICMAP 1

#ifdef CONFIG_MTD_SA1100_STATICMAP
/*
 * Here are partition information for all known SA1100-based devices.
 * See include/linux/mtd/partitions.h for definition of the mtd_partition
 * structure.
 *
 * Please note:
 *  1. We no longer support static flash mappings via the machine io_desc
 *     structure.
 *  2. The flash size given should be the largest flash size that can
 *     be accommodated.
 *
 * The MTD layer will detect flash chip aliasing and reduce the size of
 * the map accordingly.
 *
 * Please keep these in alphabetical order, and formatted as per existing
 * entries.  Thanks.
 */

#ifdef CONFIG_SA1100_ADSBITSY
static struct mtd_partition adsbitsy_partitions[] = {
	{
		.name		= "bootROM",
		.size		= 0x80000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "zImage",
		.size		= 0x100000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "ramdisk.gz",
		.size		= 0x300000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "User FS",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_ASSABET
/* Phase 4 Assabet has two 28F160B3 flash parts in bank 0: */
static struct mtd_partition assabet4_partitions[] = {
	{
		.name		= "bootloader",
		.size		= 0x00020000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "bootloader params",
		.size		= 0x00020000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "jffs",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};

/* Phase 5 Assabet has two 28F128J3A flash parts in bank 0: */
static struct mtd_partition assabet5_partitions[] = {
	{
		.name		= "bootloader",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "bootloader params",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "jffs",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};

#define assabet_partitions	assabet5_partitions
#endif

#ifdef CONFIG_SA1100_BADGE4
/*
 * 1 x Intel 28F320C3 Advanced+ Boot Block Flash (32 Mi bit)
 *   Eight 4 KiW Parameter Bottom Blocks (64 KiB)
 *   Sixty-three 32 KiW Main Blocks (4032 Ki b)
 *
 * <or>
 *
 * 1 x Intel 28F640C3 Advanced+ Boot Block Flash (64 Mi bit)
 *   Eight 4 KiW Parameter Bottom Blocks (64 KiB)
 *   One-hundred-twenty-seven 32 KiW Main Blocks (8128 Ki b)
 */
static struct mtd_partition badge4_partitions[] = {
	{
		.name		= "BLOB boot loader",
		.offset		= 0,
		.size		= 0x0000A000
	}, {
		.name		= "params",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 0x00006000
	}, {
		.name		= "root",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL
	}
};
#endif


#ifdef CONFIG_SA1100_CERF
#ifdef CONFIG_SA1100_CERF_FLASH_32MB
#  define CERF_FLASH_SIZE	0x02000000
#elif defined CONFIG_SA1100_CERF_FLASH_16MB
#  define CERF_FLASH_SIZE	0x01000000
#elif defined CONFIG_SA1100_CERF_FLASH_8MB
#  define CERF_FLASH_SIZE	0x00800000
#else
#  error "Undefined flash size for CERF in sa1100-flash.c"
#endif

static struct mtd_partition cerf_partitions[] = {
	{
		.name		= "Bootloader",
		.size		= 0x00020000,
		.offset		= 0x00000000,
	}, {
		.name		= "Params",
		.size		= 0x00040000,
		.offset		= 0x00020000,
	}, {
		.name		= "Kernel",
		.size		= 0x00100000,
		.offset		= 0x00060000,
	}, {
		.name		= "Filesystem",
		.size		= CERF_FLASH_SIZE-0x00160000,
		.offset		= 0x00160000,
	}
};
#endif

#ifdef CONFIG_SA1100_CONSUS
static struct mtd_partition consus_partitions[] = {
	{
		.name		= "Consus boot firmware",
		.offset		= 0,
		.size		= 0x00040000,
		.mask_flags	= MTD_WRITABLE, /* force read-only */
	}, {
		.name		= "Consus kernel",
		.offset		= 0x00040000,
		.size		= 0x00100000,
		.mask_flags	= 0,
	}, {
		.name		= "Consus disk",
		.offset		= 0x00140000,
		/* The rest (up to 16M) for jffs.  We could put 0 and
		   make it find the size automatically, but right now
		   i have 32 megs.  jffs will use all 32 megs if given
		   the chance, and this leads to horrible problems
		   when you try to re-flash the image because blob
		   won't erase the whole partition. */
		.size		= 0x01000000 - 0x00140000,
		.mask_flags	= 0,
	}, {
		/* this disk is a secondary disk, which can be used as
		   needed, for simplicity, make it the size of the other
		   consus partition, although realistically it could be
		   the remainder of the disk (depending on the file
		   system used) */
		 .name		= "Consus disk2",
		 .offset	= 0x01000000,
		 .size		= 0x01000000 - 0x00140000,
		 .mask_flags	= 0,
	}
};
#endif

#ifdef CONFIG_SA1100_FLEXANET
/* Flexanet has two 28F128J3A flash parts in bank 0: */
#define FLEXANET_FLASH_SIZE		0x02000000
static struct mtd_partition flexanet_partitions[] = {
	{
		.name		= "bootloader",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "bootloader params",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "kernel",
		.size		= 0x000C0000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "altkernel",
		.size		= 0x000C0000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "root",
		.size		= 0x00400000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "free1",
		.size		= 0x00300000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "free2",
		.size		= 0x00300000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "free3",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,
	}
};
#endif

#ifdef CONFIG_SA1100_FREEBIRD
static struct mtd_partition freebird_partitions[] = {
#ifdef CONFIG_SA1100_FREEBIRD_NEW
	{
		.name		= "firmware",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "kernel",
		.size		= 0x00080000,
		.offset		= 0x00040000,
	}, {
		.name		= "params",
		.size		= 0x00040000,
		.offset		= 0x000C0000,
	}, {
		.name		= "initrd",
		.size		= 0x00100000,
		.offset		= 0x00100000,
	}, {
		.name		= "root cramfs",
		.size		= 0x00300000,
		.offset		= 0x00200000,
	}, {
		.name		= "usr cramfs",
		.size		= 0x00C00000,
		.offset		= 0x00500000,
	}, {
		.name		= "local",
		.size		= MTDPART_SIZ_FULL,
		.offset		= 0x01100000,
	}
#else
	{
		.size		= 0x00040000,
		.offset		= 0,
	}, {
		.size		= 0x000c0000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.size		= 0x00400000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
#endif
};
#endif

#ifdef CONFIG_SA1100_FRODO
/* Frodo has 2 x 16M 28F128J3A flash chips in bank 0: */
static struct mtd_partition frodo_partitions[] =
{
	{
		.name		= "bootloader",
		.size		= 0x00040000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE
	}, {
		.name		= "bootloader params",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE
	}, {
		.name		= "kernel",
		.size		= 0x00100000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE
	}, {
		.name		= "ramdisk",
		.size		= 0x00400000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE
	}, {
		.name		= "file system",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND
	}
};
#endif

#ifdef CONFIG_SA1100_GRAPHICSCLIENT
static struct mtd_partition graphicsclient_partitions[] = {
	{
		.name		= "zImage",
		.size		= 0x100000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "ramdisk.gz",
		.size		= 0x300000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "User FS",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_GRAPHICSMASTER
static struct mtd_partition graphicsmaster_partitions[] = {
	{
		.name		= "zImage",
		.size		= 0x100000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	},
	{
		.name		= "ramdisk.gz",
		.size		= 0x300000,
		.offset		= MTDPART_OFS_APPEND,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	},
	{
		.name		= "User FS",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_H3XXX
static struct mtd_partition h3xxx_partitions[] = {
	{
		.name		= "H3XXX boot firmware",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
#ifdef CONFIG_MTD_2PARTS_IPAQ
		.name		= "H3XXX root jffs2",
		.size		= MTDPART_SIZ_FULL,
		.offset		= 0x00040000,
#else
		.name		= "H3XXX kernel",
		.size		= 0x00080000,
		.offset		= 0x00040000,
	}, {
		.name		= "H3XXX params",
		.size		= 0x00040000,
		.offset		= 0x000C0000,
	}, {
#ifdef CONFIG_JFFS2_FS
		.name		= "H3XXX root jffs2",
		.size		= MTDPART_SIZ_FULL,
		.offset		= 0x00100000,
#else
		.name		= "H3XXX initrd",
		.size		= 0x00100000,
		.offset		= 0x00100000,
	}, {
		.name		= "H3XXX root cramfs",
		.size		= 0x00300000,
		.offset		= 0x00200000,
	}, {
		.name		= "H3XXX usr cramfs",
		.size		= 0x00800000,
		.offset		= 0x00500000,
	}, {
		.name		= "H3XXX usr local",
		.size		= MTDPART_SIZ_FULL,
		.offset		= 0x00d00000,
#endif
#endif
	}
};

static void h3xxx_set_vpp(struct map_info *map, int vpp)
{
	assign_h3600_egpio(IPAQ_EGPIO_VPP_ON, vpp);
}
#else
#define h3xxx_set_vpp NULL
#endif

#ifdef CONFIG_SA1100_HACKKIT
static struct mtd_partition hackkit_partitions[] = {
	{
		.name		= "BLOB",
		.size		= 0x00040000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "config",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "kernel",
		.size		= 0x00100000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "initrd",
		.size		= 0x00180000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "rootfs",
		.size		= 0x700000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "data",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_HUW_WEBPANEL
static struct mtd_partition huw_webpanel_partitions[] = {
	{
		.name		= "Loader",
		.size		= 0x00040000,
		.offset		= 0,
	}, {
		.name		= "Sector 1",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_JORNADA56X
static struct mtd_partition jornada56x_partitions[] = {
	{
		.name		= "bootldr",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,
	}, {
		.name		= "rootfs",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};

static void jornada56x_set_vpp(struct map_info *map, int vpp)
{
	if (vpp)
		GPSR = GPIO_GPIO26;
	else
		GPCR = GPIO_GPIO26;
	GPDR |= GPIO_GPIO26;
}
#else
#define jornada56x_set_vpp NULL
#endif

#ifdef CONFIG_SA1100_JORNADA720
static struct mtd_partition jornada720_partitions[] = {
	{
		.name		= "JORNADA720 boot firmware",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "JORNADA720 kernel",
		.size		= 0x000c0000,
		.offset		= 0x00040000,
	}, {
		.name		= "JORNADA720 params",
		.size		= 0x00040000,
		.offset		= 0x00100000,
	}, {
		.name		= "JORNADA720 initrd",
		.size		= 0x00100000,
		.offset		= 0x00140000,
	}, {
		.name		= "JORNADA720 root cramfs",
		.size		= 0x00300000,
		.offset		= 0x00240000,
	}, {
		.name		= "JORNADA720 usr cramfs",
		.size		= 0x00800000,
		.offset		= 0x00540000,
	}, {
		.name		= "JORNADA720 usr local",
		.size		= 0,  /* will expand to the end of the flash */
		.offset		= 0x00d00000,
	}
};

static void jornada720_set_vpp(struct map_info *map, int vpp)
{
	if (vpp)
		PPSR |= 0x80;
	else
		PPSR &= ~0x80;
	PPDR |= 0x80;
}
#else
#define jornada720_set_vpp NULL
#endif

#ifdef CONFIG_SA1100_PANGOLIN
static struct mtd_partition pangolin_partitions[] = {
	{
		.name		= "boot firmware",
		.size		= 0x00080000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "kernel",
		.size		= 0x00100000,
		.offset		= 0x00080000,
	}, {
		.name		= "initrd",
		.size		= 0x00280000,
		.offset		= 0x00180000,
	}, {
		.name		= "initrd-test",
		.size		= 0x03C00000,
		.offset		= 0x00400000,
	}
};
#endif

#ifdef CONFIG_SA1100_PT_SYSTEM3
/* erase size is 0x40000 == 256k partitions have to have this boundary */
static struct mtd_partition system3_partitions[] = {
	{
		.name		= "BLOB",
		.size		= 0x00040000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "config",
		.size		= 0x00040000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "kernel",
		.size		= 0x00100000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "root",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_SHANNON
static struct mtd_partition shannon_partitions[] = {
	{
		.name		= "BLOB boot loader",
		.offset		= 0,
		.size		= 0x20000
	},
	{
		.name		= "kernel",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 0xe0000
	},
	{ 
		.name		= "initrd",
		.offset		= MTDPART_OFS_APPEND,	
		.size		= MTDPART_SIZ_FULL
	}
};

#endif

#ifdef CONFIG_SA1100_SHERMAN
static struct mtd_partition sherman_partitions[] = {
	{
		.size		= 0x50000,
		.offset		= 0,
	}, {
		.size		= 0x70000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.size		= 0x600000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.size		= 0xA0000,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_SIMPAD
static struct mtd_partition simpad_partitions[] = {
	{
		.name		= "SIMpad boot firmware",
		.size		= 0x00080000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "SIMpad kernel",
		.size		= 0x00100000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
#ifdef CONFIG_ROOT_CRAMFS
		.name		= "SIMpad root cramfs",
		.size	     	=0x00D80000,
		.offset		= MTDPART_OFS_APPEND

	}, {
		.name		= "SIMpad local jffs2",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND
#else
		.name		= "SIMpad root jffs2",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND
#endif
	}
};
#endif /* CONFIG_SA1100_SIMPAD */

#ifdef CONFIG_SA1100_STORK
static struct mtd_partition stork_partitions[] = {
	{
		.name		= "STORK boot firmware",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "STORK params",
		.size		= 0x00040000,
		.offset		= 0x00040000,
	}, {
		.name		= "STORK kernel",
		.size		= 0x00100000,
		.offset		= 0x00080000,
	}, {
#ifdef CONFIG_JFFS2_FS
		.name		= "STORK root jffs2",
		.offset		= 0x00180000,
		.size		= MTDPART_SIZ_FULL,
#else
		.name		= "STORK initrd",
		.size		= 0x00100000,
		.offset		= 0x00180000,
	}, {
		.name		= "STORK root cramfs",
		.size		= 0x00300000,
		.offset		= 0x00280000,
	}, {
		.name		= "STORK usr cramfs",
		.size		= 0x00800000,
		.offset		= 0x00580000,
	}, {
		.name		= "STORK usr local",
		.offset		= 0x00d80000,
		.size		= MTDPART_SIZ_FULL,
#endif
	}
};
#endif

#ifdef CONFIG_SA1100_TRIZEPS
static struct mtd_partition trizeps_partitions[] = {
	{
		.name		= "Bootloader",
		.size		= 0x00100000,
		.offset		= 0,
	}, {
		.name		= "Kernel",
		.size		= 0x00100000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "root",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};
#endif

#ifdef CONFIG_SA1100_YOPY
static struct mtd_partition yopy_partitions[] = {
	{
		.name		= "boot firmware",
		.size		= 0x00040000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "kernel",
		.size		= 0x00080000,
		.offset		= 0x00080000,
	}, {
		.name		= "initrd",
		.size		= 0x00300000,
		.offset		= 0x00100000,
	}, {
		.name		= "root",
		.size		= 0x01000000,
		.offset		= 0x00400000,
	}
};
#endif

static int __init sa1100_static_partitions(struct mtd_partition **parts)
{
	int nb_parts = 0;

#ifdef CONFIG_SA1100_ADSBITSY
	if (machine_is_adsbitsy()) {
		*parts       = adsbitsy_partitions;
		nb_parts     = ARRAY_SIZE(adsbitsy_partitions);
	}
#endif
#ifdef CONFIG_SA1100_ASSABET
	if (machine_is_assabet()) {
		*parts       = assabet_partitions;
		nb_parts     = ARRAY_SIZE(assabet_partitions);
	}
#endif
#ifdef CONFIG_SA1100_BADGE4
	if (machine_is_badge4()) {
		*parts       = badge4_partitions;
		nb_parts     = ARRAY_SIZE(badge4_partitions);
	}
#endif
#ifdef CONFIG_SA1100_CERF
	if (machine_is_cerf()) {
		*parts       = cerf_partitions;
		nb_parts     = ARRAY_SIZE(cerf_partitions);
	}
#endif
#ifdef CONFIG_SA1100_CONSUS
	if (machine_is_consus()) {
		*parts       = consus_partitions;
		nb_parts     = ARRAY_SIZE(consus_partitions);
	}
#endif
#ifdef CONFIG_SA1100_FLEXANET
	if (machine_is_flexanet()) {
		*parts       = flexanet_partitions;
		nb_parts     = ARRAY_SIZE(flexanet_partitions);
	}
#endif
#ifdef CONFIG_SA1100_FREEBIRD
	if (machine_is_freebird()) {
		*parts       = freebird_partitions;
		nb_parts     = ARRAY_SIZE(freebird_partitions);
	}
#endif
#ifdef CONFIG_SA1100_FRODO
	if (machine_is_frodo()) {
		*parts       = frodo_partitions;
		nb_parts     = ARRAY_SIZE(frodo_partitions);
	}
#endif	
#ifdef CONFIG_SA1100_GRAPHICSCLIENT
	if (machine_is_graphicsclient()) {
		*parts       = graphicsclient_partitions;
		nb_parts     = ARRAY_SIZE(graphicsclient_partitions);
	}
#endif
#ifdef CONFIG_SA1100_GRAPHICSMASTER
	if (machine_is_graphicsmaster()) {
		*parts       = graphicsmaster_partitions;
		nb_parts     = ARRAY_SIZE(graphicsmaster_partitions);
	}
#endif
#ifdef CONFIG_SA1100_H3XXX
	if (machine_is_h3xxx()) {
		*parts       = h3xxx_partitions;
		nb_parts     = ARRAY_SIZE(h3xxx_partitions);
	}
#endif
#ifdef CONFIG_SA1100_HACKKIT
	if (machine_is_hackkit()) {
		*parts = hackkit_partitions;
		nb_parts = ARRAY_SIZE(hackkit_partitions);
	}
#endif
#ifdef CONFIG_SA1100_HUW_WEBPANEL
	if (machine_is_huw_webpanel()) {
		*parts       = huw_webpanel_partitions;
		nb_parts     = ARRAY_SIZE(huw_webpanel_partitions);
	}
#endif
#ifdef CONFIG_SA1100_JORNADA56X
	if (machine_is_jornada56x()) {
		*parts       = jornada56x_partitions;
		nb_parts     = ARRAY_SIZE(jornada56x_partitions);
	}
#endif
#ifdef CONFIG_SA1100_JORNADA720
	if (machine_is_jornada720()) {
		*parts       = jornada720_partitions;
		nb_parts     = ARRAY_SIZE(jornada720_partitions);
	}
#endif
#ifdef CONFIG_SA1100_PANGOLIN
	if (machine_is_pangolin()) {
		*parts       = pangolin_partitions;
		nb_parts     = ARRAY_SIZE(pangolin_partitions);
	}
#endif
#ifdef CONFIG_SA1100_PT_SYSTEM3
	if (machine_is_pt_system3()) {
		*parts       = system3_partitions;
		nb_parts     = ARRAY_SIZE(system3_partitions);
	}
#endif
#ifdef CONFIG_SA1100_SHANNON
	if (machine_is_shannon()) {
		*parts       = shannon_partitions;
		nb_parts     = ARRAY_SIZE(shannon_partitions);
	}
#endif
#ifdef CONFIG_SA1100_SHERMAN
	if (machine_is_sherman()) {
		*parts       = sherman_partitions;
		nb_parts     = ARRAY_SIZE(sherman_partitions);
	}
#endif
#ifdef CONFIG_SA1100_SIMPAD
	if (machine_is_simpad()) {
		*parts       = simpad_partitions;
		nb_parts     = ARRAY_SIZE(simpad_partitions);
	}
#endif
#ifdef CONFIG_SA1100_STORK
	if (machine_is_stork()) {
		*parts       = stork_partitions;
		nb_parts     = ARRAY_SIZE(stork_partitions);
	}
#endif
#ifdef CONFIG_SA1100_TRIZEPS
	if (machine_is_trizeps()) {
		*parts       = trizeps_partitions;
		nb_parts     = ARRAY_SIZE(trizeps_partitions);
	}
#endif
#ifdef CONFIG_SA1100_YOPY
	if (machine_is_yopy()) {
		*parts       = yopy_partitions;
		nb_parts     = ARRAY_SIZE(yopy_partitions);
	}
#endif

	return nb_parts;
}
#endif

struct sa_subdev_info {
	unsigned long base;
	unsigned long size;
	int width;
	void (*set_vpp)(struct map_info *, int);
	char name[16];
	struct map_info map;
	struct mtd_info *mtd;
};

#define NR_SUBMTD 4

struct sa_info {
	struct mtd_partition	*parts;
	struct mtd_info		*mtd;
	int			num_subdev;
	struct sa_subdev_info	subdev[NR_SUBMTD];
};

static void sa1100_destroy_subdev(struct sa_subdev_info *subdev)
{
	if (subdev->mtd)
		map_destroy(subdev->mtd);
	if (subdev->map.virt)
		iounmap(subdev->map.virt);
	release_mem_region(subdev->base, subdev->size);
}

static int sa1100_probe_subdev(struct sa_subdev_info *subdev)
{
	unsigned long phys;
	unsigned int size;
	int ret;

	phys = subdev->base;
	size = subdev->size;

	/*
	 * Retrieve the bankwidth from the MSC registers.
	 * We currently only implement CS0 and CS1 here.
	 */
	switch (phys) {
	default:
		printk(KERN_WARNING "SA1100 flash: unknown base address "
		       "0x%08lx, assuming CS0\n", phys);

	case SA1100_CS0_PHYS:
		subdev->map.bankwidth = (MSC0 & MSC_RBW) ? 2 : 4;
		break;

	case SA1100_CS1_PHYS:
		subdev->map.bankwidth = ((MSC0 >> 16) & MSC_RBW) ? 2 : 4;
		break;
	}

	if (!request_mem_region(phys, size, subdev->name)) {
		ret = -EBUSY;
		goto out;
	}

	subdev->map.set_vpp = subdev->set_vpp;
	subdev->map.phys = phys;
	subdev->map.size = size;
	subdev->map.virt = ioremap(phys, size);
	if (!subdev->map.virt) {
		ret = -ENOMEM;
		goto err;
	}

	simple_map_init(&subdev->map);

	/*
	 * Now let's probe for the actual flash.  Do it here since
	 * specific machine settings might have been set above.
	 */
	subdev->mtd = do_map_probe(subdev->data->map_name, &subdev->map);
	if (subdev->mtd == NULL) {
		ret = -ENXIO;
		goto err;
	}
	subdev->mtd->owner = THIS_MODULE;

	printk(KERN_INFO "SA1100 flash: CFI device at 0x%08lx, %dMiB, "
		"%d-bit\n", phys, subdev->mtd->size >> 20,
		subdev->map.bankwidth * 8);

	return 0;

 err:
	sa1100_destroy_subdev(subdev);
 out:
	return ret;
}

static void sa1100_destroy(struct sa_info *info)
{
	int i;

	if (info->mtd) {
		del_mtd_partitions(info->mtd);

#ifdef CONFIG_MTD_CONCAT
		if (info->mtd != info->subdev[0].mtd)
			mtd_concat_destroy(info->mtd);
#endif
	}

	if (info->parts)
		kfree(info->parts);

	for (i = info->num_subdev - 1; i >= 0; i--)
		sa1100_destroy_subdev(&info->subdev[i]);
}

static int __init sa1100_setup_mtd(struct sa_info *info, int nr)
{
	struct mtd_info *cdev[nr];
	int i, ret = 0;

	/*
	 * Claim and then map the memory regions.
	 */
	for (i = 0; i < nr; i++) {
		struct sa_subdev_info *subdev = &info->subdev[i];
		if (subdev->base == (unsigned long)-1)
			break;

		subdev->map.name = subdev->name;
		sprintf(subdev->name, "sa1100-%d", i);

		ret = sa1100_probe_subdev(subdev);
		if (ret)
			break;

		cdev[i] = subdev->mtd;
	}

	info->num_subdev = i;

	/*
	 * ENXIO is special.  It means we didn't find a chip when we probed.
	 */
	if (ret != 0 && !(ret == -ENXIO && info->num_subdev > 0))
		goto err;

	/*
	 * If we found one device, don't bother with concat support.  If
	 * we found multiple devices, use concat if we have it available,
	 * otherwise fail.  Either way, it'll be called "sa1100".
	 */
	if (info->num_subdev == 1) {
		strcpy(info->subdev[0].name, "sa1100");
		info->mtd = info->subdev[0].mtd;
		ret = 0;
	} else if (info->num_subdev > 1) {
		/*
		 * We detected multiple devices.  Concatenate them together.
		 */
#ifdef CONFIG_MTD_CONCAT
		info->mtd = mtd_concat_create(cdev, info->num_subdev,
					      "sa1100");
		if (info->mtd == NULL)
			ret = -ENXIO;
#else
		printk(KERN_ERR "SA1100 flash: multiple devices "
		       "found but MTD concat support disabled.\n");
		ret = -ENXIO;
#endif
	}

	if (ret == 0)
		return 0;

 err:
	sa1100_destroy(info);
	return ret;
}

static int __init sa1100_locate_flash(struct sa_info *info)
{
	int i, nr = -ENODEV;

	if (machine_is_adsbitsy()) {
		info[0].base = SA1100_CS1_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_assabet()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		info[1].base = SA1100_CS1_PHYS; /* neponset */
		info[1].size = SZ_32M;
		nr = 2;
	}
	if (machine_is_badge4()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_64M;
		nr = 1;
	}
	if (machine_is_cerf()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_consus()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_flexanet()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_freebird()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_frodo()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_graphicsclient()) {
		info[0].base = SA1100_CS1_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_graphicsmaster()) {
		info[0].base = SA1100_CS1_PHYS;
		info[0].size = SZ_16M;
		nr = 1;
	}
	if (machine_is_h3xxx()) {
		info[0].set_vpp = h3xxx_set_vpp;
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_huw_webpanel()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_16M;
		nr = 1;
	}
	if (machine_is_itsy()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_jornada56x()) {
		info[0].set_vpp = jornada56x_set_vpp;
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_jornada720()) {
		info[0].set_vpp = jornada720_set_vpp;
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_nanoengine()) {
		info[0].base = SA1100_CS0_PHYS;
		info[1].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_pangolin()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_64M;
		nr = 1;
	}
	if (machine_is_pfs168()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_pleb()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_4M;
		info[1].base = SA1100_CS1_PHYS;
		info[1].size = SZ_4M;
		nr = 2;
	}
	if (machine_is_pt_system3()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_16M;
		nr = 1;
	}
	if (machine_is_shannon()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_4M;
		nr = 1;
	}
	if (machine_is_sherman()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_simpad()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_16M;
		info[1].base = SA1100_CS1_PHYS;
		info[1].size = SZ_16M;
		nr = 2;
	}
	if (machine_is_stork()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_32M;
		nr = 1;
	}
	if (machine_is_trizeps()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_16M;
		nr = 1;
	}
	if (machine_is_victor()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_2M;
		nr = 1;
	}
	if (machine_is_yopy()) {
		info[0].base = SA1100_CS0_PHYS;
		info[0].size = SZ_64M;
		info[1].base = SA1100_CS1_PHYS;
		info[1].size = SZ_64M;
		nr = 2;
	}

	if (nr < 0)
		return nr;

	/*
	 * Retrieve the bankwidth from the MSC registers.
	 * We currently only implement CS0 and CS1 here.
	 */
	for (i = 0; i < nr; i++) {
		switch (info[i].base) {
		default:
			printk(KERN_WARNING "SA1100 flash: unknown base address "
				"0x%08lx, assuming CS0\n", info[i].base);
		case SA1100_CS0_PHYS:
			info[i].width = (MSC0 & MSC_RBW) ? 2 : 4;
			break;

		case SA1100_CS1_PHYS:
			info[i].width = ((MSC0 >> 16) & MSC_RBW) ? 2 : 4;
			break;
		}
	}

	return nr;
}

const char *part_probes[] = { "cmdlinepart", "RedBoot", NULL };

static void __init sa1100_locate_partitions(struct sa_info *info)
{
	struct mtd_partition *parts;
	const char *part_type = NULL;
	int nr_parts = 0;

	/*
	 * Partition selection stuff.
	 */
#ifdef CONFIG_MTD_PARTITIONS
	nr_parts = parse_mtd_partitions(info->mtd, part_probes, &parts, 0);
	if (nr_parts > 0) {
		info->parts = parts;
		part_type = "dynamic";
	} else
#endif
	{
		nr_parts = sa1100_static_partitions(&parts);
		part_type = "static";
	}

	if (nr_parts == 0) {
		printk(KERN_NOTICE "SA1100 flash: no partition info "
			"available, registering whole flash\n");
		add_mtd_device(info->mtd);
	} else {
		printk(KERN_NOTICE "SA1100 flash: using %s partition "
			"definition\n", part_type);
		add_mtd_partitions(info->mtd, parts, nr_parts);
	}

	/* Always succeeds. */
}

static struct sa_info sa_info;

static int __init sa1100_mtd_probe(struct device *dev)
{
	int err;
	int nr;

	nr = sa1100_locate_flash(&sa_info);
	if (nr < 0)
		return nr;

	err = sa1100_setup_mtd(&sa_info, nr);
	if (err == 0)
		sa1100_locate_partitions(&sa_info);

	return err;
}

static int __exit sa1100_mtd_remove(struct device *dev)
{
	sa1100_destroy(&sa_info);
	return 0;
}

#ifdef CONFIG_PM
static int sa1100_mtd_suspend(struct device *dev, u32 state, u32 level)
{
}

static int sa1100_mtd_resume(struct device *dev, u32 level)
{
}
#else
#define sa1100_mtd_suspend NULL
#define sa1100_mtd_resume  NULL
#endif

static struct device_driver sa1100_mtd_driver = {
	.name		= "flash",
	.bus		= &platform_bus_type,
	.probe		= sa1100_mtd_probe,
	.remove		= __exit_p(sa1100_mtd_remove),
	.suspend	= sa1100_mtd_suspend,
	.resume		= sa1100_mtd_resume,
};

static int __init sa1100_mtd_init(void)
{
	return driver_register(&sa1100_mtd_driver);
}

static void __exit sa1100_mtd_exit(void)
{
	driver_unregister(&sa1100_mtd_driver);
}

module_init(sa1100_mtd_init);
module_exit(sa1100_mtd_exit);

MODULE_AUTHOR("Nicolas Pitre");
MODULE_DESCRIPTION("SA1100 CFI map driver");
MODULE_LICENSE("GPL");
