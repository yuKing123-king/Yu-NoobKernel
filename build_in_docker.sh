#!/bin/bash
# ============================================================
# 在 Docker 中编译指定内核
# 支持 KERNEL 环境变量：Yu-NoobKernel（默认）或 OSKernel2026-AllNull
# 用法: KERNEL=OSKernel2026-AllNull ./build_in_docker.sh
# ============================================================
set -e

DOCKER_IMAGE="docker.io/zhouzhouyi/os-contest:20260510"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"

KERNEL_NAME="${KERNEL:-Yu-NoobKernel}"
case "$KERNEL_NAME" in
    Yu-NoobKernel|noob)
        KERNEL_DIR="$PROJECT_DIR"
        KERNEL_OUT="kernel-rv"
        ;;
    OSKernel2026-AllNull|allnull)
        KERNEL_DIR="${PARENT_DIR}/OSKernel2026-AllNull"
        KERNEL_OUT="kernel-rv"
        ;;
    *)
        echo "错误: 未知内核名 '$KERNEL_NAME'"
        echo "支持: Yu-NoobKernel (noob), OSKernel2026-AllNull (allnull)"
        exit 1
        ;;
esac

echo "========================================"
echo "  编译内核: ${KERNEL_NAME}"
echo "  目录:     ${KERNEL_DIR}"
echo "========================================"

docker run --rm \
    -v "${KERNEL_DIR}:/workspace" \
    -w /workspace \
    --entrypoint bash \
    "${DOCKER_IMAGE}" \
    -c "make clean && make all"

echo ""
echo "编译完成！"
ls -lh "${KERNEL_DIR}/${KERNEL_OUT}" 2>/dev/null || echo "⚠  ${KERNEL_OUT} 未生成"