#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <asm/uaccess.h>

static dev_t hd44780_dev_number = MKDEV(248,0);
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

LcdWrite( register, data );

1. set RS line high or low to designate the register you wish to access
2. set R/W line low to indicate a write
3. set DBPORT to output
4. write data to DBPORT
5. set E line high to begin write cycle
6. pause to allow LCD to accept the data
7. set E line low to finish write cycle


Initsequenz:
msleep( 15 );
NibbleWrite( 0, 0x3 );
msleep( 5 );
NibbleWrite( 0, 0x3 );
udelay( 100 );
NibbleWrite( 0, 0x3 );
NibbleWrite( 0, 0x2 );
LcdWrite( 0, 0x28 ); // CMD: 4-Bit Mode, 2 stellige Anzeige, 5x7 Font
LcdWrite( 0, 

http://www.sprut.de/electronic/lcd/    !!!!!
https://www.mikrocontroller.net/articles/AVR-Tutorial:_LCD
*/

static void NibbleWrite( int reg, int value )
{
	gpio_set_value(7,reg);
	gpio_set_value(25, value & 0x1 );
	gpio_set_value(24, value & 0x2 );
	gpio_set_value(23, value & 0x4 );
	gpio_set_value(18, value & 0x8 );
	gpio_set_value( 8, 1 );
	udelay(40);
	gpio_set_value( 8, 0 );
}

static void LcdWrite( int reg, int value )
{
	NibbleWrite( reg, value>>4 ); // High-Nibble
	NibbleWrite( reg, value&0xf ); // Low-Nibble
}

static int gpio_request_output( int nr )
{
	char gpio_name[12];
	int err;

	snprintf( gpio_name, sizeof(gpio_name), "rpi-gpio-%d", nr );
	err = gpio_request( nr, gpio_name );
	if (err) {
		printk("gpio_request for %s failed with %d\n", gpio_name, err);
		return -1;
	}
	err = gpio_direction_output( nr, 0 );
	if (err) {
		printk("gpio_direction_output failed %d\n", err);
		gpio_free( nr );
		return -1;
	}
	return 0;
}

static int display_init( void )
{
	printk("display_init\n");
	if (gpio_request_output(7)==-1)
		return -EIO;
	if (gpio_request_output(8)==-1)
		goto free7;
	if (gpio_request_output(18)==-1)
		goto free8;
	if (gpio_request_output(23)==-1)
		goto free18;
	if (gpio_request_output(24)==-1)
		goto free23;
	if (gpio_request_output(25)==-1)
		goto free24;
	msleep( 15 );
	NibbleWrite( 0, 0x3 );
	msleep( 5 );
	NibbleWrite( 0, 0x3 );
	udelay( 100 );
	NibbleWrite( 0, 0x3 );
	msleep( 5 );
	NibbleWrite( 0, 0x2 );
	msleep( 5 );
	LcdWrite( 0, 0x28 ); // CMD: 4-Bit Mode, 2 stellige Anzeige, 5x8 Font
	msleep( 2 );
	LcdWrite( 0, 0x01 );
	msleep( 2 );
	LcdWrite( 0, 0x0c ); // Display ein, Cursor aus, Blinken aus
	LcdWrite( 0, 0xc0 );
	LcdWrite( 1, 'H' );
	LcdWrite( 1, 'i' );
	return 0;
free24: gpio_free( 24 );
free23: gpio_free( 23 );
free18: gpio_free( 18 );
free8:  gpio_free( 8 );
free7:  gpio_free( 7 );
	return -EIO;
}

static int display_exit( void )
{
	printk( "display_exit called\n" );
	gpio_free( 25 );
	gpio_free( 24 );
	gpio_free( 23 );
	gpio_free( 18 );
	gpio_free( 8 );
	gpio_free( 7 );
	return 0;
}

static ssize_t driver_write(struct file *instanz,const char __user *user,
	size_t count, loff_t *offset )
{
	unsigned long not_copied, to_copy;
	int i;

	to_copy = min( count, sizeof(textbuffer) );
	not_copied=copy_from_user(textbuffer, user, to_copy);
	//printk("driver_write( %s )\n", textbuffer );

	LcdWrite( 0, 0x80 );
	for (i=0; i<to_copy && textbuffer[i]; i++) {
		if (isprint(textbuffer[i]))
			LcdWrite( 1, textbuffer[i] );
		if (i==15)
			LcdWrite( 0, 0xc0 );
	}

	return to_copy-not_copied;
}

static struct file_operations fops = {
    .owner  = THIS_MODULE,
    .write   = driver_write,
};

static int __init mod_init( void )
{
	if( register_chrdev_region(hd44780_dev_number,1,"hd44780")<0 ) {
		printk("devicenumber (248,0) in use!\n");
		return -EIO;
	}
	driver_object = cdev_alloc(); /* Anmeldeobjekt reserv. */
	if( driver_object==NULL )
		goto free_device_number;
	driver_object->owner = THIS_MODULE;
	driver_object->ops = &fops;
	if( cdev_add(driver_object,hd44780_dev_number,1) )
		goto free_cdev;
	hd44780_class = class_create( THIS_MODULE, "hd44780" );
	if( IS_ERR( hd44780_class ) ) {
		pr_err( "hd44780: no udev support\n");
		goto free_cdev;
	}
	hd44780_dev = device_create(hd44780_class,NULL,hd44780_dev_number,
		NULL, "%s", "hd44780" );
	dev_info( hd44780_dev, "mod_init called\n" );
	if (display_init()==0)
		return 0;
free_cdev:
	kobject_put( &driver_object->kobj );
free_device_number:
	unregister_chrdev_region( hd44780_dev_number, 1 );
	printk("mod_init failed\n");
	return -EIO;
}

static void __exit mod_exit( void )
{
	dev_info( hd44780_dev, "mod_exit called\n" );
	display_exit();
	device_destroy( hd44780_class, hd44780_dev_number );
	class_destroy( hd44780_class );
	cdev_del( driver_object );
	unregister_chrdev_region( hd44780_dev_number, 1 );
	return;
}

module_init( mod_init );
module_exit( mod_exit );
MODULE_LICENSE("GPL");
