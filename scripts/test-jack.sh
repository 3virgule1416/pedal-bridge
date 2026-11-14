#!/usr/bin/env bash
# Test du client JACK, sans toucher à PiPedal ni à ton serveur JACK :
#   - serveur JACK séparé « pbtest » (pilote dummy, 48 kHz, période 256) ;
#   - tampons dans un dossier temporaire (PB_SHM_DIR), pas dans /dev/shm ;
#   - pb_fakehost joue PiPedal avec les vrais plugins, à une horloge décalée ;
#   - pedal_bridge relie sendN -> returnN dans JACK (--loopback).
#
#   bash scripts/test-jack.sh [durée par scénario en s] [ppm...]
#   ex. : bash scripts/test-jack.sh 40 0 300 -300
set -u

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "PB_JACK_PERIOD='${PB_JACK_PERIOD:-}' PB_CLIENT_OPTS='${PB_CLIENT_OPTS:-}' bash '$REMOTE/src/scripts/test-jack.sh' $*"
fi

DUR="${1:-40}"; shift || true
PPMS="${*:-0 300 -300}"
WARM=$(( DUR / 2 ))
B="$HOME/$REMOTE/build"
SO="$B/lv2/pedal-bridge.lv2/pedal_bridge.so"
BUNDLE="$B/lv2/pedal-bridge.lv2/"
SRV=pbtest

export PB_SHM_DIR="$(mktemp -d /tmp/pb-jack-XXXXXX)"
JPID=""; CPID=""
cleanup() {
  [ -n "$CPID" ] && kill "$CPID" 2>/dev/null && wait "$CPID" 2>/dev/null
  [ -n "$JPID" ] && kill "$JPID" 2>/dev/null && wait "$JPID" 2>/dev/null
  # Seulement les fichiers créés par ce test, dans son propre dossier temporaire.
  rm -f "$PB_SHM_DIR"/pedal-bridge-L* "$PB_SHM_DIR"/*.log
  rmdir "$PB_SHM_DIR" 2>/dev/null
}
trap cleanup EXIT

echo "=== Serveur JACK de test « $SRV »"
PERIOD="${PB_JACK_PERIOD:-256}"
echo "    période JACK $PERIOD, options client : ${PB_CLIENT_OPTS:-(aucune)}"
jackd -n "$SRV" -R -d dummy -r 48000 -p "$PERIOD" > "$PB_SHM_DIR/jackd.log" 2>&1 &
JPID=$!
for i in $(seq 50); do jack_lsp -s "$SRV" > /dev/null 2>&1 && break; sleep 0.1; done
jack_lsp -s "$SRV" > /dev/null 2>&1 || { echo "ÉCHEC : jackd ne démarre pas"; cat "$PB_SHM_DIR/jackd.log"; exit 1; }

echo "=== Client pedal_bridge (boucle send1 -> return1)"
"$B/pedal_bridge" --server "$SRV" --links 1 --loopback --stats 10 ${PB_CLIENT_OPTS:-} >"$PB_SHM_DIR/client.log" 2>&1 &
CPID=$!

FAIL=0
for ppm in $PPMS; do
  echo
  echo "=== Scénario : PiPedal à $ppm ppm de JACK, $DUR s (chauffe $WARM s)"
  "$B/pb_fakehost" "$SO" "$BUNDLE" --ppm "$ppm" --seconds "$DUR" --warmup "$WARM" --report 10 || FAIL=1
  kill -0 "$CPID" 2>/dev/null || { echo "ÉCHEC : le client s'est arrêté"; FAIL=1; break; }
done

echo
echo "=== Contrôle du détecteur : un clic injecté exprès doit être vu"
if out="$("$B/pb_fakehost" "$SO" "$BUNDLE" --seconds 12 --warmup 6 --report 100 --click-at 9)"; then
  echo "$out" | tail -2
  echo "ÉCHEC : le clic injecté n'a pas été détecté"; FAIL=1
else
  echo "$out" | grep TOTAL
  echo "  ok    clic injecté détecté"
fi

echo
echo "=== Journal du client (dernières lignes)"
kill -INT "$CPID" 2>/dev/null; wait "$CPID" 2>/dev/null; CPID=""
tail -n 25 "$PB_SHM_DIR/client.log"
echo
if [ "$FAIL" -eq 0 ]; then echo "=== TEST JACK OK"; else echo "=== TEST JACK EN ÉCHEC"; fi
exit "$FAIL"
