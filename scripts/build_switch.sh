#!/bin/bash
# Сборка th08.nro для Nintendo Switch.
#
# ВАЖНО: switch.specs (libnx) раскрывает %:getenv(DEVKITPRO ...) при каждом
# вызове компилятора/линкера — переменная DEVKITPRO должна быть установлена
# и на этапе конфигурации, и на этапе сборки. Этот скрипт делает это сам.
#
# Использование: scripts/build_switch.sh [build-dir] [доп. флаги ninja...]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build-switch}"
shift 2>/dev/null || true

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TOOLCHAIN="${DEVKITPRO}/cmake/Switch.cmake"

if [ ! -f "${TOOLCHAIN}" ]; then
    echo "devkitPro не найден в ${DEVKITPRO}." >&2
    echo "Запустите scripts/pull_dkp.sh или установите DEVKITPRO." >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f build.ninja ]; then
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" -DCMAKE_BUILD_TYPE=Release "${ROOT}"
fi

exec ninja "$@"
