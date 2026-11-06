# mmap_guard — out-of-tree kernel module (Linux only, needs the running kernel's headers)
#   make          build mmap_guard.ko
#   make clean    remove build output

obj-m := mmap_guard.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
