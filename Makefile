# Top-level out-of-tree build file
# Build all quest modules (add more obj-m lines for other quests)
obj-m += quests/monster/

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

