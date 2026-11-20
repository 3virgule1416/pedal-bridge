#!/usr/bin/env bash
# Démarrage de la session DLS au démarrage du Pi (service dls-session).
#
# Fenêtre de choix (pb_demarrage) : bouton 10 du Captain clignote 8 s en
# orange ; un appui = MODE SPECTACLE (PiPedal seul : ni JACK, ni Hydrogen, ni
# SooperLooper, ni le pont). Sinon : session complète (session.sh start).
#
#   bash scripts/dls-demarrage.sh
set -u
B="${PB_BUILD:-$HOME/pedal-bridge-build/build}"
S="$(dirname "$(readlink -f "$0")")/session.sh"

if "$B/pb_demarrage"; then
  echo "dls-demarrage : session complète"
  exec bash "$S" start
else
  echo "dls-demarrage : mode spectacle, la session n'est pas lancée"
  exit 0
fi
