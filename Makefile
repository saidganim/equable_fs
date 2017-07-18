CURRENT = $(shell uname -r)
KDIR =  /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)
TARGET = efat

obj-m := $(TARGET).o

$(TARGET)-objs := eqbl_fat_module.o eqbl_fat.o

build:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
