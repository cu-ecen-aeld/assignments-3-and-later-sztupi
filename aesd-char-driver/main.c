
/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include "aesdchar.h"
#include "linux/errno.h"
#include "linux/gfp.h"
#include "linux/minmax.h"
#include "linux/mutex.h"
#include "linux/string.h"
#include "aesd-circular-buffer.h"
#include "linux/uaccess.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Gabor Attila Sztupak");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");

	struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
	filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");

	struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
	filp->private_data = dev;

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    char *target = buf;
    size_t offset;
    struct aesd_buffer_entry *entry;

    while (retval < count) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->circ_buffer, *f_pos, &offset);

        if (!entry) goto out;

        ssize_t copied = copy_to_user(
            target,
            entry->buffptr+offset,
            min(count-retval, entry->size-offset));

        target = target + copied;
        *f_pos += copied;
        retval += copied;
    }

  out : mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    char *kbuf = (char *)krealloc(dev->unterminated,
                                  dev->unterminated_count+count,
                                  GFP_KERNEL);
    if (!kbuf) {
        retval = -ENOMEM;
        goto out;
    }
    if (copy_from_user(dev->unterminated+dev->unterminated_count, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    dev->unterminated_count += count;

    size_t idx = 0;
    while (idx < dev->unterminated_count) {
        if (dev->unterminated[idx] != '\n') {
            idx ++;
            continue;
        }

        struct aesd_buffer_entry entry;

        char *buff = kmalloc(idx+1, GFP_KERNEL);
        if (!buff) {
            retval = -ENOMEM;
            goto out;
        }
        strncpy(buff, dev->unterminated, idx+1);
        entry.buffptr = buff;

        if (dev->circ_buffer.full) {
            kfree(dev->circ_buffer.entry[dev->circ_buffer.out_offs].buffptr);
        }

        aesd_circular_buffer_add_entry(&dev->circ_buffer, &entry);

        if (dev->unterminated_count > idx+1) {
            char * unterminated = kmalloc(dev->unterminated_count - idx - 1,
                                          GFP_KERNEL);
            if (!unterminated) {
                retval = -ENOMEM;
                goto out;
            }

            strncpy(unterminated, dev->unterminated+idx+1, dev->unterminated_count-idx-1);
            kfree(dev->unterminated);
            dev->unterminated = unterminated;
            dev->unterminated_count = dev->unterminated_count-idx-1;
            idx = 0;
        } else {
            kfree(dev->unterminated);
            dev->unterminated = NULL;
            dev->unterminated_count = 0;
            idx = 0;
        }
    }

  out:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    mutex_init(&aesd_device.lock);
    aesd_device.unterminated = NULL;
    aesd_device.unterminated_count = 0;
    aesd_circular_buffer_init(&aesd_device.circ_buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    kfree(aesd_device.unterminated);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
