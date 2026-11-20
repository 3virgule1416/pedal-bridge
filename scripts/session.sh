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
#   bash scripts/session.sh groove N  pattern N d'Hydrogen (1 = premier), retenu par save
#   bash scripts/session.sh batterie F  ouvre le morceau Hydrogen F (.h2song, chemin absolu)
#   bash scripts/session.sh stop      arrête le pont, Hydrogen et SooperLooper
#   bash scripts/session.sh stop-all  arrête aussi JACK, pb_timebase, l'écran virtuel et VNC
#
# Le tempo et la mesure viennent de pb_timebase (maître du temps JACK) : celui
# d'Hydrogen 1.2.0-beta est défectueux. Dans Hydrogen : J.Trans oui, J.Master non.
#
# Depuis le PC, il se relance tout seul sur le Pi (hôte pi-dev).
# Réglages : PB_JACK_PERIOD (défaut 128), PB_LINKS (défaut 1,2), PB_TEMPO (défaut 120,
# au premier démarrage de pb_timebase seulement), PB_GUI (défaut 0) :
#   PB_GUI=0  sans interface (défaut) : h2cli (Hydrogen sans fenêtre, même OSC,
#             même JACK, ~180 Mo au lieu de ~515 ; gros morceau chargé en
#             2,6 s au lieu de 6) et pas de slgui ni d'écran virtuel.
#   PB_GUI=1  Hydrogen et SooperLooper avec leur interface (écran virtuel, VNC)
# Changer de mode : bash scripts/session.sh stop, puis start avec PB_GUI.
#
# Branchements faits par « start » et « connect » :
#   lien 1 = looper (DLS Looper, ou DLS Send/Return sur le lien 1)
#     pedal_bridge:send1_L/R      -> sooperlooper:common_in_1/2   (ta guitare dans le looper)
#     sooperlooper:common_out_1/2 -> pedal_bridge:return1_L/R     (les boucles dans PiPedal)
#   lien 2 = batterie (DLS Return sur le lien 2, plus tard DLS Drum), en fin de chaîne
#     Hydrogen:out_L/R            -> pedal_bridge:return2_L/R
# Avec PB_LINKS=1 : ancienne façon, la batterie arrive par le lien 1.
set -u

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "PB_JACK_PERIOD='${PB_JACK_PERIOD:-}' PB_LINKS='${PB_LINKS:-}' PB_TEMPO='${PB_TEMPO:-}' PB_GUI='${PB_GUI:-}' bash '$REMOTE/src/scripts/session.sh' $*"
fi

B="$HOME/$REMOTE/build"
RUN=/tmp/pedal-bridge-session          # journaux (effacés au redémarrage du Pi)
PERIOD="${PB_JACK_PERIOD:-128}"
LINKS="${PB_LINKS:-1,2}"
GUI="${PB_GUI:-0}"
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

unlink_port() { # débranche si branché
  jack_lsp -c "$1" 2> /dev/null | grep -qx "   $2" || return 0
  jack_disconnect "$1" "$2" 2> /dev/null && echo "  débranché : $1 -> $2"
}

do_connect() {
  echo "== Branchements JACK"
  local looper="${LINKS%%,*}" drum="${LINKS%%,*}"
  case ",$LINKS," in *,2,*) [ "$looper" != 2 ] && drum=2 ;; esac
  link "pedal_bridge:send${looper}_L" sooperlooper:common_in_1
  link "pedal_bridge:send${looper}_R" sooperlooper:common_in_2
  link sooperlooper:common_out_1 "pedal_bridge:return${looper}_L"
  link sooperlooper:common_out_2 "pedal_bridge:return${looper}_R"
  if [ "$drum" != "$looper" ]; then   # la batterie quitte le lien du looper
    unlink_port Hydrogen:out_L "pedal_bridge:return${looper}_L"
    unlink_port Hydrogen:out_R "pedal_bridge:return${looper}_R"
  fi
  link Hydrogen:out_L "pedal_bridge:return${drum}_L"
  link Hydrogen:out_R "pedal_bridge:return${drum}_R"
  echo "  looper : lien $looper ; batterie : lien $drum"
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

# Hydrogen en mode Pattern (pas Song), par son OSC (port 9000, activé dans
# ~/.hydrogen/hydrogen.conf). Vérifié : SONG_MODE_ACTIVATION 0 = mode Pattern.
h2_pattern_mode() {
  for _ in $(seq 50); do       # attendre que son OSC écoute (max 5 s)
    ss -lun 2> /dev/null | grep -q ':9000 ' && break
    sleep 0.1
  done
  if ! ss -lun 2> /dev/null | grep -q ':9000 '; then
    echo "  ATTENTION : l'OSC d'Hydrogen ne répond pas (oscEnabled dans hydrogen.conf ?)"
    return
  fi
  "$HOME/bridge-env/bin/python" - <<'PY'
import socket
from pythonosc.osc_message_builder import OscMessageBuilder
m = OscMessageBuilder(address="/Hydrogen/SONG_MODE_ACTIVATION"); m.add_arg(0.0)
socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(m.build().dgram, ("127.0.0.1", 9000))
PY
  echo "  Hydrogen : mode Pattern"
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
  if [ "$GUI" != 0 ]; then
    running Xvfb    || { launch xvfb Xvfb :1 -screen 0 1280x720x24; sleep 1; }
    running openbox || launch openbox openbox
    running x11vnc  || launch x11vnc x11vnc -display :1 -localhost -nopw -forever -shared -quiet
  fi

  echo "== SooperLooper (8 boucles stéréo, OSC 9951)"
  if ! running sooperlooper; then
    launch sooperlooper sooperlooper -l 8 -c 2 -p 9951
    wait_port sooperlooper:common_in_1 10 && sleep 0.5
  fi
  sl_settings
  [ "$GUI" != 0 ] && { running slgui || launch slgui slgui; }

  echo "== Hydrogen"
  # Hydrogen compilé (~/hydrogen-1.2.7 : corrige le son doublé après un
  # changement de tempo, bug #1953) s'il existe, sinon celui de Debian.
  # PB_H2=/usr pour forcer celui de Debian.
  local H2="${PB_H2:-$HOME/hydrogen-1.2.7}"
  [ -x "$H2/bin/h2cli" ] || H2=/usr
  echo "  Hydrogen : $H2/bin"
  if ! running hydrogen && ! running h2cli; then
    if [ "$GUI" != 0 ]; then
      launch hydrogen env LD_LIBRARY_PATH="$H2/lib" "$H2/bin/hydrogen" -d jack -n
    else                                   # sans fenêtre ; le pont ouvre ensuite le bon morceau
      launch h2cli env LD_LIBRARY_PATH="$H2/lib" "$H2/bin/h2cli" -d jack -s "$H2/share/hydrogen/data/demo_songs/GM_kit_demo1.h2song"
    fi
    wait_port Hydrogen:out_L 20
  elif [ "$GUI" = 0 ] && running hydrogen; then
    echo "  ATTENTION : Hydrogen tourne avec son interface (PB_GUI=0 demandé : stop, puis start)"
  fi
  h2_pattern_mode

  echo "== Pont pedal_bridge (liens $LINKS)"
  if ! running pedal_bridge; then
    # --sl-osc : le pont tient à jour la compensation de latence de SooperLooper
    launch pedal_bridge "$B/pedal_bridge" --links "$LINKS" --stats 10 --sl-osc 9951
    wait_port "pedal_bridge:send${LINKS%%,*}_L" 5
  fi
  # Gardien : si le pont plante, double switch rendu à PiPedal et Captain page 0.
  running pb_garde || launch pb_garde "$B/pb_garde"

  do_connect
  echo
  if [ "$GUI" != 0 ]; then echo "Prêt. Dans Hydrogen (par VNC) : J.Trans allumé, J.Master ÉTEINT."
  else echo "Prêt (sans interface ; PB_GUI=1 pour Hydrogen et SooperLooper par VNC)."; fi
  echo "Le tempo vient de pb_timebase : bash scripts/session.sh tempo 115"
  echo "Suivi du pont : bash scripts/session.sh status"
}

# Le pont prend le double switch de l'Axe à PiPedal tant qu'il tourne (il
# débranche la connexion MIDI Axe -> PiPedal) et le rend en s'arrêtant. Filet,
# s'il a planté : sans pont, rebrancher l'Axe à PiPedal (A et B changent de
# preset comme avant).
axe_back() {
  running pedal_bridge && return 0
  aconnect -l 2>/dev/null | grep -q "AXE IO One" || return 0
  aconnect "AXE IO One:0" "PiPedal:0" 2>/dev/null && echo "  double switch rendu à PiPedal (pont absent)"
  return 0
}

do_status() {
  axe_back
  echo "== Programmes"
  for p in jackd pb_timebase Xvfb openbox x11vnc sooperlooper slgui hydrogen h2cli pedal_bridge pb_garde; do
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
  axe_back
  stop_one hydrogen
  stop_one h2cli
  stop_one slgui
  stop_one sooperlooper
}

case "${1:-}" in
  start)    do_start ;;
  status)   do_status ;;
  connect)  do_connect ;;
  tempo)    # tempo de pb_timebase : Hydrogen et SooperLooper le suivent par JACK
    if [ -z "${2:-}" ]; then "$B/pb_timebase" --get; exit; fi
    # par le client si possible : il met aussi à jour l'affichage d'Hydrogen
    if running pedal_bridge; then "$B/pedal_bridge" --cmd "tempo $2"; else "$B/pb_timebase" --set "$2"; fi ;;
  groove) "$B/pedal_bridge" --cmd "groove ${2:-}" ;;   # pattern N d'Hydrogen (à partir de 1)
  batterie) "$B/pedal_bridge" --cmd "batterie ${2:-}" ;;
  # Commandes musicales : exécutées par le client pedal_bridge (voir client/controle.h)
  play)  "$B/pedal_bridge" --cmd play ;;            # tout repart de la mesure 1, ensemble
  pause) "$B/pedal_bridge" --cmd stop ;;            # tout s'arrête ; nouvelles boucles normalisées
  save)  "$B/pedal_bridge" --cmd "save ${2:-}" ;;   # sauvegarde la chanson
  load)  "$B/pedal_bridge" --cmd "load ${2:-}" ;;   # recharge une chanson
  list)  "$B/pedal_bridge" --cmd list ;;
  boucles) "$B/pedal_bridge" --cmd status ;;
  stop)     do_stop ;;
  stop-all) do_stop; stop_one pb_garde; stop_one x11vnc; stop_one openbox; stop_one Xvfb; stop_one pb_timebase; stop_one jackd ;;
  *) sed -n '2,32p' "$0"; exit 2 ;;
esac
