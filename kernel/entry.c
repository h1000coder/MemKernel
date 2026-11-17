// MIT License
/*
 * Memory Operation driver for Linux Android
 *
 * Original author:  Jiang-Night
 * Current maintainer: Poko
 *
*/

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "resolve.h"

#define DEVICE_NAME "phmeop"

static DEFINE_MUTEX(driver_mutex);


static int dispatch_open(struct inode *node, struct file *file) {
	if (!mutex_trylock(&driver_mutex))
		return -EBUSY;
	return 0;
}

static int dispatch_close(struct inode *node, struct file *file) {
	mutex_unlock(&driver_mutex);
	return 0;
}

static long dispatch_ioctl(struct file *const file, unsigned int const cmd, unsigned long const arg)
{
	struct CopyMemory cm;
	struct ModuleBase mb;
	char name[0x100] = {0};

	switch (cmd)
	{
	case OP_READ_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0) {
			return -1;
		}
		return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, false);
	}
	case OP_WRITE_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0) {
			return -1;
		}
		return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, true);
	}
	case OP_MODULE_BASE:
	{
		if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)) != 0 || copy_from_user(name, (void __user *)mb.name, sizeof(name) - 1) != 0) {
			return -1;
		}
		mb.base = get_module_base(mb.pid, name);
		if (copy_to_user((void __user *)arg, &mb, sizeof(mb)) != 0) {
			return -1;
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static struct file_operations dispatch_functions = {
	.owner = THIS_MODULE,
	.open = dispatch_open,
	.release = dispatch_close,
	.unlocked_ioctl = dispatch_ioctl,
};

static struct miscdevice misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &dispatch_functions,
};

int __init memkernel_entry(void)
{
	int ret;

	printk("[+] memkernel_entry");

	/*
	 * Resolver símbolos problemáticos em tempo de execução via kprobes +
	 * kallsyms_lookup_name.  Falhas não-fatais são logadas como warning;
	 * cada função em memory.c tem seu próprio fallback seguro.
	 */
	ret = resolve_symbols();
	if (ret < 0) {
		pr_warn("[memkernel] symbol resolution failed (%d); some ops may not work\n", ret);
		/* Não abortamos: misc_register pode ainda funcionar parcialmente */
	}

	ret = misc_register(&misc);
	return ret;
}

void __exit memkernel_unload(void)
{
	printk("[+] memkernel_unload");
	misc_deregister(&misc);
}

module_init(memkernel_entry);
module_exit(memkernel_unload);

MODULE_DESCRIPTION("Linux Kernel.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
