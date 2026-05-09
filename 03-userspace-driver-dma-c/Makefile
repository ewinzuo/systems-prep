ifneq ($(KERNELRELEASE),)
# Invoked from kbuild — just declare the module
obj-m := nic.o

else
# Invoked from command line — call into kernel build system
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
