ifeq ($(KERNELRELEASE),)
KERNELDIR?= ../linux
PWD:=$(shell pwd)
#CFLAGS += $(DEBFLAGS) -Wall
#CFLAGS += -I$(LDDINC)
all: modules download
download:
	./download.sh
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	rm -rf *.o
	rm -rf *.ko
.PHONY:modules clean
else
	obj-m := lophilo.o
    lophilo-objs :lophilo.o
endif
