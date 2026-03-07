/*
 * RAMLOG Early Console - Debugging tool for "Silent Death" kernels
 * Writes kernel log directly to a reserved physical memory region.
 *
 * Address: 0x4d010000 (Pstore region from LK)
 * Size:    0xe0000    (896 KB)
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <asm/early_ioremap.h>

/* Configuration derived from your Bootloader Logs */
/* 0x4d010000 + 0x50000 offset */
#define RAMLOG_PADDR 0x4d060000
#define RAMLOG_SIZE 0x40000  /* 256 KB - Max single slot size */

static void __iomem *ramlog_base;
static unsigned long ramlog_idx = 0;

/*
 * The writing function.
 * Called by printk() -> early_console->write()
 */
static void ramlog_write(struct console *con, const char *s, unsigned int n)
{
	unsigned int i;

	if (!ramlog_base)
		return;

	for (i = 0; i < n; i++) {
		/* Calculate circular buffer offset */
		/* We start at offset 4 to preserve the signature */
		unsigned long offset = 4 + (ramlog_idx % (RAMLOG_SIZE - 4));

		writeb(s[i], ramlog_base + offset);
		ramlog_idx++;
	}

	/*
	 * Write Memory Barrier:
	 * Ensures the CPU flushes the write buffer to DRAM immediately.
	 * Critical for catching crashes that happen microseconds later.
	 */
	wmb();
}

/*
 * Initialization.
 * Called when kernel parses "earlycon=ramlog"
 */
static int __init ramlog_early_setup(struct earlycon_device *device,
				     const char *options)
{
	/* Map the physical memory.
	 * early_ioremap works inside setup_arch(), very early.
	 */
	ramlog_base = early_ioremap(RAMLOG_PADDR, RAMLOG_SIZE);

	if (!ramlog_base)
		return -ENOMEM;

	/* Start each boot with a clean capture buffer. */
	memset_io(ramlog_base, 0, RAMLOG_SIZE);
	ramlog_idx = 0;

	/*
	 * Write Signature "BOOT" (0x544F4F42) at the very start.
	 * This helps us find the buffer in a hex dump later.
	 */
	writel(0x544F4F42, ramlog_base);

	/* Hook our write function into the system console */
	device->con->write = ramlog_write;

	return 0;
}

/* Register the driver so "earlycon=ramlog" works */
EARLYCON_DECLARE(ramlog, ramlog_early_setup);
