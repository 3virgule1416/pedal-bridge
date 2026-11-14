#!/usr/bin/env bash
# Déploiement PC -> Pi : copie les sources dans ~/pedal-bridge-build/src et
# compile dans ~/pedal-bridge-build/build. N'installe rien dans le système.
#
#   bash scripts/deploy.sh            (hôte par défaut : pi-dev)
#   PB_HOST=autre bash scripts/deploy.sh
set -euo pipefail

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"

cd "$(dirname "$0")/.."

echo "== Copie des sources vers $HOST:~/$REMOTE/src"
tar -cf - CMakeLists.txt README.md common lv2 client tools tests scripts \
  | ssh "$HOST" "mkdir -p '$REMOTE/src' && tar -xf - -m -C '$REMOTE/src'"

echo "== Compilation sur $HOST"
ssh "$HOST" "cd '$REMOTE' && cmake -S src -B build -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build build -j4"

echo "== OK. Pour tester : bash scripts/test.sh"
