# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build wrapper for the RPBridge Rust kernel driver.
#
# Typical invocations:
#
#   make                          # build against the running kernel
#   make KDIR=/path/to/kheaders   # build against specific headers
#   make clean
#   make modules_install          # install into /lib/modules/$kver/extra
#
# DKMS passes KERNELRELEASE + $(KDIR) on its own and calls `make -C $(KDIR)
# M=$(PWD) modules` — this Makefile supports both flows.

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# Verify CONFIG_RUST is enabled on the target kernel before trying to
# compile. Rust-for-Linux won't build Rust modules otherwise, and the
# error message from Kbuild in that case is opaque.
.PHONY: preflight
preflight:
	@if [ ! -f "$(KDIR)/include/generated/rustc_cfg" ] && \
	    ! grep -q "^CONFIG_RUST=y" "$(KDIR)/.config" 2>/dev/null; then \
	    echo "error: target kernel at $(KDIR) does not have CONFIG_RUST=y" >&2; \
	    echo "       Rebuild the kernel with Rust-for-Linux enabled, or"  >&2; \
	    echo "       point KDIR at a kernel that has Rust support."       >&2; \
	    exit 2; \
	fi

# ensure Kbuild can find a lib.rs at the module root — symlink once.
.PHONY: _link
_link:
	@test -e rpbridge.rs || ln -sf src/lib.rs rpbridge.rs

.PHONY: all
all: preflight _link
	$(MAKE) -C $(KDIR) M=$(PWD) modules

.PHONY: modules
modules: all

.PHONY: modules_install
modules_install: preflight _link
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f rpbridge.rs
	rm -f *.o *.ko *.mod *.mod.c .*.cmd modules.order Module.symvers
	find . -name '*.rmeta' -delete

# Convenience: local clippy pass (does NOT use the kernel's rustc; checks
# syntactic/hygiene issues on the userspace-visible portions only).
.PHONY: clippy-host
clippy-host:
	@echo "note: kernel Rust code cannot be fully linted outside a kernel build."
	@echo "      Use `make C=1 W=1` against a Rust-enabled kernel for full checks."
