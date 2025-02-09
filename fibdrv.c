#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 100

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);

#define BUFLEN 100
#define ASC2INT(x) (x - 0x30)
#define INT2ASC(x) (x + 0x30)

typedef struct {
    char buf[BUFLEN];
} Bignum;

void bn_zero(Bignum *x)
{
    for (int i = 0; i < 99; ++i)
        x->buf[i] = 0;
    x->buf[99] = '0';
}

void bn_one(Bignum *x)
{
    for (int i = 0; i < 99; ++i)
        x->buf[i] = 0;
    x->buf[99] = '1';
}

void bn_assgin(Bignum *dest, const Bignum *src)
{
    for (int i = 0; i < 100; ++i)
        dest->buf[i] = src->buf[i];
}

void bn_add(Bignum *dest, const Bignum *x, const Bignum *y)
{
    int idx = BUFLEN - 1;
    for (int i = 0; i < 100; ++i)
        dest->buf[i] = 0;
    while (x->buf[idx] != 0 || y->buf[idx] != 0) {
        if (x->buf[idx])
            dest->buf[idx] += ASC2INT(x->buf[idx]);
        if (y->buf[idx])
            dest->buf[idx] += ASC2INT(y->buf[idx]);
        if (dest->buf[idx] >= 10) {
            dest->buf[idx] -= 10;
            dest->buf[idx - 1] = 1;
        }
        dest->buf[idx] = INT2ASC(dest->buf[idx]);
        idx--;
    }
    if (dest->buf[idx])
        dest->buf[idx] = INT2ASC(dest->buf[idx]);
}

void bn_print(Bignum *x)
{
    for (int i = 0; i < BUFLEN; ++i)
        pr_info("%c", x->buf[i]);
    pr_info("\n");
}

static Bignum *fib_sequence(long long k)
{
    /* FIXME: use clz/ctz and fast algorithms to speed up */
    /* FIXME: use alloc to store f in heap */
    Bignum f[k + 2];

    bn_zero(&f[0]);
    bn_one(&f[1]);

    for (int i = 2; i <= k; i++) {
        bn_add(&f[i], &f[i - 1], &f[i - 2]);
    }

    Bignum *ret = kmalloc(sizeof(Bignum), GFP_KERNEL);
    bn_assgin(ret, &f[k]);

    return ret;
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    /* FIXME: change to cp ret of fib_sequence to buf and then return cp size*/
    Bignum *n = fib_sequence(*offset);
    bn_print(n);
    int start = 0;
    for (start = 0; start < BUFLEN && !n->buf[start]; ++start)
        ;
    for (int i = start; i < BUFLEN; ++i)
        put_user(n->buf[i], buf++);

    kfree(n);

    return (ssize_t)(BUFLEN - start);
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
