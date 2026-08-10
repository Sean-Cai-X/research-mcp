#!/bin/bash
# 通用制品完整性校验脚本
# 下游流水线拉取制品后强制调用, 任何项目通用
# 用法: ./scripts/verify_artifact.sh <deploy_root> <manifest_file>
# 示例: ./scripts/verify_artifact.sh ./deploy_output ../artifact_manifest.sha256
set -euo pipefail

DEPLOY_ROOT="$1"
MANIFEST_FILE="$2"

if [ -z "${DEPLOY_ROOT}" ] || [ -z "${MANIFEST_FILE}" ]; then
    echo "Usage: $0 <deploy_root> <manifest_file>"
    echo "  deploy_root: 制品平铺部署目录(如 ./deploy_output)"
    echo "  manifest_file: 上游生成的 sha256 清单(如 artifact_manifest.sha256)"
    exit 2
fi

if [ ! -d "${DEPLOY_ROOT}" ]; then
    echo "ERROR: deploy_root not found: ${DEPLOY_ROOT}"
    exit 2
fi

if [ ! -f "${MANIFEST_FILE}" ]; then
    echo "ERROR: manifest_file not found: ${MANIFEST_FILE}"
    exit 2
fi

echo "=== Starting artifact integrity check ==="
echo "deploy_root : ${DEPLOY_ROOT}"
echo "manifest    : ${MANIFEST_FILE}"
echo ""

cd "${DEPLOY_ROOT}"
# sha256sum -c 期望清单中的相对路径以 deploy_root 为根, 清单由 find . 生成所以路径前缀是 ./
sha256sum --check "${MANIFEST_FILE}"
echo ""
echo "[OK] File hash manifest validation passed"

# 运行时功能自检(可选)
export PATH="${DEPLOY_ROOT}/bin:$PATH"
# 在这里追加项目专属二进制自检命令
# example: mytool --version && mytool --selftest

echo "=== All verification complete ==="
