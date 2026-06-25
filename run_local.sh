#!/bin/bash
# ============================================================
# 本地评测环境脚本（与评测机 QEMU 参数保持一致）
# 支持 KERNEL 环境变量：Yu-NoobKernel（默认）或 OSKernel2026-AllNull
# 用法: KERNEL=OSKernel2026-AllNull ./run_local.sh
# ============================================================
set -e

DOCKER_IMAGE="docker.io/zhouzhouyi/os-contest:20260510"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"
QEMU="/opt/qemu-bin-10.0.2/bin/qemu-system-riscv64"

KERNEL_NAME="${KERNEL:-Yu-NoobKernel}"
case "$KERNEL_NAME" in
    Yu-NoobKernel|noob)
        KERNEL_DIR="$PROJECT_DIR"
        KERNEL_BIN="kernel-rv"
        ;;
    OSKernel2026-AllNull|allnull)
        KERNEL_DIR="${PARENT_DIR}/OSKernel2026-AllNull"
        KERNEL_BIN="kernel-rv"
        ;;
    *)
        echo "错误: 未知内核名 '$KERNEL_NAME'"
        echo "支持: Yu-NoobKernel (noob), OSKernel2026-AllNull (allnull)"
        exit 1
        ;;
esac

# 评测机参数（可根据需要调整）
MEM=${MEM:-"128M"}
SMP=${SMP:-"1"}
RUN_TIMEOUT=${RUN_TIMEOUT:-"240"}

KERNEL="${KERNEL_DIR}/${KERNEL_BIN}"
SDCARD="${SDCARD:-}"
if [ -n "$SDCARD" ]; then
    FS_IMG="${KERNEL_DIR}/sdcard-rv.img"
else
    FS_IMG="${KERNEL_DIR}/fs.img"
fi
DISK_IMG="${KERNEL_DIR}/disk.img"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}========================================${NC}"
echo -e "${YELLOW}  本地评测: ${KERNEL_NAME}${NC}"
echo -e "${YELLOW}========================================${NC}"

# 检查必要文件
missing=0
for f in "$KERNEL" "$FS_IMG" "$DISK_IMG"; do
    if [ ! -f "$f" ]; then
        echo -e "${RED}缺少: $f${NC}"
        missing=1
    fi
done

if [ "$missing" -eq 1 ]; then
    echo -e "${YELLOW}请先编译内核：${NC}"
    echo "  KERNEL=${KERNEL_NAME} ./build_in_docker.sh"
    exit 1
fi

# 检查是否要额外构建测试程序
if [ -d "${PARENT_DIR}/comption-env/testsuits-for-oskernel/basic/user/build/riscv64" ]; then
    TEST_BIN_DIR="${PARENT_DIR}/comption-env/testsuits-for-oskernel/basic/user/build/riscv64"
    FS_IMG_NEEDS_UPDATE=0

    # 对比 fs.img 中的 brk 和测试目录中的 brk 是否一致
    TMPDIR=$(mktemp -d)
    debugfs -R "dump /basic/brk ${TMPDIR}/brk_on_img" "${FS_IMG}" 2>/dev/null || FS_IMG_NEEDS_UPDATE=1
    if [ -f "${TMPDIR}/brk_on_img" ]; then
        if ! cmp -s "${TMPDIR}/brk_on_img" "${TEST_BIN_DIR}/brk" 2>/dev/null; then
            FS_IMG_NEEDS_UPDATE=1
        fi
    fi
    rm -rf "${TMPDIR}"

    if [ "$FS_IMG_NEEDS_UPDATE" -eq 1 ]; then
        echo -e "${YELLOW}测试程序已更新，需要重新制作 fs.img...${NC}"
        echo -e "${YELLOW}请执行: KERNEL=${KERNEL_NAME} sudo ./rebuild_fs_img.sh${NC}"
        exit 1
    fi
fi

echo -e "${GREEN}内核:     ${KERNEL} $(ls -lh ${KERNEL} | awk '{print $5}')${NC}"
echo -e "${GREEN}fs.img:   ${FS_IMG} $(ls -lh ${FS_IMG} | awk '{print $5}')${NC}"
echo -e "${GREEN}disk.img: ${DISK_IMG} $(ls -lh ${DISK_IMG} | awk '{print $5}')${NC}"
echo -e "${GREEN}内存:     ${MEM}${NC}"
echo -e "${GREEN}CPU数:    ${SMP}${NC}"
echo -e "${GREEN}超时:     ${RUN_TIMEOUT}s${NC}"
echo ""

OUTFILE="${KERNEL_DIR}/test_output_$(date +%Y%m%d_%H%M%S).log"

# QEMU 命令（默认与评测机一致）
# 设置 FORCE_LEGACY=false 可添加 -global virtio-mmio.force-legacy=false
QEMU_CMD="${QEMU} \
    -machine virt \
    -kernel ${KERNEL} \
    -m ${MEM} \
    -nographic \
    -smp ${SMP} \
    ${BIOS:+-bios ${BIOS}} \
    ${FORCE_LEGACY:+-global virtio-mmio.force-legacy=${FORCE_LEGACY}} \
    -drive file=${FS_IMG},if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -no-reboot \
    -device virtio-net-device,netdev=net \
    -netdev user,id=net \
    -rtc base=utc \
    -drive file=${DISK_IMG},if=none,format=raw,id=x1 \
    -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1"

echo -e "${YELLOW}启动 QEMU 10.0.2（评测机版本）...${NC}"
echo -e "${YELLOW}输出将保存到: ${OUTFILE}${NC}"
echo -e "${YELLOW}按 Ctrl+A 然后 X 可退出 QEMU（如果没卡死）${NC}"
echo ""

# 在 Docker 中运行 QEMU
docker run --rm \
    -v "${KERNEL_DIR}:${KERNEL_DIR}" \
    --entrypoint bash \
    "${DOCKER_IMAGE}" \
    -c "timeout ${RUN_TIMEOUT} ${QEMU_CMD}" 2>&1 | tee "${OUTFILE}"

EXIT_CODE=${PIPESTATUS[0]}

echo ""
if [ "$EXIT_CODE" -eq 124 ]; then
    echo -e "${RED}[超时] QEMU 运行超过 120 秒，被强制终止${NC}"
elif [ "$EXIT_CODE" -ne 0 ]; then
    echo -e "${RED}[退出码 $EXIT_CODE] QEMU 异常退出${NC}"
else
    echo -e "${GREEN}[完成] QEMU 正常退出${NC}"
fi

# 检查输出中是否包含关键信息
echo ""
echo -e "${YELLOW}========== 检查关键输出 ==========${NC}"

if grep -q "PANIC\|Assert Fatal\|Assertion failed" "${OUTFILE}" 2>/dev/null; then
    echo -e "${RED}⚠  发现 PANIC / Assert 失败！${NC}"
fi

if grep -q "OS COMP TEST START basic\|OS COMP TEST GROUP START basic" "${OUTFILE}" 2>/dev/null; then
    echo -e "${GREEN}✓  基本测试已开始${NC}"
fi

if grep -q "OS COMP TEST END basic\|OS COMP TEST GROUP END basic" "${OUTFILE}" 2>/dev/null; then
    echo -e "${GREEN}✓  基本测试已结束${NC}"
fi

if grep -q "ALL TEST PASS" "${OUTFILE}" 2>/dev/null; then
    echo -e "${GREEN}✓  所有测试通过！${NC}"
fi

# 统计 mount 返回值
if grep -o "mount return: [-0-9]*" "${OUTFILE}" 2>/dev/null; then
    echo ""
fi

echo ""
echo -e "${YELLOW}完整输出文件: ${OUTFILE}${NC}"
echo -e "${YELLOW}用 less 查看: less ${OUTFILE}${NC}"
