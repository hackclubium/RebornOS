# RebornOS -- Milestone 0 ("Cold Boot")
#
# Sacred targets: `make run`, `make test`, `make debug`. Everything here
# is meant to run inside WSL2 (or native Linux) -- the toolchains this
# depends on (a real x86_64-elf-gcc cross-compiler, mingw for the UEFI
# PE target, QEMU+OVMF, mtools/sgdisk for disk images) don't behave
# consistently on native Windows.

CROSS_PREFIX := $(HOME)/opt/cross/bin/x86_64-elf-
KCC          := $(CROSS_PREFIX)gcc
MCC          := x86_64-w64-mingw32-gcc

BUILD        := build
COMMON_INC   := common/include
BOOT_INC     := boot/include $(COMMON_INC)
KERNEL_INC   := kernel/include $(COMMON_INC)

vpath %.c boot/src kernel/src common/src
vpath %.S kernel/src

# ---------------------------------------------------------------------
# Bootloader (PE32+ EFI application, built with mingw)
# ---------------------------------------------------------------------

BOOT_OBJDIR := $(BUILD)/boot
BOOT_SRCS   := main.c serial.c minilib.c
BOOT_OBJS   := $(addprefix $(BOOT_OBJDIR)/,$(BOOT_SRCS:.c=.o))
BOOT_EFI    := $(BUILD)/BOOTX64.EFI

MCFLAGS  := -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
            -Wall -Wextra -std=c17 -MMD -MP $(addprefix -I,$(BOOT_INC))
MLDFLAGS := -e efi_main -Wl,--subsystem,10 -nostdlib

$(BOOT_OBJDIR)/%.o: %.c | $(BOOT_OBJDIR)
	$(MCC) $(MCFLAGS) -c $< -o $@

$(BOOT_OBJDIR):
	mkdir -p $@

$(BOOT_EFI): $(BOOT_OBJS)
	$(MCC) $(MLDFLAGS) -o $@ $(BOOT_OBJS)

# ---------------------------------------------------------------------
# Kernel (ELF64, built with our own x86_64-elf-gcc cross-compiler)
# ---------------------------------------------------------------------

KERNEL_OBJDIR := $(BUILD)/kernel
KERNEL_SRCS   := entry.S kmain.c serial.c minilib.c framebuffer.c font8x8.c panic.c kprintf.c qemu_debug.c pmm.c vmm.c isr_stubs.S idt.c timer.c heap.c scheduler.c context_switch.S
KERNEL_OBJS   := $(addprefix $(KERNEL_OBJDIR)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(KERNEL_SRCS))))
KERNEL_ELF    := $(BUILD)/kernel.elf

KCFLAGS  := -ffreestanding -fno-stack-protector -fno-omit-frame-pointer \
            -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
            -Wall -Wextra -std=c17 -MMD -MP $(addprefix -I,$(KERNEL_INC))
KLDFLAGS := -T kernel/linker.ld -ffreestanding -nostdlib -static -Wl,--build-id=none

$(KERNEL_OBJDIR)/%.o: %.c | $(KERNEL_OBJDIR)
	$(KCC) $(KCFLAGS) -c $< -o $@

$(KERNEL_OBJDIR)/%.o: %.S | $(KERNEL_OBJDIR)
	$(KCC) $(KCFLAGS) -c $< -o $@

$(KERNEL_OBJDIR):
	mkdir -p $@

$(KERNEL_ELF): $(KERNEL_OBJS)
	$(KCC) $(KLDFLAGS) -o $@ $(KERNEL_OBJS)

# ---- Test-mode kernel: same sources, -DREBORNOS_TEST_MODE, separate
# object dir so it never gets mixed up with the normal build. ----

KERNEL_TEST_OBJDIR := $(BUILD)/kernel-test
KERNEL_TEST_OBJS    := $(addprefix $(KERNEL_TEST_OBJDIR)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(KERNEL_SRCS))))
KERNEL_TEST_ELF     := $(BUILD)/kernel-test.elf

$(KERNEL_TEST_OBJDIR)/%.o: %.c | $(KERNEL_TEST_OBJDIR)
	$(KCC) $(KCFLAGS) -DREBORNOS_TEST_MODE -c $< -o $@

$(KERNEL_TEST_OBJDIR)/%.o: %.S | $(KERNEL_TEST_OBJDIR)
	$(KCC) $(KCFLAGS) -c $< -o $@

$(KERNEL_TEST_OBJDIR):
	mkdir -p $@

$(KERNEL_TEST_ELF): $(KERNEL_TEST_OBJS)
	$(KCC) $(KLDFLAGS) -o $@ $(KERNEL_TEST_OBJS)

# ---------------------------------------------------------------------
# ESP staging directories, booted via QEMU's VVFAT driver
# (-drive file=fat:rw:<dir>) rather than a hand-built FAT disk image --
# see tools/mkimage.sh for why.
# ---------------------------------------------------------------------

ESP_DIR       := $(BUILD)/esp
ESP_STAMP     := $(ESP_DIR)/.stamp
ESP_TEST_DIR  := $(BUILD)/esp-test
ESP_TEST_STAMP := $(ESP_TEST_DIR)/.stamp

$(ESP_STAMP): $(BOOT_EFI) $(KERNEL_ELF)
	bash tools/mkimage.sh $(ESP_DIR) $(BOOT_EFI) $(KERNEL_ELF)
	touch $@

$(ESP_TEST_STAMP): $(BOOT_EFI) $(KERNEL_TEST_ELF)
	bash tools/mkimage.sh $(ESP_TEST_DIR) $(BOOT_EFI) $(KERNEL_TEST_ELF)
	touch $@

# ---------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------

.PHONY: all boot kernel image run test debug toolchain clean

all: image

boot: $(BOOT_EFI)
kernel: $(KERNEL_ELF)
image: $(ESP_STAMP)

run: image
	bash tools/run-qemu.sh

test: $(ESP_TEST_STAMP)
	bash tools/test-qemu.sh

debug: image
	bash tools/debug-qemu.sh

toolchain:
	bash toolchain/build-cross-gcc.sh

clean:
	rm -rf $(BUILD)
	mkdir -p $(BUILD)

# Auto-generated header dependencies (via -MMD -MP above) so editing a
# .h file correctly triggers a rebuild of everything that includes it,
# instead of silently linking a stale object file.
-include $(BOOT_OBJS:.o=.d) $(KERNEL_OBJS:.o=.d) $(KERNEL_TEST_OBJS:.o=.d)
