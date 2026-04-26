# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build wrapper for the RPBridge C kernel driver.
#
# Typical invocations:
#
#   make                          # build against the running kernel
#   make KDIR=/path/to/kheaders   # build against specific headers
#   make clean
#   make modules_install          # install into /lib/modules/$kver/extra
#   make checkpatch               # run scripts/checkpatch.pl on src/
#
# DKMS passes KERNELRELEASE + $(KDIR) on its own and calls
# `make -C $(KDIR) M=$(PWD) modules` — this Makefile supports both flows.

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

.PHONY: all
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

.PHONY: modules
modules: all

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c .*.cmd modules.order Module.symvers
	find src -name '*.o' -delete 2>/dev/null || true
	find src -name '.*.cmd' -delete 2>/dev/null || true

# Static-analysis convenience target. Runs the kernel tree's own
# checkpatch.pl against every C source file under src/ — same script
# the upstream maintainers use during review. Exits non-zero on any
# WARNING/ERROR so CI can gate.
.PHONY: checkpatch
checkpatch:
	@if [ ! -x "$(KDIR)/scripts/checkpatch.pl" ]; then \
	    echo "error: $(KDIR)/scripts/checkpatch.pl not found" >&2; \
	    exit 2; \
	fi
	@find src -name '*.[ch]' -print0 \
	    | xargs -0 $(KDIR)/scripts/checkpatch.pl --no-tree --terse \
	                                              --strict --max-line-length=100 -f
