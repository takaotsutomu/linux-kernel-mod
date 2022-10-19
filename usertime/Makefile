PWD := $(CURDIR)
CC := gcc

obj-m += usertime.o

all: clean modules userapp

modules:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

userapp: test_userapp.c test_userapp.h
	$(CC) -o userapp test_userapp.c

.PHONY: clean
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf userapp *.o *.ko *.mod.c Module.symvers modules.order