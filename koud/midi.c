#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>

static int major;
static char text[64];

static ssize_t my_read(struct file *fp, char __user *user_buf, size_t len, loff_t *off)
{
    int to_copy = (len+*off) < sizeof(text) ? len : (sizeof(text)-*off);
    printk(KERN_DEBUG "Read is called, we want to read %ld bytes, but actually only copying %d bytes. The offset is %lld\n", len, to_copy, *off);
	if (*off >= sizeof(text))
		return 0;
	not_copied = copy_to_user(user_buf, &text[*off], to_copy);
	delta = to_copy-not_copied;
	if (not_copied) 
		printk(KERN_WARNING "Could only copy %d bytes\n", delta);
	*off += delta;

	return delta;
}

static struct file_operations fops = {
    .read = my_read
};

static int __init my_init(void)
{
    major = register_chrdev(0, "hello_driver", &fops);
    if (major < 0)
    {
        printk(KERN_ERR "Error registering device\n");
        return major;
    }
    printk(KERN_DEBUG "Major Device Number: %d\n", major);

    return 0;
}

static void __exit my_exit(void)
{
    unregister_chrdev(major, "hello_driver");
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");