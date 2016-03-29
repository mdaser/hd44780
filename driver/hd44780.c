/* -*- linux-c -*- */
/*
 * Copyright (C) 2016 Martin Daser
 *
 * Notes:
 * Mar 26 2016: import and make checkpatch clean
 * Mar 29 2016: add symbolic constants and traces
 *
 * original source from https://ezs.kr.hsnr.de/EmbeddedBuch/
 */
/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>

static dev_t hd44780_dev_number = MKDEV(181, 0);

static struct cdev *driver_object;
static struct class *hd44780_class;
static struct device *hd44780_dev;

static char textbuffer[1024];

/*
  RS = GPIO  7
  E  = GPIO  8
  D4 = GPIO 25
  D5 = GPIO 24
  D6 = GPIO 23
  D7 = GPIO 18

  LcdWrite(register, data);

  1. set RS line high or low to designate the register you wish to access
  2. set R/W line low to indicate a write
  3. set DBPORT to output
  4. write data to DBPORT
  5. set E line high to begin write cycle
  6. pause to allow LCD to accept the data
  7. set E line low to finish write cycle


  Initsequenz:
  msleep(15);
  NibbleWrite(0, 0x3);
  msleep(5);
  NibbleWrite(0, 0x3);
  udelay(100);
  NibbleWrite(0, 0x3);
  NibbleWrite(0, 0x2);
  LcdWrite(0, 0x28); // CMD: 4-Bit Mode, 2 stellige Anzeige, 5x7 Font
  LcdWrite(0,

  http://www.sprut.de/electronic/lcd/	 !!!!!
  https://www.mikrocontroller.net/articles/AVR-Tutorial:_LCD
*/

/*
 * define gpio port numbers
 */
enum gpio_port {
	port_rs =  7,
	port_e	=  8,
	port_d4 = 25,
	port_d5 = 24,
	port_d6 = 23,
	port_d7 = 18,
};

/*
 * The data sheet of the HD44780 display controler may be found here:
 * https://www.sparkfun.com/datasheets/LCD/HD44780.pdf
 *
 * HW specific values are taken from there
 */

/*
 * define value for register access
 */
enum h44780_rs {
	rs_cmd = 0,
	rs_data = 1,
};

/*
 * HD44780 Instructions
 */
enum hd44780_instruction {
	/* instructions */
	clear_display	     = 0x01,
	return_home	     = 0x02,
	entry_mode_set	     = 0x04,
	display_on_off	     = 0x08,
	cursor_display_shift = 0x10,
	function_set	     = 0x20,
	set_cg_ram	     = 0x40,
	set_dd_ram	     = 0x80,

	/*
	 * extra bits for each instruction
	 */

	/* entry mode set */
	id_incr		     = 0x02,
	id_decr		     = 0x00,
	acc_display_shift    = 0x01,

	/* display on off control */
	display_on	     = 0x04,
	cursor_on	     = 0x02,
	blinking	     = 0x01,

	/* cursor or display shift */
	display_shift	     = 0x08,
	cursor_move	     = 0x00,
	shift_right	     = 0x04,
	shift_left	     = 0x00,

	/* function set */
	dl_8_bits	     = 0x10,
	dl_4_bits	     = 0x00,
	n_lines_2	     = 0x08,
	n_lines_1	     = 0x00,
	font_10_dots	     = 0x40,
	font_8_dots	     = 0x00,

	/* set DDRAM (depend on line mode) */
	ddram_line_1	     = 0x00,
	ddram_line_2	     = 0x40,
};

static void NibbleWrite(enum h44780_rs reg, int value)
{
	gpio_set_value(port_rs, reg);

	gpio_set_value(port_d4, value & 0x1);
	gpio_set_value(port_d5, value & 0x2);
	gpio_set_value(port_d6, value & 0x4);
	gpio_set_value(port_d7, value & 0x8);

	gpio_set_value(port_e, 1);
	udelay(40);
	gpio_set_value(port_e, 0);
}

static void LcdWrite(enum h44780_rs reg, int value)
{
	pr_info("hd44780: %s 0x%02x  %c [%c%c%c%c %c%c%c%c]\n",
		(reg ? "DTA" : "CMD"),
		value,
		((reg == rs_data && isprint(value)) ? value : ' '),
		(value & BIT(7) ? '*' : '.'),
		(value & BIT(6) ? '*' : '.'),
		(value & BIT(5) ? '*' : '.'),
		(value & BIT(4) ? '*' : '.'),
		(value & BIT(3) ? '*' : '.'),
		(value & BIT(2) ? '*' : '.'),
		(value & BIT(1) ? '*' : '.'),
		(value & BIT(0) ? '*' : '.'));

	NibbleWrite(reg, value >> 4);  /* High-Nibble */
	NibbleWrite(reg, value & 0xf); /* Low-Nibble */
}

static int gpio_request_output(int nr)
{
	char gpio_name[12];
	int err;

	snprintf(gpio_name, sizeof(gpio_name), "rpi-gpio-%d", nr);
	err = gpio_request(nr, gpio_name);
	if (err) {
		pr_err("hd44780: gpio_request for %s failed with %d\n",
			gpio_name, err);
		return -1;
	}

	err = gpio_direction_output(nr, 0);
	if (err) {
		pr_err("hd44780: gpio_direction_output failed %d\n", err);
		gpio_free(nr);
		return -1;
	}

	return 0;
}

static int display_init(void)
{
	pr_info("hd44780: display_init\n");

	if (gpio_request_output(port_rs) == -1)
		return -EIO;
	if (gpio_request_output(port_e) == -1)
		goto free7;
	if (gpio_request_output(port_d7) == -1)
		goto free8;
	if (gpio_request_output(port_d6) == -1)
		goto free18;
	if (gpio_request_output(port_d5) == -1)
		goto free23;
	if (gpio_request_output(port_d4) == -1)
		goto free24;

	msleep(15);
	NibbleWrite(rs_cmd, 0x3); /* (function_set | dl_8_bits) >> 4 */
	msleep(5);
	NibbleWrite(rs_cmd, 0x3);
	udelay(100);
	NibbleWrite(rs_cmd, 0x3);
	msleep(5);
	NibbleWrite(rs_cmd, 0x2); /* (function_set | dl_4_bits) >> 4 */
	msleep(5);

	/* CMD: function_set | dl_4_bits | n_lines_2 | font_8_dots */
	LcdWrite(rs_cmd, 0x28);
	msleep(2);
	/* CMD: clear_display */
	LcdWrite(rs_cmd, 0x01);
	msleep(2);
	/* CMD: display_on_off | display_on */
	LcdWrite(rs_cmd, 0x0c); /* display on, cursor off, blinking off */

	/* CMD: set_dd_ram | ddram_line_2 */
	LcdWrite(rs_cmd, 0xc0);
	LcdWrite(rs_data, 'H');
	LcdWrite(rs_data, 'i');

	return 0;

free24: gpio_free(port_d5);
free23: gpio_free(port_d6);
free18: gpio_free(port_d7);
free8:	gpio_free(port_e);
free7:	gpio_free(port_rs);

	return -EIO;
}

static int display_exit(void)
{
	pr_info("hd44780: display_exit\n");

	gpio_free(port_d4);
	gpio_free(port_d5);
	gpio_free(port_d6);
	gpio_free(port_d7);
	gpio_free(port_e);
	gpio_free(port_rs);

	return 0;
}

static ssize_t hd44780_write(struct file *instanz, const char __user *user,
	size_t count, loff_t *offset)
{
	unsigned long not_copied, to_copy;
	int i;

	to_copy = min(count, sizeof(textbuffer));
	not_copied = copy_from_user(textbuffer, user, to_copy);

	pr_info("hd44780: write([%lu] %s)\n", to_copy, textbuffer);

	LcdWrite(rs_cmd, 0x80); /* CMD: set_ddram | ddram_line_1 */
	for (i = 0; i < to_copy && textbuffer[i]; i++) {
		if (isprint(textbuffer[i]))
			LcdWrite(rs_data, textbuffer[i]);
		if (i == 15)
			LcdWrite(rs_cmd, 0xc0); /* set_ddram | ddram_line_2 */
	}

	return to_copy-not_copied;
}

static const struct file_operations hd44780_fops = {
	.owner	= THIS_MODULE,
	.write	= hd44780_write,
};

static int __init hd44780_init(void)
{
	pr_info("hd44780: init\n");

	if (register_chrdev_region(hd44780_dev_number, 1, "hd44780") < 0) {
		pr_warn("hd44780: devicenumber (248,0) in use!\n");
		return -EIO;
	}

	driver_object = cdev_alloc(); /* Anmeldeobjekt reserv. */
	if (!driver_object)
		goto free_device_number;

	driver_object->owner = THIS_MODULE;
	driver_object->ops = &hd44780_fops;

	if (cdev_add(driver_object, hd44780_dev_number, 1))
		goto free_cdev;

	hd44780_class = class_create(THIS_MODULE, "hd44780");
	if (IS_ERR(hd44780_class)) {
		pr_err("hd44780: no udev support\n");
		goto free_cdev;
	}

	hd44780_dev = device_create(hd44780_class, NULL, hd44780_dev_number,
		NULL, "%s", "hd44780");

	if (!display_init()) {
		pr_info("hd44780: init OK\n");

		return 0;
	}
free_cdev:
	kobject_put(&driver_object->kobj);
free_device_number:
	unregister_chrdev_region(hd44780_dev_number, 1);
	pr_err("hd44780: init failed\n");

	return -EIO;
}

static void __exit hd44780_exit(void)
{
	pr_info("hd44780: exit\n");

	display_exit();

	device_destroy(hd44780_class, hd44780_dev_number);
	class_destroy(hd44780_class);
	cdev_del(driver_object);
	unregister_chrdev_region(hd44780_dev_number, 1);
}

module_init(hd44780_init);
module_exit(hd44780_exit);
MODULE_LICENSE("GPL");
