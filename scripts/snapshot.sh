#!/bin/bash
# Снапшот проекта для сохранения между сессиями/песочницами.
#
# Собирает tar.gz: исходники дерева (без build*/), артефакты релиза
# (NRO + ELF парой — для символизации крашей), PROGRESS.md, README.
#
# Использование: scripts/snapshot.sh [ревизия]   (по умолчанию из CMake VERSION)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REV="${1:-1.00d-r3}"
OUT="${ROOT}/../th08-switch-snapshot-${REV}.tar.gz"

cd "${ROOT}"
if [ ! -f build-switch/th08.nro ]; then
    echo "Нет build-switch/th08.nro — сначала scripts/build_switch.sh" >&2
    exit 1
fi

mkdir -p artifacts
cp -f build-switch/th08.nro "artifacts/th08-${REV}.nro"
cp -f build-switch/th08.elf "artifacts/th08-${REV}.elf"
cp -f build-switch/th08.nacp "artifacts/th08-${REV}.nacp"

tar -czf "${OUT}" \
    --exclude='build*' \
    --exclude='.git' \
    --exclude='scripts/prefix' \
    --exclude='*.o' \
    -C "$(dirname "${ROOT}")" \
    "$(basename "${ROOT}")"

echo "Снапшот: ${OUT}"
ls -la "${OUT}"
