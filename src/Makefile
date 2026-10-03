obj-m += dram_probe.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install: all
	sudo insmod dram_probe.ko

uninstall:
	sudo rmmod dram_probe

status:
	@cat /proc/dram_probe 2>/dev/null || echo "Module not loaded"

.PHONY: all clean install uninstall status
