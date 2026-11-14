#!/usr/bin/env bash
# Test de synchro SooperLooper <- pb_timebase, sur un serveur JACK séparé
# (ta session n'est pas touchée). Note, à la trame près, quand SooperLooper
# commence et finit d'enregistrer, et où tombe le début de la boucle par
# rapport au 1 de la mesure.
#
#   bash scripts/sl-sync-test.sh [bpm, défaut 100]
set -u
HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "SL_EXTRA='${SL_EXTRA:-}' bash '$REMOTE/src/scripts/sl-sync-test.sh' $*"
fi

BPM="${1:-100}"
B="$HOME/$REMOTE/build"
PY="$HOME/bridge-env/bin/python"
export JACK_DEFAULT_SERVER=pbtest
OSC=9975
TMP="$(mktemp -d /tmp/pb-sync-XXXXXX)"
PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done
  wait 2> /dev/null
  rm -f "$TMP"/*.log; rmdir "$TMP" 2> /dev/null
}
trap cleanup EXIT

jackd -n pbtest -R -d dummy -r 48000 -p 128 > "$TMP/jackd.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do jack_lsp > /dev/null 2>&1 && break; sleep 0.1; done
"$B/pb_timebase" --server pbtest --bpm "$BPM" --port 9981 > "$TMP/tb.log" 2>&1 & PIDS+=($!)
sooperlooper -S pbtest -j sltest -p "$OSC" -l 1 -c 2 > "$TMP/sl.log" 2>&1 & PIDS+=($!)
sleep 2

# Réglages comme ta session (+ SL_EXTRA="param=valeur ..." pour essayer autre chose)
"$PY" - "$OSC" ${SL_EXTRA:-} <<'PY'
import sys
from pythonosc.udp_client import SimpleUDPClient
c = SimpleUDPClient("127.0.0.1", int(sys.argv[1]))
c.send_message("/set", ["sync_source", -1.0])
c.send_message("/set", ["eighth_per_cycle", 8.0])
for k, v in (("sync", 1), ("quantize", 1), ("round", 1)):
    c.send_message("/sl/0/set", [k, float(v)])
for kv in sys.argv[2:]:
    k, v = kv.split("=")
    c.send_message("/sl/0/set", [k, float(v)])
    print(f"  réglage en plus : {k} = {v}")
PY

BAR=$(awk -v b="$BPM" 'BEGIN{print 240/b}')
echo "Tempo $BPM : une mesure = $BAR s"
"$B/pb_transport" stop; "$B/pb_transport" zero; sleep 0.3

echo
echo "== Essai 1 : record armé transport arrêté, puis play"
"$B/pb_slhit" "$OSC" 0 record 0.5
"$B/pb_transport" start
"$B/pb_slhit" "$OSC" 0 none 0.01 > /dev/null 2>&1   # (rien : on laisse tourner)
sleep "$(awk -v b="$BAR" 'BEGIN{print b*1.6}')"
"$B/pb_slhit" "$OSC" 0 record "$(awk -v b="$BAR" 'BEGIN{print b*1.5}')"
"$B/pb_slphase" "$OSC" 0 2
"$B/pb_slhit" "$OSC" 0 undo_all 0.3 > /dev/null
"$B/pb_transport" stop; sleep 0.3

echo
echo "== Essai 2 : en lecture, record vers le milieu de la mesure 2"
"$B/pb_transport" zero; "$B/pb_transport" start
sleep "$(awk -v b="$BAR" 'BEGIN{print b*1.4}')"
"$B/pb_slhit" "$OSC" 0 record "$(awk -v b="$BAR" 'BEGIN{print b*1.2}')"
sleep "$(awk -v b="$BAR" 'BEGIN{print b*1.0}')"
"$B/pb_slhit" "$OSC" 0 record "$(awk -v b="$BAR" 'BEGIN{print b*1.5}')"
"$B/pb_slphase" "$OSC" 0 2
"$B/pb_slhit" "$OSC" 0 undo_all 0.3 > /dev/null

echo
echo "== Essai 3 : changement de tempo en cours de route ($BPM -> 120 -> $BPM), puis record"
"$B/pb_timebase" --port 9981 --set 120 > /dev/null; sleep 3.1
"$B/pb_timebase" --port 9981 --set "$BPM" > /dev/null; sleep 1.3
"$B/pb_slhit" "$OSC" 0 record "$(awk -v b="$BAR" 'BEGIN{print b*1.2}')"
sleep "$(awk -v b="$BAR" 'BEGIN{print b*1.0}')"
"$B/pb_slhit" "$OSC" 0 record "$(awk -v b="$BAR" 'BEGIN{print b*1.5}')"
"$B/pb_slphase" "$OSC" 0 2
"$B/pb_transport" stop
