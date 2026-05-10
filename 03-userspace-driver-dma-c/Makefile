ifneq ($(KERNELRELEASE),)
obj-m := nic.o

else
KVER := $(shell uname -r)
KDIR := $(shell \
    if [ -e /lib/modules/$(KVER)/build ]; then echo /lib/modules/$(KVER)/build; \
    else ls -d /nix/store/*-linux-$(KVER)-dev/lib/modules/$(KVER)/build 2>/dev/null | head -1; \
    fi)

.PHONY: all clean

all:
ifeq ($(KDIR),)
	$(error Kernel headers not found for $(KVER))
endif
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
