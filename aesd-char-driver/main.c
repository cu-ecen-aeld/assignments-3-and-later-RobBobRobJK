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
#include <linux/slab.h>     // kmalloc, krealloc, kfree
#include <linux/uaccess.h>  // copy_to_user, copy_from_user
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("RobBobRobJK");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /* inode->i_cdev points at the cdev inside our aesd_dev; container_of walks back to the whole struct */
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /* nothing was allocated in open, so there is nothing to free here */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Which stored write holds byte position *f_pos, counting all 10 writes joined end to end? */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (entry == NULL) {
        retval = 0;     /* past the last byte: end of file, so cat stops */
        goto out;
    }

    /* Send what is left of this entry, but never more than the caller asked for */
    bytes_to_copy = entry->size - entry_offset;
    if (bytes_to_copy > count)
        bytes_to_copy = count;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;    /* next read continues where this one stopped */
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    struct aesd_buffer_entry new_entry;
    char *new_buf;
    size_t new_size;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (count == 0)
        return 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Grow the pending (partial) buffer to hold the new bytes after whatever is already there.
     * krealloc(NULL, n) behaves like kmalloc, and on failure the old buffer stays valid. */
    new_size = dev->partial.size + count;
    new_buf = krealloc(dev->partial.buffptr, new_size, GFP_KERNEL);
    if (new_buf == NULL)
        goto out;
    dev->partial.buffptr = new_buf;

    if (copy_from_user(new_buf + dev->partial.size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    dev->partial.size = new_size;

    /* Only a newline-terminated command goes into the circular buffer */
    if (new_buf[new_size - 1] == '\n') {
        /* If the ring is full, the slot at in_offs holds the oldest write, which add_entry is about
         * to overwrite. Free its memory first or it leaks. */
        if (dev->buffer.full)
            kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);

        new_entry.buffptr = dev->partial.buffptr;   /* the ring now owns this memory */
        new_entry.size = dev->partial.size;
        aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);

        dev->partial.buffptr = NULL;                /* start a fresh partial for the next command */
        dev->partial.size = 0;
    }

    retval = count;

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

    /* AESD specific setup: lock and an empty circular buffer */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    uint8_t index;
    struct aesd_buffer_entry *entry;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /* Free every stored write (kfree(NULL) is safe) and any unfinished partial write */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        kfree(entry->buffptr);
    }
    kfree(aesd_device.partial.buffptr);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
