#!/usr/bin/env bash
# Tests de pedal-bridge. Lancé depuis le PC, il se relance tout seul sur le Pi.
#
#   bash scripts/test.sh
#
# 1. Tests unitaires (ctest) : tampon circulaire et plugin chargé par dlopen.
# 2. Lecture des TTL par lilv (lv2info) : ce que PiPedal verra.
# 3. Vérification temps réel : aucun appel système pendant run() (strace).
set -euo pipefail

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"

if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "bash '$REMOTE/src/scripts/test.sh'"
fi

B="$HOME/$REMOTE/build"
BUNDLE="$B/lv2/pedal-bridge.lv2"
SO="$BUNDLE/pedal_bridge.so"
FAIL=0

echo "=== 1. Tests unitaires"
(cd "$B" && ctest --output-on-failure) || FAIL=1

echo
echo "=== 2. TTL lus par lilv"
for uri in http://pedal-bridge.local/plugins/send http://pedal-bridge.local/plugins/return; do
  out="$(LV2_PATH="$B/lv2:/usr/lib/lv2" lv2info "$uri" 2>&1)" || true
  if echo "$out" | grep -q "Name:.*Davis Loop Station" && echo "$out" | grep -q "lv2core#enumeration" \
     && [ "$(echo "$out" | grep -cE '"Link [1-8]"|Link [1-8]')" -ge 8 ]; then
    echo "  ok    $uri"
    echo "$out" | grep -E "^\s+(Name|Symbol|Minimum|Maximum|Default|Type|Properties|Scale Points)" \
                | sed 's/^/        /' | head -60
  else
    echo "  ÉCHEC $uri"; echo "$out"; FAIL=1
  fi
done

for uri in http://pedal-bridge.local/plugins/looper-mono http://pedal-bridge.local/plugins/looper-stereo; do
  out="$(LV2_PATH="$B/lv2:/usr/lib/lv2" lv2info "$uri" 2>&1)" || true
  if echo "$out" | grep -q "Name:.*Davis Loop Station Looper" && [ "$(echo "$out" | grep -c 'port-props#trigger')" -eq 7 ]      && echo "$out" | grep -q '"recording"' && echo "$out" | grep -q '"240"'; then
    echo "  ok    $uri (7 boutons, états, tempo)"
  else
    echo "  ÉCHEC $uri"; echo "$out" | head -40; FAIL=1
  fi
done
for uri in http://pedal-bridge.local/plugins/drum-mono http://pedal-bridge.local/plugins/drum-stereo; do
  out="$(LV2_PATH="$B/lv2:/usr/lib/lv2" lv2info "$uri" 2>&1)" || true
  if echo "$out" | grep -q "Name:.*Davis Loop Station Drum" && [ "$(echo "$out" | grep -c 'port-props#trigger')" -eq 4 ]      && echo "$out" | grep -q '"Groove 16"' && echo "$out" | grep -q '"no bridge"'; then
    echo "  ok    $uri (4 boutons, grooves, liaison)"
  else
    echo "  ÉCHEC $uri"; echo "$out" | head -40; FAIL=1
  fi
done

echo
echo "=== 3. Temps réel : appels système pendant run()"
for t in test_plugin test_return test_looper test_drum; do
  LOG="$(mktemp /tmp/pb-strace-XXXXXX)"
  if strace -f -qq -o "$LOG" "$B/$t" "$SO" "$BUNDLE/" --rt-loop > /dev/null; then
    # Lignes entre les deux marqueurs getppid().
    n=$(awk '/getppid\(/{m++; next} m==1' "$LOG" | wc -l)
    markers=$(grep -c 'getppid(' "$LOG" || true)
    if [ "$markers" -ne 2 ]; then
      echo "  ÉCHEC $t : marqueurs strace introuvables ($markers au lieu de 2)"; FAIL=1
    elif [ "$n" -eq 0 ]; then
      echo "  ok    $t : aucun appel système ni allocation pendant run() (changements de réglages, boutons, sous-débits)"
    else
      echo "  ÉCHEC $t : $n appel(s) système pendant run() :"
      awk '/getppid\(/{m++; next} m==1' "$LOG" | head -20
      FAIL=1
    fi
  else
    echo "  ÉCHEC $t --rt-loop (allocation pendant run() ?)"; FAIL=1
  fi
  rm -f "$LOG"
done

echo
if [ "$FAIL" -eq 0 ]; then echo "=== TOUT EST OK"; else echo "=== DES TESTS ONT ÉCHOUÉ"; fi
exit "$FAIL"
