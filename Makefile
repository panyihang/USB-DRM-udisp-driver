DRIVER_NAME := udisp_drm
KERNEL_SOURCE_DIR ?= /lib/modules/`uname -r`/build

ccflags-y += -I$(CURDIR)/src
obj-m := $(DRIVER_NAME).o
$(DRIVER_NAME)-objs := src/udisp_drm_usb.o src/tiny_jpeg.o

.PHONY: modules clean install

modules:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=$(CURDIR) modules

install: modules
	install -D -m 0644 $(DRIVER_NAME).ko /lib/modules/`uname -r`/extra/$(DRIVER_NAME).ko
	depmod -a

clean:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=$(CURDIR) clean
