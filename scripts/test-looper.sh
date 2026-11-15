#!/usr/bin/env bash
# Banc de test de DLS Looper côté client, sans toucher à PiPedal ni à ta session :
#   - serveur JACK séparé « pbtest » (pilote dummy, 48 kHz, période 128) ;
#   - pb_timebase, SooperLooper (OSC 9961) et pedal_bridge sur ce serveur ;
#   - fichiers partagés dans un dossier temporaire (PB_SHM_DIR) ;
#   - pb_fakehost fait l'audio de PiPedal (Send/Return, lien 1), pb_ctl joue
#     les boutons de DLS Looper.
# Ne pas lancer pendant une session en direct (ports 9980/9982 et CPU).
#
#   bash scripts/test-looper.sh
set -u

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "bash '$REMOTE/src/scripts/test-looper.sh' $*"
fi

B="$HOME/$REMOTE/build"
SO="$B/lv2/pedal-bridge.lv2/pedal_bridge.so"
BUNDLE="$B/lv2/pedal-bridge.lv2/"
SRV=pbtest
SLPORT=9961

for p in pb_timebase pedal_bridge sooperlooper; do
  if pgrep -u "$USER" -x "$p" > /dev/null; then echo "ÉCHEC : $p tourne déjà (session en direct ?). Arrête-la d'abord."; exit 1; fi
done

export PB_SHM_DIR="$(mktemp -d /tmp/pb-looper-XXXXXX)"
WORK="$PB_SHM_DIR/travail"
PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
  # Seulement les fichiers créés par ce test, dans son propre dossier temporaire.
  rm -f "$PB_SHM_DIR"/pedal-bridge-L* "$PB_SHM_DIR"/pedal-bridge-control "$PB_SHM_DIR"/*.log
  rm -rf "$WORK"
  rmdir "$PB_SHM_DIR" 2>/dev/null
}
trap cleanup EXIT

FAIL=0
check() {   # check "description" commande...
  local what=$1; shift
  if "$@"; then echo "  ok    $what"; else echo "  ÉCHEC $what"; FAIL=1; fi
}
show() { "$B/pb_ctl" show; }
# Attend (max $2 s) que « pb_ctl show » contienne le motif $1.
wait_show() {
  local t=$(( ${2:-5} * 10 ))
  for _ in $(seq "$t"); do show | grep -q -- "$1" && return 0; sleep 0.1; done
  echo "        (dernier état : $(show))"
  return 1
}

echo "=== Serveur JACK de test « $SRV »"
jackd -n "$SRV" -R -d dummy -r 48000 -p 128 > "$PB_SHM_DIR/jackd.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do jack_lsp -s "$SRV" > /dev/null 2>&1 && break; sleep 0.1; done
jack_lsp -s "$SRV" > /dev/null 2>&1 || { echo "ÉCHEC : jackd ne démarre pas"; exit 1; }

"$B/pb_timebase" --server "$SRV" --bpm 120 > "$PB_SHM_DIR/timebase.log" 2>&1 & PIDS+=($!)
# -S ne marche pas dans SooperLooper 1.7.8 : il démarre alors son propre serveur.
JACK_DEFAULT_SERVER="$SRV" JACK_NO_START_SERVER=1 sooperlooper -p "$SLPORT" -l 2 -c 2 -q > "$PB_SHM_DIR/sl.log" 2>&1 & PIDS+=($!)
sleep 1.5
"$HOME/bridge-env/bin/python" - <<PY
from pythonosc.udp_client import SimpleUDPClient
c = SimpleUDPClient("127.0.0.1", $SLPORT)
c.send_message("/set", ["sync_source", -1])
c.send_message("/set", ["eighth_per_cycle", 8.0])
for i in range(2):
    c.send_message(f"/sl/{i}/set", ["sync", 1])
    c.send_message(f"/sl/{i}/set", ["quantize", 1])
    c.send_message(f"/sl/{i}/set", ["round", 1])
PY

echo "=== Audio de PiPedal (pb_fakehost) et « plugin » (pb_ctl)"
"$B/pb_fakehost" "$SO" "$BUNDLE" --seconds 90 --warmup 1 --report 1000 > "$PB_SHM_DIR/fakehost.log" 2>&1 & PIDS+=($!)
"$B/pb_ctl" create > /dev/null
sleep 0.5

echo "=== Client pedal_bridge"
"$B/pedal_bridge" --server "$SRV" --links 1 --stats 0 --sl-osc "$SLPORT" --dir "$PB_SHM_DIR" \
  > "$PB_SHM_DIR/client.log" 2>&1 & PIDS+=($!)
sleep 1
jack_connect -s "$SRV" pedal_bridge:send1_L sooperlooper:common_in_1
jack_connect -s "$SRV" pedal_bridge:send1_R sooperlooper:common_in_2
jack_connect -s "$SRV" sooperlooper:common_out_1 pedal_bridge:return1_L
jack_connect -s "$SRV" sooperlooper:common_out_2 pedal_bridge:return1_R

echo
echo "=== Scénario"
check "le client ouvre le fichier et publie (pont actif, SooperLooper vu)" wait_show "pont=1 sl=1" 5
check "état initial : arrêté, boucle 1 vide" wait_show "lecture=0 .* b1=vide" 3
"$B/pb_ctl" tempo 100 > /dev/null
check "réglage Tempo 100 -> pb_timebase à 100" wait_show "bpm=100.00" 3
"$B/pb_ctl" press play > /dev/null
check "Play : transport en marche, mesure 1" wait_show "lecture=1 mesure=1" 3
check "les temps avancent (temps 3)" wait_show "lecture=1 mesure=1 temps=3" 3
"$B/pb_ctl" press record > /dev/null
check "Record : attente puis enregistrement" wait_show "b1=\(attente\|enregistre\)" 2
check "enregistrement sur la mesure suivante" wait_show "b1=enregistre" 4
"$B/pb_ctl" press record > /dev/null
check "Record de nouveau : fin au cycle, puis lecture" wait_show "b1=lecture" 5
"$B/pb_ctl" loop 2 > /dev/null
check "boucle 2 choisie : vide" wait_show "boucle=2 .*b2=vide" 2
"$B/pb_ctl" loop 1 > /dev/null
"$B/pb_ctl" press mute > /dev/null
check "Mute : boucle 1 muette" wait_show "b1=muet" 5
"$B/pb_ctl" press mute > /dev/null
check "Mute de nouveau : lecture" wait_show "b1=lecture" 5
"$B/pb_ctl" press overdub > /dev/null
check "Overdub" wait_show "b1=overdub" 5
"$B/pb_ctl" press overdub > /dev/null
check "Overdub de nouveau : lecture" wait_show "b1=lecture" 5
"$B/pb_ctl" press play > /dev/null
check "Play/Stop pendant la lecture : arrêt, boucle en pause" wait_show "lecture=0 .*b1=pause" 5
check "la boucle a été normalisée à l'arrêt" grep -q "boucle 1 normalisée" "$PB_SHM_DIR/client.log"
"$B/pb_ctl" press play > /dev/null
check "Play : repart de la mesure 1, boucle en lecture" wait_show "lecture=1 mesure=1 .*b1=lecture" 5
"$B/pb_ctl" press undo > /dev/null
sleep 0.5
"$B/pb_ctl" press effacer > /dev/null
check "Effacer : boucle 1 vide" wait_show "b1=vide" 5
echo
echo "=== DLS Drum (sans Hydrogen sur le banc)"
"$B/pb_ctl" drum-take > /dev/null
sleep 0.3
for _ in 1 2 3 4; do "$B/pb_ctl" drum-press tap > /dev/null; sleep 0.6; done
check "Tap ×4 toutes les 0,6 s : tempo 100" wait_show "bpm=\(99\.[5-9]\|100\.[0-4]\)" 3
"$B/pb_ctl" drum-press play > /dev/null
check "Play/Stop de Drum pendant la lecture : arrêt" wait_show "lecture=0" 5
"$B/pb_ctl" drum-press play > /dev/null
check "Play/Stop de Drum : repart de la mesure 1" wait_show "lecture=1 mesure=1" 5
"$B/pb_ctl" drum-tempo 110 > /dev/null
check "réglage Tempo de Drum 110" wait_show "bpm=110.00" 3
"$B/pb_ctl" drum-groove 3 > /dev/null
sleep 0.3
check "groove sans Hydrogen : refusé proprement (client toujours là)" wait_show "pont=1" 2

"$B/pb_ctl" take > /dev/null
sleep 0.3
"$B/pb_ctl" press record > /dev/null
check "nouvelle instance (changement de preset) : ses boutons marchent" wait_show "b1=\(attente\|enregistre\)" 3

echo
echo "--- journal du client (contrôle) ---"
grep "contrôle" "$PB_SHM_DIR/client.log" | tail -30
echo
if [ "$FAIL" -eq 0 ]; then echo "=== TOUT EST OK"; else echo "=== DES TESTS ONT ÉCHOUÉ"; fi
exit "$FAIL"
