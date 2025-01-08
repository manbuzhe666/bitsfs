### This is a Makefile for BitsFS of BitsObject.com
### The entry source bitsfs.c
obj-m:= bitsfs.o
bitsfs-m := block.o inode.o dentry.o namei.o super.o
CURRENT_PATH     :=$(shell pwd)             # Current path
LINUX_KERNEL     :=$(shell uname -r)        # Kernel version
LINUX_KERNEL_PATH:=/usr/src/kernels/4.18.0-553.22.1.el8_10.x86_64/   # Kernel headers path

all:
	make -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) modules    # Compile
clean:
	make -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) clean      # Clean