
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
#include "aesd_ioctl.h"
#include "access_ok_version.h"

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

        if (!entry) {
            PDEBUG("couldn't find an entry at offset %lld", *f_pos);
            goto out;
        }

        ssize_t copy_size = min(count-retval, entry->size-offset);
        ssize_t not_copied = copy_to_user(
            target,
            entry->buffptr+offset,
            copy_size);

        ssize_t copied = copy_size - not_copied;

        target = target + copied;
        *f_pos += copied;
        retval += copied;

        if (!not_copied) {
            PDEBUG("couldn't copy %d out of %d bytes to user", not_copied, copy_size);
            goto out;
        }

    }

  out : mutex_unlock(&dev->lock);
    PDEBUG("finished reading %zu bytes",retval);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    PDEBUG("trying to allocate buffer for input data size %d", dev->unterminated_count+count);
    char *kbuf = (char *)krealloc(dev->unterminated,
                                  dev->unterminated_count+count,
                                  GFP_KERNEL);
    if (!kbuf) {
        PDEBUG("failed to allocate buffer for input data size %d", dev->unterminated_count+count);
        retval = -ENOMEM;
        goto out;
    }
    dev->unterminated = kbuf;

    PDEBUG("trying to copy data from user");
    if (copy_from_user(dev->unterminated+dev->unterminated_count, buf, count)) {
        PDEBUG("failed to copy data from user");
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

        PDEBUG("trying to allocate buffer for user data of size %d", idx+1);
        char *buff = kmalloc(idx+1, GFP_KERNEL);
        if (!buff) {
            PDEBUG("failed to allocate buffer for user data of size %d", idx+1);
            retval = -ENOMEM;
            goto out;
        }
        strncpy(buff, dev->unterminated, idx+1);
        entry.buffptr = buff;
        entry.size = idx+1;

        if (dev->circ_buffer.full) {
            PDEBUG("freeing up overridden buffer entry");
            kfree(dev->circ_buffer.entry[dev->circ_buffer.out_offs].buffptr);
        }

        PDEBUG("adding buffer entry of size %d", entry.size);
        aesd_circular_buffer_add_entry(&dev->circ_buffer, &entry);

        if (dev->unterminated_count > idx+1) {
            PDEBUG("reducing unterminated buffer");
            char * unterminated = kmalloc(dev->unterminated_count - idx - 1,
                                          GFP_KERNEL);
            PDEBUG("trying to allocate buffer for remaining data of size %d",
                   dev->unterminated_count - idx - 1);
            if (!unterminated) {
                PDEBUG("failed to allocate buffer for remaining data of size %d",
                       dev->unterminated_count - idx - 1);
                retval = -ENOMEM;
                goto out;
            }

            strncpy(unterminated, dev->unterminated+idx+1, dev->unterminated_count-idx-1);
            kfree(dev->unterminated);
            dev->unterminated = unterminated;
            dev->unterminated_count = dev->unterminated_count-idx-1;
            idx = 0;
        } else {
            PDEBUG("clearing unterminated");
            kfree(dev->unterminated);
            dev->unterminated = NULL;
            dev->unterminated_count = 0;
            idx = 0;
        }
    }
    retval = count;
    PDEBUG("written %d bytes", retval);

  out:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence) {
    PDEBUG("seek to offset %lld with mode %d", offset, whence);

    struct aesd_dev *dev = filp->private_data;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    loff_t size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circ_buffer, index) {
        size += entry->size;
    }

    loff_t retval = fixed_size_llseek(filp, offset, whence, size);

  out:
    mutex_unlock(&dev->lock);
    return retval;
}

long aesd_adjust_file_offset(struct file *filp, struct aesd_seekto *seekto) {
    PDEBUG("adjust file offset to command %d, offset %d", seekto->write_cmd, seekto->write_cmd_offset);
    long retval = 0;

    struct aesd_dev *dev = filp->private_data;
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    loff_t prev_size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circ_buffer, index) {
        if (index == seekto->write_cmd) {
            if (entry->size < seekto->write_cmd_offset) {
                filp->f_pos = prev_size + seekto->write_cmd_offset;
                retval = 0;
                goto out;
            } else {
                retval = -EINVAL;
                goto out;
            }
        } else {
            prev_size += entry->size;
        }
    }
    // index not found
    retval = -EINVAL;

  out:
    mutex_unlock(&dev->lock);
    return retval;
}


long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    PDEBUG("ioctl with cmd %d, arg %ld", cmd, arg);
    long retval = 0;
    int err = 0;

	if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok_wrapper(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok_wrapper(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err) return -EFAULT;

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO: {
            struct aesd_seekto seekto;
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0) {
                retval = EFAULT;
            } else {
                retval = aesd_adjust_file_offset(filp, &seekto);
            }
            break;
        }
        default:
            retval = -ENOTTY;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl
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
