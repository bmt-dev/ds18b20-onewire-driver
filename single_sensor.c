#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#define MY_MAJOR 42
#define MY_MAX_MINORS 1

#define NUM_RETRIES 50

/* Sélection du numéro du bus avec le paramètre `bus_pin'. */
int bus_pin = 17;

struct cdev cdev;

/* Fonctions onewire */
inline void onewire_high(void)
{
	/* Bus à l'état haut réalisé en plaçant le pin en mode entrée. */
	gpio_direction_input(bus_pin);
}

inline void onewire_low(void)
{
	/* Bus à l'état bas réalisé en plaçant le pin en mode sortie, valeur
	   0. */
	gpio_direction_output(bus_pin, 0);
}

int onewire_reset(void)
{
	int val, was_pulled_down = 0, retries = 0;

	onewire_low();
	usleep_range(480, 500);
	onewire_high();

	usleep_range(15, 20);

	do
	{
		udelay(5);
		val = gpio_get_value(bus_pin);

		if (!was_pulled_down && !val)
			was_pulled_down = 1;
		++retries;
	} while ((!was_pulled_down || (was_pulled_down && !val)) && retries < NUM_RETRIES);

	if (retries == NUM_RETRIES)
	{
		printk(KERN_ALERT "failed to reset onewire bus (50 retries)\n");
		return -ECANCELED;
	}

	udelay(5);
	return 0;
}

inline int onewire_read(void)
{
	int res;

	/* Ouverture d'une fenêtre de lecture, cf. datasheet page 16. */
	onewire_low();
	udelay(5);
	onewire_high();

	/* 15µs après le front descendant, si le bus est à l'état bas, un 0 a
	   été écrit, sinon un 1 a été écrit. */
	udelay(10);
	res = gpio_get_value(bus_pin);
	usleep_range(50, 55);

	return res;
}

u8 onewire_read_byte(void)
{
	int i;
	u8 val = 0;

	/* Lecture d'un octet. */
	for (i = 0; i < 8; ++i)
		val |= onewire_read() << i;

	return val;
}

inline void onewire_write_zero(void)
{
	/* Écriture d'un 0 : on met le bus à l'état bas pour 60µs, cf. datasheet
	   page 15. */
	onewire_low();
	usleep_range(60, 65);
	onewire_high();
	udelay(5);
}

inline void onewire_write_one(void)
{
	/* Écriture d'un 1 : on met le bus à l'état bas pour 5µs, et on attend
	   60µs, cf. datasheet page 15. */
	onewire_low();
	udelay(5);
	onewire_high();
	usleep_range(60, 65);
}

void onewire_write_byte(u8 b)
{
	u8 mask;

	/* Écriture d'un octet. */
	for (mask = 1; mask; mask <<= 1)
	{
		if (b & mask)
			onewire_write_one();
		else
			onewire_write_zero();
	}
}

u8 onewire_crc8(const u8 *data, size_t len)
{
	size_t i;
	u8 shift_register = 0; /* Shift register à 0. */

	/* Calcul d'un CRC, selon la description sur la datasheet page 9. */
	/* Pour chaque octet disponible en données… */
	for (i = 0; i < len; ++i)
	{
		u8 mask;

		/* Pour chaque bit de chaque octet… */
		for (mask = 1; mask; mask <<= 1)
		{
			/* input = (bit courant) xor shifted_register[0] */
			u8 input = (!!(data[i] & mask)) ^ (shift_register & 0x01);

			/* Shift à gauche du registre, masquage des bits 3 et
			   4 pour les remplacer. */
			u8 shifted_register = (shift_register >> 1) & ~0x0C;

			/* shifted_register[2] = input xor shift_register[3] */
			shifted_register |= ((!!(shift_register & 0x08)) ^ input) << 2;

			/* shifted_register[3] = input xor shift_register[4] */
			shifted_register |= ((!!(shift_register & 0x10)) ^ input) << 3;

			/* shifted_register[7] = input */
			shifted_register |= input << 7;
			shift_register = shifted_register;
		}
	}

	return shift_register;
}

void send_command(u8 command)
{
	onewire_write_byte(command);
}

void read_temp(char *buf)
{
	pr_warn("reading temperature from the sensor");

	int reset_response = onewire_reset();

	if (reset_response)
	{
		pr_warn("cannot reset");
		return;
	}

	// send skip rom function command CC
	send_command(0xCC);

	// perform a temperature conversion 44
	send_command(0x44);

	// check if conversion end (return 1)
	while (onewire_read_byte() == 0)
		;

	onewire_reset();
	
	// send skip rom function command CC
	send_command(0xCC);

	// issue a Read Scratchpad
	send_command(0xBE);

	// read temp from scratchpad memory
	u8 temp_lsb = (u8)onewire_read_byte();
	u8 temp_msb = (u8)onewire_read_byte();

	int temp = (temp_msb & 0x07) << 4; // grab lower 3 bits of temp_msb
	temp |= temp_lsb >> 4;			   // grab upper 4 bits temp_lsb
	int temp_d = temp_lsb & 0x0F;	   // grab decimals in lower 4 bits of temp_lsb
	temp_d *= 625;
	u8 sign = temp_msb & 0x80;

	if (sign)
	{
		temp = 127 - temp; // needed when negative temperature
		temp *= -1;		   // add sign now
	}

	if (temp_d < 1000)
	{
		sprintf(buf, "%d,0%d°C", temp, temp_d);
	}
	else
	{
		sprintf(buf, "%d,%d°C", temp, temp_d);
	}
}

static int my_open(struct inode *inode, struct file *file)
{
	pr_warn("open");
	return 0;
}

static int my_release(struct inode *inode, struct file *file)
{
	pr_warn("release");
	return 0;
}

static int my_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
	if (size <= 0)
		return 0;

	char buf[64];

	read_temp(buf);

	int cnt = strlen(buf);

	if (*offset >= cnt)
		return 0;

	if (copy_to_user(user_buffer, buf, cnt))
	{
		return -EFAULT;
	}

	*offset += cnt;

	return cnt;
}

static int my_write(struct file *file, const char __user *user_buffer, size_t size, loff_t *offset)
{
	pr_warn("write");
	return size;
}

struct file_operations my_fops = {
	.owner = THIS_MODULE,
	.open = my_open,
	.read = my_read,
	.write = my_write,
	.release = my_release};

int init_module(void)
{
	pr_warn("Hi, module initialization\n");

	int r;

	r = register_chrdev_region(MKDEV(MY_MAJOR, 0), MY_MAX_MINORS, "my_device_driver");

	if (r != 0)
	{
		/* report error */
		pr_warn("Cannot register device");
	}

	cdev_init(&cdev, &my_fops);
	cdev_add(&cdev, MKDEV(MY_MAJOR, 0), 1);

	pr_warn("Device registered");
	return 0;
}

void cleanup_module(void)
{
	pr_warn("Bye, module exit\n");

	cdev_del(&cdev);

	unregister_chrdev_region(MKDEV(MY_MAJOR, 0), MY_MAX_MINORS);

	pr_warn("Device unregistered");
}

MODULE_DESCRIPTION("My kernel module");
MODULE_AUTHOR("Madani TALL");
MODULE_LICENSE("GPL");
