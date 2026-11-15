#!/usr/bin/env bash
# Session audio JACK de pedal-bridge sur le Pi (sans écran physique).
#
# PiPedal n'utilise PAS JACK : il parle directement à l'Axe I/O One par ALSA.
# Ce serveur JACK (pilote dummy, sans carte son) sert seulement à relier
# Hydrogen, SooperLooper et le pont pedal_bridge.
#
#   bash scripts/session.sh start     démarre tout ce qui manque, puis branche
#   bash scripts/session.sh status    état, branchements, dernières stats du pont
#   bash scripts/session.sh connect   refait les branchements JACK
#   bash scripts/session.sh tempo 115 change le tempo (Hydrogen et SooperLooper suivent)
#   bash scripts/session.sh play      batterie + boucles repartent ensemble de la mesure 1
#   bash scripts/session.sh pause     batterie + boucles s'arrêtent (vrai stop)
#   bash scripts/session.sh save NOM  sauvegarde la chanson (boucles + tempo)
#   bash scripts/session.sh load NOM  recharge une chanson, prête à jouer de la mesure 1
#   bash scripts/session.sh list      chansons sauvegardées
#   bash scripts/session.sh boucles   état des boucles (normalisées ou non)
#   bash scripts/session.sh stop      arrête le pont, Hydrogen et SooperLooper
#   bash scripts/session.sh stop-all  arrête aussi JACK, pb_timebase, l'écran virtuel et VNC
#
# Le tempo et la mesure viennent de pb_timebase (maître du temps JACK) : celui
# d'Hydrogen 1.2.0-beta est défectueux. Dans Hydrogen : J.Trans oui, J.Master non.
#
# Depuis le PC, il se relance tout seul sur le Pi (hôte pi-dev).
# Réglages : PB_JACK_PERIOD (défaut 128), PB_LINKS (défaut 1), PB_TEMPO (défaut 120,
# au premier démarrage de pb_timebase seulement).
#
# Branchements faits par « start » et « connect » (lien 1) :
#   pedal_bridge:send1_L/R    -> sooperlooper:common_in_1/2   (ta guitare dans le looper)
#   sooperlooper:common_out_1/2 -> pedal_bridge:return1_L/R   (les boucles dans PiPedal)
#   Hydrogen:out_L/R          -> pedal_bridge:return1_L/R     (la batterie dans PiPedal)
set -u

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "PB_JACK_PERIOD='${PB_JACK_PERIOD:-}' PB_LINKS='${PB_LINKS:-}' PB_TEMPO='${PB_TEMPO:-}' bash '$REMOTE/src/scripts/session.sh' $*"
fi

B="$HOME/$REMOTE/build"
RUN=/tmp/pedal-bridge-session          # journaux (effacés au redémarrage du Pi)
PERIOD="${PB_JACK_PERIOD:-128}"
LINKS="${PB_LINKS:-1}"
TEMPO="${PB_TEMPO:-120}"
export DISPLAY=:1
mkdir -p "$RUN"

running() { pgrep -u "$USER" -x "$1" > /dev/null; }

# Lance un programme détaché de la session SSH, journal dans $RUN/<nom>.log
launch() {
  local name=$1; shift
  echo "  démarrage : $name"
  setsid -f "$@" > "$RUN/$name.log" 2>&1 < /dev/null
}

wait_port() {   # attend qu'un port JACK existe (max $2 secondes)
  local t=$(( ${2:-10} * 10 ))
  for _ in $(seq "$t"); do
    jack_lsp 2> /dev/null | grep -qx "$1" && return 0
    sleep 0.1
  done
  echo "  ATTENTION : port $1 absent après ${2:-10} s"
  return 1
}

jack_ok() { jack_lsp > /dev/null 2>&1; }

link() {        # branche si pas déjà fait
  jack_lsp -c "$1" 2> /dev/null | grep -qx "   $2" && return 0
  jack_connect "$1" "$2" 2> /dev/null && echo "  $1 -> $2" || echo "  ÉCHEC : $1 -> $2"
}

do_connect() {
  echo "== Branchements JACK"
  for n in ${LINKS//,/ }; do
    link "pedal_bridge:send${n}_L" sooperlooper:common_in_1
    link "pedal_bridge:send${n}_R" sooperlooper:common_in_2
    link sooperlooper:common_out_1 "pedal_bridge:return${n}_L"
    link sooperlooper:common_out_2 "pedal_bridge:return${n}_R"
    link Hydrogen:out_L "pedal_bridge:return${n}_L"
    link Hydrogen:out_R "pedal_bridge:return${n}_R"
    break   # looper et batterie sur le premier lien seulement
  done
}

sl_settings() {   # synchro SooperLooper sur JACK (comme ~/sl_init.py)
  "$HOME/bridge-env/bin/python" - <<'PY'
from pythonosc.udp_client import SimpleUDPClient
c = SimpleUDPClient("127.0.0.1", 9951)
c.send_message("/set", ["sync_source", -1])        # Jack/Host
c.send_message("/set", ["eighth_per_cycle", 8.0])  # 1 mesure en 4/4
for i in range(8):
    c.send_message(f"/sl/{i}/set", ["sync", 1])
    c.send_message(f"/sl/{i}/set", ["quantize", 1])   # cycle
    c.send_message(f"/sl/{i}/set", ["round", 1])
PY
  echo "  SooperLooper : synchro Jack/Host, quantize cycle, round, 8 croches/cycle"
}

do_start() {
  echo "== JACK (dummy, 48 kHz, période $PERIOD)"
  if jack_ok; then
    cur=$(jack_bufsize 2> /dev/null | grep -o '[0-9]\+' | head -1)
    if [ "$cur" != "$PERIOD" ] && running sooperlooper; then
      # SooperLooper 1.7.8 plante si la période JACK change pendant qu'il tourne
      # (constaté : processus disparu, boucles perdues).
      echo "  ATTENTION : JACK est en période $cur, pas $PERIOD. Je ne la change pas :"
      echo "  SooperLooper planterait. Pour changer : session.sh stop, puis start."
    elif [ "$cur" != "$PERIOD" ]; then
      echo "  déjà démarré en période $cur : passage à $PERIOD"
      jack_bufsize "$PERIOD" > /dev/null
    else
      echo "  déjà démarré"
    fi
  else
    launch jackd jackd -R -d dummy -r 48000 -p "$PERIOD"
    jack_wait -w -t 5 > /dev/null 2>&1 || { echo "ÉCHEC : JACK ne démarre pas"; tail "$RUN/jackd.log"; exit 1; }
  fi

  echo "== Maître du temps JACK (pb_timebase)"
  if running pb_timebase; then
    echo "  déjà démarré : $("$B/pb_timebase" --get 2> /dev/null)"
  else
    launch pb_timebase "$B/pb_timebase" --bpm "$TEMPO"
    sleep 0.3
  fi

  echo "== Écran virtuel :1 et VNC (localhost:5900)"
  running Xvfb    || { launch xvfb Xvfb :1 -screen 0 1280x720x24; sleep 1; }
  running openbox || launch openbox openbox
  running x11vnc  || launch x11vnc x11vnc -display :1 -localhost -nopw -forever -shared -quiet

  echo "== SooperLooper (8 boucles stéréo, OSC 9951)"
  if ! running sooperlooper; then
    launch sooperlooper sooperlooper -l 8 -c 2 -p 9951
    wait_port sooperlooper:common_in_1 10 && sleep 0.5
  fi
  sl_settings
  running slgui || launch slgui slgui

  echo "== Hydrogen"
  if ! running hydrogen; then
    launch hydrogen hydrogen -d jack -n
    wait_port Hydrogen:out_L 20
  fi

  echo "== Pont pedal_bridge (liens $LINKS)"
  if ! running pedal_bridge; then
    # --sl-osc : le pont tient à jour la compensation de latence de SooperLooper
    launch pedal_bridge "$B/pedal_bridge" --links "$LINKS" --stats 10 --sl-osc 9951
    wait_port "pedal_bridge:send${LINKS%%,*}_L" 5
  fi

  do_connect
  echo
  echo "Prêt. Dans Hydrogen (par VNC) : J.Trans allumé, J.Master ÉTEINT."
  echo "Le tempo vient de pb_timebase : bash scripts/session.sh tempo 115"
  echo "Suivi du pont : bash scripts/session.sh status"
}

do_status() {
  echo "== Programmes"
  for p in jackd pb_timebase Xvfb openbox x11vnc sooperlooper slgui hydrogen pedal_bridge; do
    pid=$(pgrep -u "$USER" -x "$p" | head -1)
    if [ -n "$pid" ]; then st="en marche (pid $pid)"; else st="arrêté"; fi
    printf "  %-13s %s\n" "$p" "$st"
  done
  if jack_ok; then
    echo "== JACK : période $(jack_bufsize 2> /dev/null | grep -o '[0-9]\+' | head -1)"
    running pb_timebase && echo "== Maître du temps : $("$B/pb_timebase" --get 2> /dev/null)"
    echo "== Branchements du pont"
    jack_lsp -c 2> /dev/null | awk '/^pedal_bridge:/{p=$0; print "  " p; next} /^ /{if (p) print "     ->" $0; next} {p=""}'
  else
    echo "== JACK : arrêté"
  fi
  if [ -f "$RUN/pedal_bridge.log" ]; then
    echo "== Dernières stats du pont"
    tail -n 3 "$RUN/pedal_bridge.log" | sed 's/^/  /'
  fi
}

stop_one() {
  if running "$1"; then
    pkill -u "$USER" -x "$1" && echo "  arrêt : $1"
    for _ in $(seq 30); do running "$1" || return 0; sleep 0.1; done
  fi
}

do_stop() {
  echo "== Arrêt"
  stop_one pedal_bridge
  stop_one hydrogen
  stop_one slgui
  stop_one sooperlooper
}

case "${1:-}" in
  start)    do_start ;;
  status)   do_status ;;
  connect)  do_connect ;;
  tempo)    # tempo de pb_timebase : Hydrogen et SooperLooper le suivent par JACK
    if [ -z "${2:-}" ]; then "$B/pb_timebase" --get; exit; fi
    "$B/pb_timebase" --set "$2" ;;
  # Commandes musicales : exécutées par le client pedal_bridge (voir client/controle.h)
  play)  "$B/pedal_bridge" --cmd play ;;            # tout repart de la mesure 1, ensemble
  pause) "$B/pedal_bridge" --cmd stop ;;            # tout s'arrête ; nouvelles boucles normalisées
  save)  "$B/pedal_bridge" --cmd "save ${2:-}" ;;   # sauvegarde la chanson
  load)  "$B/pedal_bridge" --cmd "load ${2:-}" ;;   # recharge une chanson
  list)  "$B/pedal_bridge" --cmd list ;;
  boucles) "$B/pedal_bridge" --cmd status ;;
  stop)     do_stop ;;
  stop-all) do_stop; stop_one x11vnc; stop_one openbox; stop_one Xvfb; stop_one pb_timebase; stop_one jackd ;;
  *) sed -n '2,31p' "$0"; exit 2 ;;
esac
