// SPDX-License-Identifier: GPL

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/ktime.h>

#define DRV_NAME "otp"
#define MAX_DEVS 256
#define MAX_KEY_LEN 16

// System configuration
static struct class *dev_class;
static struct proc_dir_entry *proc_entry;
static int dev_major;

// Device states
enum {
	DEV_FREE = 0,
	DEV_BUSY = 1,
};

// Core data structure
struct sys_data {
	union {
		int counter;
		struct {
			time64_t timestamp;
			char secret[MAX_KEY_LEN];
		} dynamic;
	} info;
	bool used;
	atomic_t status;
	bool dynamic_mode;
};

// Global state
static struct sys_data sys_devices[MAX_DEVS];
static int num_devices = 1;
static char *pwd_list[4096] = { NULL };
static int pwd_list_argc;
static int pwd_key = 0x42;
static int pwd_expiration = 30;

// Forward declarations
static int sys_open(struct inode *inode, struct file *file);
static int sys_release(struct inode *inode, struct file *file);
static ssize_t sys_read(struct file *file, char __user *buf, size_t len, loff_t *off);
static ssize_t sys_write(struct file *file, const char __user *buf, size_t len, loff_t *off);
static long sys_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

// System operations
static const struct file_operations sys_ops = {
	.read = sys_read,
	.write = sys_write,
	.open = sys_open,
	.release = sys_release,
	.unlocked_ioctl = sys_ioctl,
};

// Initialize system state
static struct sys_data init_sys(void)
{
	return (struct sys_data) {
		.info = {.counter = -1},
		.used = true,
		.status = ATOMIC_INIT(DEV_FREE),
		.dynamic_mode = false
	};
}

// Update device configuration
static int update_devices(const char *val, const struct kernel_param *kp)
{
	int old_count = num_devices;
	int result = param_set_int(val, kp);

	if (result != 0)
		return -EINVAL;

	if (num_devices < 1 || num_devices > MAX_DEVS) {
		num_devices = old_count;
		return -EINVAL;
	}

	if (dev_class == NULL)
		return 0;

	pr_info("otp: device count changed from %d to %d\n", old_count, num_devices);

	if (num_devices > old_count) {
		for (int i = old_count; i < num_devices; i++) {
			device_create(dev_class, NULL, MKDEV(dev_major, i), NULL, "%s%d", DRV_NAME, i);
			sys_devices[i] = init_sys();
			pr_info("otp: new device at /dev/%s%d\n", DRV_NAME, i);
		}
	} else if (num_devices < old_count) {
		for (int i = num_devices; i < old_count; i++) {
			device_destroy(dev_class, MKDEV(dev_major, i));
			pr_info("otp: removed device /dev/%s%d\n", DRV_NAME, i);
		}
	}

	return 0;
}

// System parameter handlers
static const struct kernel_param_ops devices_ops = {
	.set = &update_devices,
	.get = &param_get_int,
};

// Register system parameters
module_param_cb(devices, &devices_ops, &num_devices, 0660);
MODULE_PARM_DESC(devices, "Number of devices");

module_param_array(pwd_list, charp, &pwd_list_argc, 0660);
MODULE_PARM_DESC(pwd_list, "Secret list");

module_param(pwd_key, int, 0660);
MODULE_PARM_DESC(pwd_key, "Secret key");

module_param(pwd_expiration, int, 0660);
MODULE_PARM_DESC(pwd_expiration, "Key timeout in seconds");

// Device access functions
static int sys_open(struct inode *inode, struct file *file)
{
	int minor = iminor(file_inode(file));

	if (atomic_cmpxchg(&sys_devices[minor].status, DEV_FREE, DEV_BUSY))
		return -EBUSY;

	try_module_get(THIS_MODULE);

	return 0;
}

static int sys_release(struct inode *inode, struct file *file)
{
	int minor = iminor(file_inode(file));

	atomic_set(&sys_devices[minor].status, DEV_FREE);

	module_put(THIS_MODULE);

	return 0;
}

// Data generation
static void generate_secret(char *key, int key_len)
{
	const char first_char = '0';
	const char last_char_offset = '9' - first_char;
	int random = 0;

	for (int i = 0; i < key_len; i++) {
		get_random_bytes(&random, sizeof(random));
		key[i] = first_char + ((random ^ pwd_key) % last_char_offset);
	}
}

// Read operations
static ssize_t sys_read_list(struct file *file,
				char __user *buf,
				size_t len,
				loff_t *off)
{
	size_t str_len;
	size_t read_len;

	int minor = iminor(file_inode(file));

	if (pwd_list_argc == 0)
		return -EINVAL;

	if (*off == 0) {
		sys_devices[minor].info.counter++;
		if (sys_devices[minor].info.counter >= pwd_list_argc)
			sys_devices[minor].info.counter = 0;
	}

	str_len = strlen(pwd_list[sys_devices[minor].info.counter]);
	read_len = min(len, (size_t)(str_len - *off));

	if (*off >= str_len) {
		sys_devices[minor].used = false;
		return 0;
	}

	if (copy_to_user(buf, pwd_list[sys_devices[minor].info.counter] + *off, read_len))
		return -EFAULT;

	*off += read_len;

	return read_len;
}

static ssize_t sys_read_dynamic(struct file *file,
				char __user *buf,
				size_t len,
				loff_t *off)
{
	ssize_t read_len;

	int minor = iminor(file_inode(file));

	if (*off == 0)
		generate_secret(sys_devices[minor].info.dynamic.secret, MAX_KEY_LEN);

	sys_devices[minor].info.dynamic.timestamp = ktime_get_seconds();

	read_len = min(len, (size_t)(MAX_KEY_LEN - *off));

	if (*off >= MAX_KEY_LEN) {
		sys_devices[minor].used = false;
		return 0;
	}

	if (copy_to_user(buf, sys_devices[minor].info.dynamic.secret + *off, read_len))
		return -EFAULT;

	*off += read_len;

	return read_len;
}

static ssize_t sys_read(struct file *file,
				char __user *buf,
				size_t len,
				loff_t *off)
{
	int minor = iminor(file_inode(file));

	if (sys_devices[minor].dynamic_mode)
		return sys_read_dynamic(file, buf, len, off);
	else
		return sys_read_list(file, buf, len, off);
}

// Write operations
static ssize_t sys_write_list(struct file *file, const char __user *buf,
				size_t len, loff_t *off)
{
	static char kernel_buf[PAGE_SIZE];
	int secret_len = 0;

	int minor = iminor(file_inode(file));

	if (sys_devices[minor].used)
		return -EINVAL;

	if (sys_devices[minor].info.counter == -1 || sys_devices[minor].info.counter >= pwd_list_argc)
		return -EINVAL;

	secret_len = strlen(pwd_list[sys_devices[minor].info.counter]);

	if (len > PAGE_SIZE)
		return -EINVAL;

	if (len != secret_len)
		return -EINVAL;

	if (copy_from_user(kernel_buf, buf, len))
		return -EFAULT;

	if (strncmp(kernel_buf, pwd_list[sys_devices[minor].info.counter], secret_len) == 0) {
		sys_devices[minor].used = true;
		return len;
	} else
		return -EINVAL;
}

static ssize_t sys_write_dynamic(struct file *file, const char __user *buf,
				size_t len, loff_t *off)
{
	static char kernel_buf[MAX_KEY_LEN];
	int secret_len = 0;

	int minor = iminor(file_inode(file));

	if (sys_devices[minor].used)
		return -EINVAL;

	if (sys_devices[minor].info.dynamic.timestamp + pwd_expiration < ktime_get_seconds())
		return -EINVAL;

	secret_len = MAX_KEY_LEN;

	if (len != secret_len)
		return -EINVAL;

	if (copy_from_user(kernel_buf, buf, len))
		return -EFAULT;

	if (strncmp(kernel_buf, sys_devices[minor].info.dynamic.secret, secret_len) == 0) {
		sys_devices[minor].used = true;
		return len;
	} else
		return -EINVAL;
}

static ssize_t sys_write(struct file *file, const char __user *buf,
				size_t len, loff_t *off)
{
	int minor = iminor(file_inode(file));

	if (sys_devices[minor].dynamic_mode)
		return sys_write_dynamic(file, buf, len, off);
	else
		return sys_write_list(file, buf, len, off);
}

// Control operations
static long sys_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int minor = iminor(file_inode(file));

	switch (cmd) {
	case 0:
		sys_devices[minor].dynamic_mode = false;
		sys_devices[minor].info.counter = -1;
		pr_info("Switched to list mode for /dev/%s%i\n", DRV_NAME, minor);
		break;
	case 1:
		sys_devices[minor].dynamic_mode = true;
		*sys_devices[minor].info.dynamic.secret = 0;
		pr_info("Switched to dynamic mode for /dev/%s%i\n", DRV_NAME, minor);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

// System status display
static int proc_show(struct seq_file *seq, void *off)
{
	seq_printf(seq, "DEVICE     MODE     SECRET\n");
	seq_printf(seq, "------     ----     -------\n");

	for (int i = 0; i < num_devices; i++) {
		int counter = sys_devices[i].info.counter;
		bool used = sys_devices[i].used;
		bool dynamic = sys_devices[i].dynamic_mode;
		time64_t elapsed = ktime_get_seconds() - sys_devices[i].info.dynamic.timestamp;

		seq_printf(seq, "%s%d%s     %s     ",
			DRV_NAME,
			i,
			i < 10 ? "  " : (i < 100 ? " " : ""),
			dynamic ? "dynamic" : "list"
		);

		seq_printf(seq, "%s",
			dynamic ? (elapsed > pwd_expiration || used ? "" : sys_devices[i].info.dynamic.secret) : (
				counter == -1 || used ? "" : pwd_list[counter]
			)
		);

		if (dynamic && !(elapsed > pwd_expiration || used))
			seq_printf(seq, " (%lld secs)\n", pwd_expiration - elapsed);
		else
			seq_printf(seq, "\n");
	}

	return 0;
}

// System initialization and cleanup
static int __init sys_init(void)
{
	dev_major = register_chrdev(0, DRV_NAME, &sys_ops);

	if (dev_major < 0) {
		pr_alert("Device registration failed with code %d\n", dev_major);
		return dev_major;
	}

	pr_info("otp: major number assigned: %d\n", dev_major);

	dev_class = class_create(DRV_NAME);

	for (int i = 0; i < num_devices; i++) {
		device_create(dev_class, NULL, MKDEV(dev_major, i), NULL, "%s%d", DRV_NAME, i);
		sys_devices[i] = init_sys();
		pr_info("otp: new device at /dev/%s%d\n", DRV_NAME, i);
	}

	proc_entry = proc_create_single_data(DRV_NAME, 0666, NULL, &proc_show, NULL);

	pr_info("otp: proc created at /proc/%s\n", DRV_NAME);

	return 0;
}

static void __exit sys_exit(void)
{
	proc_remove(proc_entry);

	for (int i = 0; i < num_devices; i++) {
		device_destroy(dev_class, MKDEV(dev_major, i));
		pr_info("otp: removed device /dev/%s%d\n", DRV_NAME, i);
	}
	class_destroy(dev_class);

	unregister_chrdev(dev_major, DRV_NAME);

	pr_info("otp: proc removed /proc/%s\n", DRV_NAME);
}

module_init(sys_init);
module_exit(sys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martin Olivier, Gabriel Medoukali, Edouard Sengeissen");
MODULE_DESCRIPTION("A one time password management kernel module");
