obj-m += vvsfs.o
DEBUG_CFLAGS += -g -DDEBUG
KDIR=/lib/modules/$(shell uname -r)/build

all: debug_mod mkfs.vvsfs

debug_mod:
	$(MAKE) -C $(KDIR) M=$(PWD) modules EXTRA_CFLAGS="$(DEBUG_CFLAGS)"

mkfs.vvsfs: mkfs.vvsfs.c
	gcc -Wall -o $@ $<

kernel_mod:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f mkfs.vvsfs
