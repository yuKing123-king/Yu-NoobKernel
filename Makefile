# 工具链设置 — RISC-V 64
TOOL_PREFIX_RV ?= riscv64-unknown-elf-
CC_RV = $(TOOL_PREFIX_RV)gcc
LD_RV = $(TOOL_PREFIX_RV)ld
OBJCOPY_RV = $(TOOL_PREFIX_RV)objcopy
OBJDUMP_RV = $(TOOL_PREFIX_RV)objdump
GDB = gdb-multiarch

# 工具链设置 — LoongArch 64 (比赛要求，可后续配置)
TOOL_PREFIX_LA ?= loongarch64-unknown-linux-gnu-
CC_LA = $(TOOL_PREFIX_LA)gcc
LD_LA = $(TOOL_PREFIX_LA)ld
OBJCOPY_LA = $(TOOL_PREFIX_LA)objcopy
OBJDUMP_LA = $(TOOL_PREFIX_LA)objdump

QEMU_RV = qemu-system-riscv64
QEMU_LA = qemu-system-loongarch64

# 编译参数设置
ARCH ?= QEMU
ROOT_DIR := $(shell pwd)
SRC_DIR := $(ROOT_DIR)/src
BUILD_DIR := $(ROOT_DIR)/build/$(ARCH)
INCLUDES := $(ROOT_DIR)/include/
RULES_MK := $(ROOT_DIR)/rules.mk
MODULES := boot misc mm hal trap task sync fs ipc syscall
MODULE_DIRS := $(foreach m,$(MODULES),$(BUILD_DIR)/$(m))
MODULE_MAKEFILES := $(foreach m,$(MODULES),$(SRC_DIR)/$(m)/Makefile)
MODULE_OBJS := $(foreach d,$(MODULE_DIRS),$(d).o)
TARGET := $(BUILD_DIR)/kernel

# 比赛输出文件 (必须在项目根目录)
KERNEL_RV := $(ROOT_DIR)/kernel-rv
KERNEL_LA := $(ROOT_DIR)/kernel-la
DISK_IMG := $(ROOT_DIR)/disk.img

# RISC-V 编译参数
C_FLAGS := -Wall -Werror -O -O0 -fno-omit-frame-pointer -ggdb -std=gnu11
C_FLAGS += -Wno-unused-variable -Wno-unused-function
C_FLAGS += -MMD -MP
C_FLAGS += -c
C_FLAGS += -mcmodel=medany
C_FLAGS += -ffreestanding -fno-common -nostdlib -mno-relax -nostdinc
C_FLAGS += -I $(INCLUDES)
C_FLAGS += $(shell $(CC_RV) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
C_FLAGS += -D $(ARCH)

# Disable PIE when possible
ifneq ($(shell $(CC_RV) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
C_FLAGS += -fno-pie -no-pie
endif

AS_FLAGS := $(C_FLAGS)

LD_FLAGS := -z max-page-size=4096
LD_FLAGS += -T $(ROOT_DIR)/scripts/kernel.ld

CC = $(CC_RV)
AS = $(CC_RV)
LD = $(LD_RV)
OBJCOPY = $(OBJCOPY_RV)
OBJDUMP = $(OBJDUMP_RV)

# 运行参数设置 (本地调试用)
GDB_PORT = 15234
QEMU_FLAGS = -nographic -machine virt -m 128M -d unimp -kernel $(TARGET)
QEMU_FLAGS += -global virtio-mmio.force-legacy=false
QEMU_FLAGS += -drive file=fs.img,if=none,format=raw,id=x0
QEMU_FLAGS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_FLAGS += -netdev user,id=net0
QEMU_FLAGS += -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1

QEMUGDB = -gdb tcp::$(GDB_PORT)

# 辅助函数
log = @printf '  %-10s %s\n' '$1' '$(patsubst $(SRC_DIR)/%,%,$(patsubst $(BUILD_DIR)/%,%,$2))'

.PHONY: all clean run debug vs-debug kernel-rv kernel-la disk.img test-fs

# ============================================================
# 比赛目标: all → 生成 kernel-rv + kernel-la + disk.img
# ============================================================
all: $(KERNEL_RV) $(KERNEL_LA) $(DISK_IMG)

# RISC-V 内核
$(KERNEL_RV): $(TARGET)
	$(call log,CP,$@)
	@cp $< $@

# LoongArch 内核 (占位，待实现 LA 架构支持)
$(KERNEL_LA):
	$(call log,STUB,$@)
	@echo "# stub: LoongArch kernel not yet implemented" > $@

# 辅助磁盘镜像 (EXT4, 可选)
$(DISK_IMG):
	$(call log,IMG,$@)
	dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null
	if command -v mkfs.ext4 >/dev/null 2>&1; then mkfs.ext4 -F $@ >/dev/null 2>&1; else echo "warn: mkfs.ext4 not found, disk.img is raw zero"; fi
$(TARGET): $(MODULE_OBJS) $(BUILD_DIR)/initcode.o
	$(call log,LD,$@)
	@$(LD) $^ -o $@ $(LD_FLAGS)
	$(call log,OBJDUMP,$@.asm)
	@$(OBJDUMP) -S $(TARGET) > $@.asm
	$(call log,OBJDUMP,$@.sym)
	@$(OBJDUMP) -t $(TARGET) | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $@.sym

# ============================================================
# 用户态 init 程序 (C 语言测试运行器)
# ============================================================
INIT_C_SRC := $(SRC_DIR)/user/init.c
INIT_C_OBJ := $(BUILD_DIR)/init.c.o
INIT_ELF := $(BUILD_DIR)/init.elf

$(INIT_C_OBJ): $(INIT_C_SRC)
	@mkdir -p $(@D)
	$(call log,CC,$@)
	@$(CC) $(C_FLAGS) -fno-builtin -c $< -o $@

$(INIT_ELF): $(INIT_C_OBJ)
	$(call log,LD,$@)
	@$(LD) -T $(ROOT_DIR)/scripts/init.ld -N -nostdlib $< -o $@

$(BUILD_DIR)/initcode.bin: $(INIT_ELF)
	$(call log,OBJCOPY,$@)
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/initcode.o: $(BUILD_DIR)/initcode.bin
	@mkdir -p $(@D) $(BUILD_DIR)/initcode_tmp
	$(call log,LD,$@)
	@cp $< $(BUILD_DIR)/initcode_tmp/z
	@cd $(BUILD_DIR)/initcode_tmp && $(LD) -r -b binary z -o $@
	@rm -rf $(BUILD_DIR)/initcode_tmp

# ============================================================
# 用户态测试 ELF (用于验证 fork/execve/wait4)
# ============================================================
HELLO_C_SRC := $(SRC_DIR)/user/hello.c
HELLO_C_OBJ := $(BUILD_DIR)/hello.c.o
HELLO_ELF  := $(BUILD_DIR)/hello.elf

PIPE_C_SRC := $(SRC_DIR)/user/pipe_test.c
PIPE_C_OBJ := $(BUILD_DIR)/pipe_test.c.o
PIPE_ELF  := $(BUILD_DIR)/pipe_test.elf

SYSCALL_C_SRC := $(SRC_DIR)/user/syscall_test.c
SYSCALL_C_OBJ := $(BUILD_DIR)/syscall_test.c.o
SYSCALL_ELF  := $(BUILD_DIR)/syscall_test.elf

$(HELLO_C_OBJ): $(HELLO_C_SRC)
	@mkdir -p $(@D)
	$(call log,CC,$@)
	@$(CC) $(C_FLAGS) -fno-builtin -c $< -o $@

$(HELLO_ELF): $(HELLO_C_OBJ)
	$(call log,LD,$@)
	@$(LD) -T $(ROOT_DIR)/scripts/init.ld -N -nostdlib $< -o $@

$(PIPE_C_OBJ): $(PIPE_C_SRC)
	@mkdir -p $(@D)
	$(call log,CC,$@)
	@$(CC) $(C_FLAGS) -fno-builtin -c $< -o $@

$(PIPE_ELF): $(PIPE_C_OBJ)
	$(call log,LD,$@)
	@$(LD) -T $(ROOT_DIR)/scripts/init.ld -N -nostdlib $< -o $@

$(SYSCALL_C_OBJ): $(SYSCALL_C_SRC)
	@mkdir -p $(@D)
	$(call log,CC,$@)
	@$(CC) $(C_FLAGS) -fno-builtin -c $< -o $@

$(SYSCALL_ELF): $(SYSCALL_C_OBJ)
	$(call log,LD,$@)
	@$(LD) -T $(ROOT_DIR)/scripts/init.ld -N -nostdlib $< -o $@

# ============================================================
# 本地调试目标
# ============================================================
clean:
	$(call log,RM,$(ARCH))
	@rm -rf $(BUILD_DIR)
	@rm -f $(KERNEL_RV) $(KERNEL_LA) $(DISK_IMG)

run: $(TARGET)
	$(call log,RUN,$(ARCH))
	@$(QEMU_RV) $(QEMU_FLAGS)

test-fs: $(HELLO_ELF) $(PIPE_ELF) $(SYSCALL_ELF) fs.img
	@echo "Copying test ELF to fs.img..."
	@sudo mkdir -p /tmp/noob_testfs
	@sudo mount -o loop fs.img /tmp/noob_testfs 2>/dev/null && \
			sudo cp $(HELLO_ELF) /tmp/noob_testfs/hello_test && \
			sudo cp $(PIPE_ELF) /tmp/noob_testfs/pipe_test && \
			sudo cp $(SYSCALL_ELF) /tmp/noob_testfs/syscall_test && \
			sudo umount /tmp/noob_testfs && \
			sudo rmdir /tmp/noob_testfs && \
			echo "Done: hello_test, pipe_test added to fs.img" || \
			echo "Failed: mount fs.img manually"

debug:
	$(call log,DEBUG,$(ARCH))
	$(QEMU_RV) $(QEMU_FLAGS) -S $(QEMUGDB) &
	@while ! nc -zv localhost $(GDB_PORT) 2>/dev/null; do sleep 0.5; done
	@echo "GDB     connecting..."
	@$(GDB)

vs-debug:
	@echo "按F5 启动vscode可视化调试"
	@$(QEMU_RV) $(QEMU_FLAGS) -S $(QEMUGDB)

include $(MODULE_MAKEFILES)
