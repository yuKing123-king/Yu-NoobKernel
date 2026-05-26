# 工具链设置
TOOL_PREFIX = riscv64-unknown-elf-
CC = $(TOOL_PREFIX)gcc
AS = $(TOOL_PREFIX)gcc
LD = $(TOOL_PREFIX)ld
OBJCOPY = $(TOOL_PREFIX)objcopy
OBJDUMP = $(TOOL_PREFIX)objdump
GDB = gdb-multiarch
PY = python3
CP = cp
QEMU = qemu-system-riscv64

# 编译参数设置
ARCH ?= QEMU
INIT_PROC ?= usershell
ROOT_DIR := $(shell pwd)
SRC_DIR := $(ROOT_DIR)/src
BUILD_DIR := $(ROOT_DIR)/build/$(ARCH)
INCLUDES := $(ROOT_DIR)/include/
RULES_MK := $(ROOT_DIR)/rules.mk
MODULES := boot misc mm hal trap task sync fs
MODULE_DIRS := $(foreach m,$(MODULES),$(BUILD_DIR)/$(m))
MODULE_MAKEFILES := $(foreach m,$(MODULES),$(SRC_DIR)/$(m)/Makefile)
MODULE_OBJS := $(foreach d,$(MODULE_DIRS),$(d).o)
TARGET := $(BUILD_DIR)/kernel

C_FLAGS := -Wall -Werror -O -O0 -fno-omit-frame-pointer -ggdb -std=gnu11
C_FLAGS += -Wno-unused-variable -Wno-unused-function
C_FLAGS += -MMD -MP
C_FLAGS += -c
C_FLAGS += -mcmodel=medany
C_FLAGS += -ffreestanding -fno-common -nostdlib -mno-relax -nostdinc
C_FLAGS += -I $(INCLUDES)
C_FLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
C_FLAGS += -D $(ARCH)

# Disable PIE when possible
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
C_FLAGS += -fno-pie -no-pie
endif

AS_FLAGS := $(C_FLAGS)

LD_FLAGS := -z max-page-size=4096
LD_FLAGS += -T $(ROOT_DIR)/scripts/kernel.ld

# 运行参数设置
GDB_PORT = 15234
QEMU_FLAGS = \
	-nographic \
	-machine virt \
	-m 128M \
	-d guest_errors,unimp \
	-kernel $(TARGET)
QEMU_FLAGS += -global virtio-mmio.force-legacy=false
QEMU_FLAGS += -drive file=fs.img,if=none,format=raw,id=x0
QEMU_FLAGS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

QEMUGDB = -gdb tcp::$(GDB_PORT)

#辅助函数
log = @printf '  %-10s %s\n' '$1' '$(patsubst $(SRC_DIR)/%,%,$(patsubst $(BUILD_DIR)/%,%,$2))'

$(TARGET): $(MODULE_OBJS)
	$(call log,LD,$@)
	@$(LD) $^ -o $@ $(LD_FLAGS)
	$(call log,OBJDUMP,$@.asm)
	@$(OBJDUMP) -S $(TARGET) > $@.asm
	$(call log,OBJDUMP,$@.sym)
	@$(OBJDUMP) -t $(TARGET) | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

.PHONY: all clean run debug vs-debug docs

all: $(TARGET)

clean:
	$(call log,RM,$(ARCH))
	@rm -rf $(BUILD_DIR)

run: $(TARGET)
	$(call log,RUN,$(ARCH))
	@$(QEMU) $(QEMU_FLAGS)

debug:
	$(call log,DEBUG,$(ARCH))
	$(QEMU) $(QEMU_FLAGS) -S $(QEMUGDB) &
	@while ! nc -zv localhost $(GDB_PORT) 2>/dev/null; do sleep 0.5; done
	@echo "GDB     connecting..."
	@$(GDB)

vs-debug:
	@echo "按F5 启动vscode可视化调试"
	@$(QEMU) $(QEMU_FLAGS) -S $(QEMUGDB)

include $(MODULE_MAKEFILES)
