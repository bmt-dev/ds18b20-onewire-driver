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

#define FALSE 0
#define TRUE 1

/* Sélection du numéro du bus avec le paramètre `bus_pin'. */
int bus_pin = 17;

struct cdev cdev;

unsigned char ROM[8];          // ROM Bit
unsigned char lastDiscrep = 0; // last discrepancy
unsigned char doneFlag = 0;    // Done flag
unsigned char foundROMs[5][8];  // table of found ROM codes
unsigned char numROMs;
unsigned char dowcrc;

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

void write_bit(char bitval)
{
    if (bitval == 1)
    {
        onewire_write_one();
    }
    else
    {
        onewire_write_zero();
    }
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

unsigned char next(void)
{
    unsigned char m = 1; // ROM Bit index
    unsigned char n = 0; // ROM Byte index
    unsigned char k = 1; // bit mask
    unsigned char x = 0;
    unsigned char discrepMarker = 0; // discrepancy marker
    unsigned char g;                 // Output bit
    unsigned char nxt;               // return value
    nxt = FALSE;                     // set the next flag to false
    int flag;
    dowcrc = 0;
    flag = onewire_reset();
    if (flag || doneFlag)
    {
        lastDiscrep = 0; // reset the search
        return FALSE;
    }

    send_command(0xF0); // send Search ROM command

    do
    // for all eight bytes
    {
        x = 0;
        if (onewire_read() == 1)
            x = 2;
        udelay(6);
        if (onewire_read() == 1)
            x |= 1; // and its complement
        if (x == 3) // there are no devices on the 1-Wire
            break;

        else
        {
            if (x > 0)      // all devices coupled have 0 or 1
                g = x >> 1; // bit write value for search
            else
            {
                if (m < lastDiscrep)
                    g = ((ROM[n] & k) > 0);
                else // if equal to last pick 1
                    g = (m == lastDiscrep);

                if (g == 0)
                    discrepMarker = m;
            }
            if (g == 1)
                ROM[n] |= k;
            else
                ROM[n] &= ~k;
            write_bit(g);
            m++;
            k = k << 1;
            if (k == 0)
            {
                onewire_crc8(ROM[n], sizeof(ROM[n]));
                n++;
                k++;
            }
        }
    } while (n < 8); // loop until through all ROM bits (0-7)
    if (m < 65 || dowcrc)
        lastDiscrep = 0;
    else
    {
        lastDiscrep = discrepMarker;
        doneFlag = (lastDiscrep == 0);
        nxt = TRUE; // search is not complete yet
    }
    return nxt;
}

unsigned char first(void)
{
    lastDiscrep = 0; // reset the rom search last discrepancy global
    doneFlag = FALSE;
    return next(); // call next and return its return value
}

void find_sensors(void)
{
    unsigned char m;
    if (!onewire_reset())
    {
        if (first()) // Begins when at least one part is found
        {
            numROMs = 0;
            do
            {
                numROMs++;
                for (m = 0; m < 8; m++)
                {
                    foundROMs[numROMs][m] = ROM[m]; // Identifies ROM number on found device
                }
            } while (next() && (numROMs < 10)); // Continues until no additional devices are found
        }
    }

    pr_warn("%d sensor(s) found !", numROMs);
}

void match_rom(unsigned char *rom)
{
    send_command(0x55); // match ROM command

    int i;

    for (i = 0; i < 8; i++)
    {
        onewire_write_byte(rom[i]); // send ROM code
    }
}

void read_temp(char *buf, unsigned char *rom)
{
    pr_warn("reading temperature from ROM %02X %02X %02X %02X %02X %02X %02X %02X\n", rom[7], rom[6], rom[5], rom[4],
            rom[3], rom[2], rom[1], rom[0]);

    int reset_response = onewire_reset();

    if (reset_response)
    {
        pr_warn("cannot reset");
        return;
    }

    match_rom(rom);

    // perform a temperature conversion 44
    send_command(0x44);

    // check if conversion end (return 1)
    while (onewire_read_byte() == 0)
        ;

    onewire_reset();

    match_rom(rom);

    // issue a Read Scratchpad
    send_command(0xBE);

    // read temp from scratchpad memory
    u8 temp_lsb = (u8)onewire_read_byte();
    u8 temp_msb = (u8)onewire_read_byte();

    int temp = (temp_msb & 0x07) << 4; // grab lower 3 bits of temp_msb
    temp |= temp_lsb >> 4;             // grab upper 4 bits temp_lsb
    int temp_d = temp_lsb & 0x0F;      // grab decimals in lower 4 bits of temp_lsb
    temp_d *= 625;
    u8 sign = temp_msb & 0x80;

    if (sign)
    {
        temp = 127 - temp; // needed when negative temperature
        temp *= -1;        // add sign now
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

    find_sensors(); // sensors discovering

    if (numROMs == 0)
        return 0;

    char temp[2][20];

    int i;

    for (i = 1; i <= numROMs; i++)
    {
        pr_warn("\nROM CODE =%02X %02X %02X %02X %02X %02X %02X %02X\n",
                foundROMs[i][7], foundROMs[i][6], foundROMs[i][5], foundROMs[i][4],
                foundROMs[i][3], foundROMs[i][2], foundROMs[i][1], foundROMs[i][0]);

        read_temp(temp[i - 1], foundROMs[i]);

        if (i == 2)
            break; // stop after reading two sensors
    }

    char *buffer_out = kmalloc(strlen(temp[0]) + 3 + strlen(temp[1]), GFP_KERNEL);

    strcpy(buffer_out, temp[0]);
    strcat(buffer_out, " | "); // separator for the two readed temp
    strcat(buffer_out, temp[1]);

    int cnt = strlen(buffer_out);

    if (*offset >= cnt)
        return 0;

    if (copy_to_user(user_buffer, buffer_out, cnt))
    {
        return -EFAULT;
    }

    *offset += cnt;

    kfree(buffer_out);

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
