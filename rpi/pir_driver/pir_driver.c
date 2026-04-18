#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/device.h>

#define DEVICE_NAME "pir_dev"
#define GPIO_PIN 529   // GPIO

static int major;
static struct class *pir_class;
static struct device *pir_device;

// open
static int dev_open(struct inode *inode, struct file *file)
{
    return 0;
}

// read
static ssize_t dev_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
    char value_str[4];
    int value;
    int ret;
   
    value = gpio_get_value(GPIO_PIN);

    snprintf(value_str, sizeof(value_str), "%d\n", value);

    ret = copy_to_user(buf, value_str, strlen(value_str));
    if (ret != 0)
        return -EFAULT;

    return strlen(value_str);
}

// file operations
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .read = dev_read,
};

// init
static int __init pir_init(void)
{
    printk(KERN_INFO "PIR device driver start\n");

    if (!gpio_is_valid(GPIO_PIN))
        return -ENODEV;

    gpio_request(GPIO_PIN, "pir_gpio");
    gpio_direction_input(GPIO_PIN);

    major = register_chrdev(0, DEVICE_NAME, &fops);

    pir_class = class_create( "pir_class");
    pir_device = device_create(pir_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

    printk(KERN_INFO "Major number: %d\n", major);
    printk(KERN_INFO "/dev/pir_dev created\n");

    return 0;
}

// exit
static void __exit pir_exit(void)
{
    device_destroy(pir_class, MKDEV(major, 0));
    class_destroy(pir_class);

    unregister_chrdev(major, DEVICE_NAME);
    gpio_free(GPIO_PIN);

    printk(KERN_INFO "PIR device driver exit\n");
}

module_init(pir_init);
module_exit(pir_exit);

MODULE_LICENSE("GPL");


