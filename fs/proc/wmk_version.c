// SPDX-License-Identifier: GPL-2.0
/*
 * fs/proc/wmk_version.c - WMK custom kernel version info
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include <linux/version.h>

#define WMK_VERSION "4.14.356-openela-cip-patches"

static int wmk_version_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "Made by Zears14 \n");
	seq_puts(m, "---------------------------------------\n");
	seq_printf(m, "WMK Custom Kernel: %s\n", WMK_VERSION);
	seq_printf(m, "Base Kernel: %s\n", utsname()->release);
	seq_printf(m, "Kernel Version: %s\n", utsname()->version);
	seq_printf(m, "Architecture: %s\n", utsname()->machine);
	seq_printf(m, "Node Name: %s\n", utsname()->nodename);

#ifdef CONFIG_LOCALVERSION
	seq_printf(m, "Local Version: %s\n", CONFIG_LOCALVERSION);
#endif

	seq_puts(m, "Compiler: ");
#ifdef __clang__
	seq_puts(m, "Clang " __clang_version__);
#else
	seq_puts(m, "GCC " __VERSION__);
#endif
	seq_puts(m, "\n");

#ifdef CONFIG_ARM64
	seq_puts(m, "ARM64 Features: ");
#ifdef CONFIG_ARM64_VHE
	seq_puts(m, "VHE ");
#endif
#ifdef CONFIG_ARM64_SVE
	seq_puts(m, "SVE ");
#endif
#ifdef CONFIG_ARM64_LSE_ATOMICS
	seq_puts(m, "LSE ");
#endif
	seq_puts(m, "\n");
#endif

	return 0;
}

static int wmk_version_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, wmk_version_proc_show, NULL);
}

static const struct file_operations wmk_version_proc_fops = {
	.open = wmk_version_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init proc_wmk_version_init(void)
{
	proc_create("wmk_version", 0444, NULL, &wmk_version_proc_fops);
	return 0;
}
fs_initcall(proc_wmk_version_init);
