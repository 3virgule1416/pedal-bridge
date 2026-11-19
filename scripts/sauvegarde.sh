#!/usr/bin/env bash
# Copie sur le PC ce qui n'existe que sur le Pi : chansons de batterie,
# mémoire des choix (variations, intro, outro, Browse) et modèles de kits.
# Ne supprime rien, ni sur le Pi ni sur le PC (les fichiers copiés remplacent
# ceux du même nom).
#
#   bash scripts/sauvegarde.sh [DOSSIER]   (défaut : E:/DWL/Pedalboard)
#
# Résultat :
#   DOSSIER/DLS Drums/        chansons (Style/Type/Beat.h2song, .alias.txt, .X.parts.json cachés)
#   DOSSIER/dls-pi/choix.txt  mémoire des choix
#   DOSSIER/dls-pi/dls-modeles/
set -euo pipefail

HOST="${PB_HOST:-pi-dev}"
DEST="${1:-/e/DWL/Pedalboard}"
mkdir -p "$DEST/dls-pi"

echo "== Chansons -> $DEST/DLS Drums"
ssh "$HOST" "tar -C /var/pipedal/audio_uploads -cf - 'DLS Drums'" | tar -C "$DEST" -xf -
echo "== Mémoire et modèles -> $DEST/dls-pi"
ssh "$HOST" "tar -C ~ -cf - dls-batterie/choix.txt dls-modeles" | tar -C "$DEST/dls-pi" -xf -
mv -f "$DEST/dls-pi/dls-batterie/choix.txt" "$DEST/dls-pi/choix.txt" && rmdir "$DEST/dls-pi/dls-batterie"

echo "== OK : $(find "$DEST/DLS Drums" -name '*.h2song' | wc -l) chansons, $(du -sh "$DEST/DLS Drums" | cut -f1)"
