#!/bin/bash
# ============================================================
# 重新制作 fs.img：将测试程序拷贝到 ext4 镜像
# 支持 KERNEL 环境变量：Yu-NoobKernel（默认）或 OSKernel2026-AllNull
# 用法: KERNEL=OSKernel2026-AllNull ./rebuild_fs_img.sh
# ============================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"
KERNEL_NAME="${KERNEL:-Yu-NoobKernel}"

case "$KERNEL_NAME" in
    Yu-NoobKernel|noob)
        KERNEL_DIR="$PROJECT_DIR"
        ;;
    OSKernel2026-AllNull|allnull)
        KERNEL_DIR="${PARENT_DIR}/OSKernel2026-AllNull"
        ;;
    *)
        echo "错误: 未知内核名 '$KERNEL_NAME'"
        echo "支持: Yu-NoobKernel (noob), OSKernel2026-AllNull (allnull)"
        exit 1
        ;;
esac

# 支持环境变量 TEST_SUITE_DIR 指定测试套件路径
# 默认在 ../comption-env/testsuits-for-oskernel
TEST_SUITE_DIR="${TEST_SUITE_DIR:-${PARENT_DIR}/comption-env/testsuits-for-oskernel}"
TEST_BIN_DIR="${TEST_SUITE_DIR}/basic/user/build/riscv64"
FS_IMG="${KERNEL_DIR}/fs.img"
DISK_IMG="${KERNEL_DIR}/disk.img"

echo "========================================"
echo "  重新制作 fs.img 和 disk.img"
echo "  内核: ${KERNEL_NAME}"
echo "========================================"

# 检查测试套件
if [ ! -d "${TEST_BIN_DIR}" ]; then
    echo "错误: 测试程序未找到，需要先在 Docker 中编译测试套件"
    echo ""
    echo "  方式1: 用 Docker 编译测试套件"
    echo "  cd ${TEST_SUITE_DIR}/basic/user"
    echo "  docker run --rm -v \$(pwd):/workspace -w /workspace \\"
    echo "      zhouzhouyi/os-contest:20260510 \\"
    echo "      ./build-oscomp.sh"
    echo ""
    echo "  方式2: 手动修改 fs.img（将已有文件拷贝进去）"
    exit 1
fi

# 创建 fs.img（32MB ext4）
echo "创建 fs.img..."
rm -f "${FS_IMG}"
dd if=/dev/zero of="${FS_IMG}" bs=1M count=32 2>/dev/null
mkfs.ext4 -F -O ^metadata_csum,^has_journal "${FS_IMG}" >/dev/null 2>&1

# 创建 disk.img（32MB ext4，为空用于 mount 测试）
echo "创建 disk.img..."
rm -f "${DISK_IMG}"
dd if=/dev/zero of="${DISK_IMG}" bs=1M count=32 2>/dev/null
mkfs.ext4 -F -O ^metadata_csum,^has_journal "${DISK_IMG}" >/dev/null 2>&1

# 挂载并拷贝文件
echo "拷贝测试程序到 fs.img..."
MNT=$(mktemp -d)
sudo mount -o loop "${FS_IMG}" "${MNT}"

# 创建 basic 目录
sudo mkdir -p "${MNT}/basic"
sudo mkdir -p "${MNT}/mnt"

# 拷贝所有测试二进制
echo "  拷贝测试程序..."
count=0
for f in "${TEST_BIN_DIR}"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    # 跳过非可执行文件（如 .o .a 等），但保留 run-all.sh 和 text.txt
    if [[ "$base" == run-all.sh ]] || [[ "$base" == text.txt ]]; then
        sudo cp "$f" "${MNT}/basic/"
        continue
    fi
    if [ -x "$f" ]; then
        sudo cp "$f" "${MNT}/basic/"
        count=$((count + 1))
    fi
done
echo "  共拷贝 ${count} 个测试程序"

# 拷贝必要的脚本文件
if [ -f "${TEST_BIN_DIR}/run-all.sh" ]; then
    sudo cp "${TEST_BIN_DIR}/run-all.sh" "${MNT}/basic/"
fi
if [ -f "${TEST_BIN_DIR}/text.txt" ]; then
    sudo cp "${TEST_BIN_DIR}/text.txt" "${MNT}/basic/"
else
    # fallback: create text.txt from source or with default content
    src_txt="${TEST_SUITE_DIR}/basic/user/src/oscomp/text.txt"
    if [ -f "${src_txt}" ]; then
        sudo cp "${src_txt}" "${MNT}/basic/"
    else
        echo "Hi, this is a text file." | sudo tee "${MNT}/basic/text.txt" >/dev/null
        echo "syscalls testing success!" | sudo tee -a "${MNT}/basic/text.txt" >/dev/null
    fi
fi

# 创建 basic_testcode.sh（如果不存在）
if [ ! -f "${MNT}/basic_testcode.sh" ]; then
    sudo tee "${MNT}/basic_testcode.sh" >/dev/null <<'SCRIPT'
#!/bin/busybox
echo "#### OS COMP TEST GROUP START basic ####"
cd ./basic
./run-all.sh
cd ..
echo "#### OS COMP TEST GROUP END basic ####"
SCRIPT
    sudo chmod +x "${MNT}/basic_testcode.sh"
fi

# 创建测试需要的目录
sudo mkdir -p "${MNT}/test_chdir"
sudo mkdir -p "${MNT}/test_mkdir"
sudo mkdir -p "${MNT}/basic/mnt"

# 创建 test_close.txt test_mmap.txt（空文件）
sudo touch "${MNT}/test_close.txt"
sudo touch "${MNT}/test_mmap.txt"

sudo umount "${MNT}"
rmdir "${MNT}"

echo ""
echo "完成！"
ls -lh "${FS_IMG}" "${DISK_IMG}"
