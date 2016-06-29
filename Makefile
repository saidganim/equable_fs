CURRENT = $(shell uname -r)
KDIR =  /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)
TARGET = eqbl_fat_module

obj-m := $(TARGET).o


all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean