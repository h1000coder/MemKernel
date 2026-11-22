// MIT License
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "resolve.h"

#define DEVICE_NAME "freea"

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

static long dispatch_ioctl(struct file *const file, unsigned int const cmd,
                           unsigned long const arg)
{
    struct CopyMemory cm;
    struct ModuleBase mb;
    char name[0x100] = {0};

    switch (cmd) {
    case OP_READ_MEM:
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
            return -EFAULT;
        if (cm.size == 0 || cm.size > MEMK_MAX_RW_SIZE)
            return -EINVAL;
        if (!access_ok(cm.buffer, cm.size))
            return -EFAULT;
        return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, false);

    case OP_WRITE_MEM:
        if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
            return -EFAULT;
        if (cm.size == 0 || cm.size > MEMK_MAX_RW_SIZE)
            return -EINVAL;
        if (!access_ok(cm.buffer, cm.size))
            return -EFAULT;
        return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, true);

    case OP_MODULE_BASE:
        if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)) != 0)
            return -EFAULT;
        /* mb.name é um ponteiro userspace; validar antes de desreferenciar */
        if (!mb.name || !access_ok((void __user *)mb.name, 1))
            return -EFAULT;
        if (copy_from_user(name, (void __user *)mb.name, sizeof(name) - 1) != 0)
            return -EFAULT;
        mb.base = get_module_base(mb.pid, name);
        if (copy_to_user((void __user *)arg, &mb, sizeof(mb)) != 0)
            return -EFAULT;
        break;

    default:
        return -EINVAL;
    }
    return 0;
}

static struct file_operations dispatch_functions = {
    .owner          = THIS_MODULE,
    .open           = dispatch_open,
    .release        = dispatch_close,
    .unlocked_ioctl = dispatch_ioctl,
};

static struct miscdevice misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &dispatch_functions,
    .mode  = 0666,
};

int __init memkernel_entry(void)
{
    int ret;

    ret = resolve_symbols();
    if (ret < 0)
        pr_warn("[memkernel] symbol resolution failed (%d); some ops may not work\n", ret);

    ret = misc_register(&misc);
    if (ret)
        pr_err("[memkernel] misc_register failed: %d\n", ret);
    else
        pr_info("[memkernel] /dev/" DEVICE_NAME " registered\n");
    return ret;
}

void __exit memkernel_unload(void)
{
    misc_deregister(&misc);
    pr_info("[memkernel] unloaded\n");
}

module_init(memkernel_entry);
module_exit(memkernel_unload);

MODULE_DESCRIPTION("Linux Kernel.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
