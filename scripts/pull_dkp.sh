#!/bin/bash
# pull_dkp.sh — вытянуть devkitPro (devkita64 + libnx + switch-portlibs) из образа
# devkitpro/devkita64 через Docker Registry API (зеркала pacman/apt отдают 403).
# Идемпотентен: скачанные слои не качаются повторно. Результат: /opt/devkitpro.
set -euo pipefail

IMAGE="devkitpro/devkita64"
STAGE="/var/tmp/dkp-root"
LAYERS="/var/tmp/dkp-layers"
REG="https://registry-1.docker.io"
AUTH="https://auth.docker.io/token?service=registry.docker.io&scope=repository:${IMAGE}:pull"

mkdir -p "$STAGE" "$LAYERS"

token() { curl -s -m 20 "$AUTH" | python3 -c "import json,sys;print(json.load(sys.stdin)['token'])"; }
TOK="$(token)"

# manifest latest → index → amd64
curl -s -m 20 -H "Authorization: Bearer $TOK" \
     -H "Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json" \
     "$REG/v2/${IMAGE}/manifests/latest" -o /tmp/dkp-index.json
AMD64=$(python3 - <<'EOF'
import json
idx = json.load(open('/tmp/dkp-index.json'))
for m in idx['manifests']:
    p = m.get('platform', {})
    if p.get('architecture') == 'amd64' and p.get('os') == 'linux':
        print(m['digest']); break
EOF
)
echo "[pull_dkp] amd64 manifest: $AMD64"

curl -s -m 20 -H "Authorization: Bearer $TOK" \
     -H "Accept: application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json" \
     "$REG/v2/${IMAGE}/manifests/$AMD64" -o /tmp/dkp-manifest.json

mapfile -t DIGESTS < <(python3 - <<'EOF'
import json
m = json.load(open('/tmp/dkp-manifest.json'))
for l in m['layers']:
    print(l['digest'])
EOF
)
echo "[pull_dkp] слоёв: ${#DIGESTS[@]}"

i=0
for d in "${DIGESTS[@]}"; do
    i=$((i+1))
    f="$LAYERS/layer$i.tar.gz"
    if [ -s "$f" ] && ! tar -tzf "$f" >/dev/null 2>&1; then
        echo "[pull_dkp] слой $i битый, перекачиваю"; rm -f "$f"
    fi
    if [ ! -s "$f" ]; then
        echo "[pull_dkp] качаю слой $i (${d:0:19}...)"
        TOK="$(token)"
        curl -sL --retry 3 --retry-delay 2 -m 900 -H "Authorization: Bearer $TOK" \
             "$REG/v2/${IMAGE}/blobs/$d" -o "$f"
        echo "[pull_dkp] слой $i: $(du -h "$f" | cut -f1)"
    else
        echo "[pull_dkp] слой $i уже на месте ($(du -h "$f" | cut -f1))"
    fi
    echo "[pull_dkp] распаковываю слой $i"
    tar -xzf "$f" -C "$STAGE"
done

echo "[pull_dkp] копирую /opt/devkitpro"
sudo mkdir -p /opt/devkitpro
sudo cp -a "$STAGE/opt/devkitpro/." /opt/devkitpro/

echo "[pull_dkp] содержимое /opt/devkitpro:"
ls /opt/devkitpro/ || true
echo "[pull_dkp] проверка компилятора:"
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc --version | head -1 || echo "GCC НЕ НАЙДЕН"
echo "[pull_dkp] portlibs SDL2:"
ls /opt/devkitpro/portlibs/switch/lib/libSDL2.a 2>/dev/null || echo "libSDL2.a НЕ НАЙДЕН"
echo "DKP_READY"
