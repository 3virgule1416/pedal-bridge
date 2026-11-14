#!/usr/bin/env bash
# Affiche les commandes d'installation (n'exécute rien : sudo est fait à la main).
#
#   bash scripts/install.sh
set -euo pipefail

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"

if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "bash '$REMOTE/src/scripts/install.sh'"
fi

BUNDLE="$HOME/$REMOTE/build/lv2/pedal-bridge.lv2"
DEST=/usr/local/lib/lv2/pedal-bridge.lv2

[ -f "$BUNDLE/pedal_bridge.so" ] || { echo "Pas compilé : $BUNDLE/pedal_bridge.so"; exit 1; }

cat <<EOF
# --- À lancer sur le Pi (ssh pi-dev) ---
sudo install -d -m 755 $DEST
sudo install -m 644 $BUNDLE/*.ttl $DEST/
sudo install -m 755 $BUNDLE/pedal_bridge.so $DEST/
# PiPedal relit les plugins au redémarrage (le son coupe quelques secondes) :
sudo systemctl restart pipedald
EOF

if [ -f "$HOME/$REMOTE/build/pedal_bridge" ]; then
cat <<EOF

# --- Client JACK (facultatif : il marche aussi depuis le dossier de compilation) ---
sudo install -m 755 $HOME/$REMOTE/build/pedal_bridge /usr/local/bin/
EOF
fi
