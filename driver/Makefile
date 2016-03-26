ARCH  := arm
CROSS_COMPILE:=arm-none-eabi-
KDIR:=/home/md/embedded/raspi/linux


ifneq ($(KERNELRELEASE),)
obj-m	:= hd44780.o

else
PWD	:= $(shell pwd)

default:
	$(MAKE)	-C $(KDIR) M=$(PWD) modules
endif

clean:
	rm -rf *.ko *.o .*.cmd .tmp_versions Module.symvers
	rm -rf modules.order *.mod.c
