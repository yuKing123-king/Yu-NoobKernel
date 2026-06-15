#!/bin/bash
# ============================================================
# 重新制作文件系统镜像
#
# 默认模式：制作 fs.img + disk.img（根目录级 basic/ 布局）
#   KERNEL=OSKernel2026-AllNull ./rebuild_fs_img.sh
#
# SDCARD 模式：制作 sdcard-rv.img（musl/ + glibc/ 子目录布局）
#   SDCARD=1 ./rebuild_fs_img.sh
#
# SDCARD 模式下可以用 DOCKER_BUILD=1 在 Docker 中编译测试程序
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
SDCARD_IMG="${KERNEL_DIR}/sdcard-rv.img"

###############################################################################
# 辅助函数
###############################################################################

# 用 debugfs 将文件写入 ext4 镜像（避免 sudo）
# 用法: debugfs_write <镜像路径> <源文件> <目标路径>
debugfs_write() {
    local img="$1" src="$2" dest="$3"
    debugfs -w -R "rm $dest" "$img" 2>/dev/null || true
    debugfs -w -R "write $src $dest" "$img" >/dev/null 2>&1
}

# 用 debugfs 创建目录
# 用法: debugfs_mkdir <镜像路径> <目录路径>
debugfs_mkdir() {
    local img="$1" dir="$2"
    debugfs -w -R "mkdir $dir" "$img" >/dev/null 2>&1 || true
}

# 创建空的 ext4 镜像
# 用法: create_ext4 <镜像路径> <大小MB>
create_ext4() {
    local img="$1" size_mb="$2"
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1M count="$size_mb" 2>/dev/null
    mkfs.ext4 -F -O ^metadata_csum,^has_journal "$img" >/dev/null 2>&1
}

# 将测试程序拷贝到 ext4 镜像（用 debugfs）
# 用法: copy_tests_to_image <镜像路径> <目标前缀路径> <源目录>
#   e.g. copy_tests_to_image sdcard-rv.img /glibc/basic /path/to/binaries
copy_tests_to_image() {
    local img="$1" prefix="$2" src_dir="$3"

    debugfs_mkdir "$img" "$prefix"
    debugfs_mkdir "$img" "${prefix}/mnt"

    count=0
    for f in "$src_dir"/*; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        if [[ "$base" == run-all.sh ]] || [[ "$base" == text.txt ]]; then
            debugfs_write "$img" "$f" "${prefix}/${base}"
            continue
        fi
        if [ -x "$f" ]; then
            debugfs_write "$img" "$f" "${prefix}/${base}"
            count=$((count + 1))
        fi
    done
    echo "  拷贝 ${count} 个测试程序到 ${prefix}"

    # 拷贝辅助文件
    if [ -f "${src_dir}/run-all.sh" ]; then
        debugfs_write "$img" "${src_dir}/run-all.sh" "${prefix}/run-all.sh"
    fi
    if [ -f "${src_dir}/text.txt" ]; then
        debugfs_write "$img" "${src_dir}/text.txt" "${prefix}/text.txt"
    else
        src_txt="${TEST_SUITE_DIR}/basic/user/src/oscomp/text.txt"
        if [ -f "$src_txt" ]; then
            debugfs_write "$img" "$src_txt" "${prefix}/text.txt"
        fi
    fi
}

###############################################################################
# SDCARD 模式：制作 sdcard-rv.img
###############################################################################

if [ -n "$SDCARD" ]; then
    echo "========================================"
    echo "  重新制作 sdcard-rv.img"
    echo "  内核: ${KERNEL_NAME}"
    echo "========================================"

    # 检查测试套件
    if [ ! -d "${TEST_BIN_DIR}" ]; then
        echo "错误: 测试程序未找到: ${TEST_BIN_DIR}"
        exit 1
    fi

    # 如设置 DOCKER_BUILD=1，在 Docker 中重新编译测试程序
    if [ -n "$DOCKER_BUILD" ]; then
        echo "在 Docker 中编译测试程序..."
        DOCKER_IMAGE="docker.io/zhouzhouyi/os-contest:20260510"

        # musl 版本
        echo "编译 musl 版本..."
        docker run --rm \
            -v "${TEST_SUITE_DIR}:/code" \
            -w /code \
            --entrypoint bash \
            "${DOCKER_IMAGE}" \
            -c "make -f Makefile.sub clean 2>/dev/null; \
                make -f Makefile.sub PREFIX=riscv64-buildroot-linux-musl- DESTDIR=/code/sdcard/riscv/musl basic 2>&1; \
                sed -E -i 's/#### OS COMP TEST GROUP ([^ ]+) ([^ ]+) ####/#### OS COMP TEST GROUP \1 \2-musl ####/g' /code/sdcard/riscv/musl/*_testcode.sh 2>/dev/null || true"

        # glibc 版本
        echo "编译 glibc 版本..."
        docker run --rm \
            -v "${TEST_SUITE_DIR}:/code" \
            -w /code \
            --entrypoint bash \
            "${DOCKER_IMAGE}" \
            -c "make -f Makefile.sub clean 2>/dev/null; \
                make -f Makefile.sub PREFIX=riscv64-linux-gnu- DESTDIR=/code/sdcard/riscv/glibc basic 2>&1; \
                sed -E -i 's/#### OS COMP TEST GROUP ([^ ]+) ([^ ]+) ####/#### OS COMP TEST GROUP \1 \2-glibc ####/g' /code/sdcard/riscv/glibc/*_testcode.sh 2>/dev/null || true"

        MUSL_BIN="${TEST_SUITE_DIR}/sdcard/riscv/musl/basic"
        GLIBC_BIN="${TEST_SUITE_DIR}/sdcard/riscv/glibc/basic"
        MUSL_ROOT="${TEST_SUITE_DIR}/sdcard/riscv/musl"
        GLIBC_ROOT="${TEST_SUITE_DIR}/sdcard/riscv/glibc"
    else
        # 使用现有编译结果
        MUSL_BIN="${TEST_BIN_DIR}"
        GLIBC_BIN="${TEST_BIN_DIR}"
    fi

    # 创建 sdcard-rv.img（256MB）
    echo "创建 sdcard-rv.img..."
    create_ext4 "${SDCARD_IMG}" 256

    # 创建 /musl/basic 目录并拷贝文件
    echo "拷贝 musl 测试程序..."
    debugfs_mkdir "${SDCARD_IMG}" "/musl"
    if [ -d "$MUSL_BIN" ]; then
        copy_tests_to_image "${SDCARD_IMG}" "/musl/basic" "$MUSL_BIN"
    else
        echo "  警告: musl 测试程序未找到: $MUSL_BIN"
    fi

    # 创建 /glibc/basic 目录并拷贝文件
    echo "拷贝 glibc 测试程序..."
    debugfs_mkdir "${SDCARD_IMG}" "/glibc"
    if [ -d "$GLIBC_BIN" ]; then
        copy_tests_to_image "${SDCARD_IMG}" "/glibc/basic" "$GLIBC_BIN"
    else
        echo "  警告: glibc 测试程序未找到: $GLIBC_BIN"
    fi

    # 创建 basic_testcode.sh（musl 版本）
    debugfs_write "${SDCARD_IMG}" /dev/stdin "/musl/basic_testcode.sh" 2>/dev/null <<'SCRIPT'
#!/bin/busybox
echo "#### OS COMP TEST GROUP START basic-musl ####"
cd ./basic
./run-all.sh
cd ..
echo "#### OS COMP TEST GROUP END basic-musl ####"
SCRIPT
    debugfs -w -R "set_inode_field /musl/basic_testcode.sh mode 0100755" "${SDCARD_IMG}" >/dev/null 2>&1

    # 创建 basic_testcode.sh（glibc 版本）
    debugfs_write "${SDCARD_IMG}" /dev/stdin "/glibc/basic_testcode.sh" 2>/dev/null <<'SCRIPT'
#!/bin/busybox
echo "#### OS COMP TEST GROUP START basic-glibc ####"
cd ./basic
./run-all.sh
cd ..
echo "#### OS COMP TEST GROUP END basic-glibc ####"
SCRIPT
    debugfs -w -R "set_inode_field /glibc/basic_testcode.sh mode 0100755" "${SDCARD_IMG}" >/dev/null 2>&1

    # 创建测试所需的目录和文件（在 musl 和 glibc 各一份）
    for prefix in "/musl" "/glibc"; do
        debugfs_mkdir "${SDCARD_IMG}" "${prefix}/basic/mnt"
        debugfs_mkdir "${SDCARD_IMG}" "${prefix}/test_chdir"
        debugfs_mkdir "${SDCARD_IMG}" "${prefix}/test_mkdir"
        # 空文件
        debugfs_write "${SDCARD_IMG}" /dev/null "${prefix}/test_close.txt" 2>/dev/null
        debugfs_write "${SDCARD_IMG}" /dev/null "${prefix}/test_mmap.txt" 2>/dev/null
    done

    echo ""
    echo "完成！"
    ls -lh "${SDCARD_IMG}"
    exit 0
fi

###############################################################################
# 默认模式：制作 fs.img + disk.img
###############################################################################

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
create_ext4 "${FS_IMG}" 32

# 创建 disk.img（32MB ext4，为空用于 mount 测试）
echo "创建 disk.img..."
create_ext4 "${DISK_IMG}" 32

# 用 debugfs 拷贝文件到 fs.img（不需要 sudo）
echo "拷贝测试程序到 fs.img..."
debugfs_mkdir "${FS_IMG}" "/basic"
debugfs_mkdir "${FS_IMG}" "/mnt"

echo "  拷贝测试程序..."
count=0
for f in "${TEST_BIN_DIR}"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    if [[ "$base" == run-all.sh ]] || [[ "$base" == text.txt ]]; then
        debugfs_write "${FS_IMG}" "$f" "/basic/${base}"
        continue
    fi
    if [ -x "$f" ]; then
        debugfs_write "${FS_IMG}" "$f" "/basic/${base}"
        count=$((count + 1))
    fi
done
echo "  共拷贝 ${count} 个测试程序"

# 拷贝必要的脚本文件
if [ -f "${TEST_BIN_DIR}/run-all.sh" ]; then
    debugfs_write "${FS_IMG}" "${TEST_BIN_DIR}/run-all.sh" "/basic/run-all.sh"
fi
if [ -f "${TEST_BIN_DIR}/text.txt" ]; then
    debugfs_write "${FS_IMG}" "${TEST_BIN_DIR}/text.txt" "/basic/text.txt"
else
    src_txt="${TEST_SUITE_DIR}/basic/user/src/oscomp/text.txt"
    if [ -f "${src_txt}" ]; then
        debugfs_write "${FS_IMG}" "${src_txt}" "/basic/text.txt"
    fi
fi

# 创建 basic_testcode.sh（如果不存在）
debugfs -w -R "rm /basic_testcode.sh" "${FS_IMG}" 2>/dev/null || true
debugfs_write "${FS_IMG}" /dev/stdin "/basic_testcode.sh" 2>/dev/null <<'SCRIPT'
#!/bin/busybox
echo "#### OS COMP TEST GROUP START basic ####"
cd ./basic
./run-all.sh
cd ..
echo "#### OS COMP TEST GROUP END basic ####"
SCRIPT
debugfs -w -R "set_inode_field /basic_testcode.sh mode 0100755" "${FS_IMG}" >/dev/null 2>&1

# 创建测试需要的目录
debugfs_mkdir "${FS_IMG}" "/test_chdir"
debugfs_mkdir "${FS_IMG}" "/test_mkdir"
debugfs_mkdir "${FS_IMG}" "/basic/mnt"

# 创建 test_close.txt test_mmap.txt（空文件）
debugfs_write "${FS_IMG}" /dev/null "/test_close.txt" 2>/dev/null
debugfs_write "${FS_IMG}" /dev/null "/test_mmap.txt" 2>/dev/null

echo ""
echo "完成！"
ls -lh "${FS_IMG}" "${DISK_IMG}"
