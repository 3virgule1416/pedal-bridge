# Ce qui est installé sur le Pi pour le projet

Inventaire tenu à jour pour pouvoir faire le ménage à la fin du projet. Chaque
ligne dit **quoi**, **où**, **pourquoi**, et **comment l'enlever**. Rien n'est
supprimé sans l'accord du musicien.

Relevé du 2026-11-18 (historique `/var/log/apt/history.log` et dossiers).

## 1. Paquets apt

| Date | Commande | Pourquoi | Garder ? |
|---|---|---|---|
| 2026-11-13 | `apt upgrade` | mise à jour du système | — |
| 2026-11-13 | `apt-get install ./Downloads/pipedal_2.0.110_arm64.deb`, puis mise à jour automatique en 2.0.111 | PiPedal | oui (c'est le cœur) |
| 2026-11-13 | `apt install --no-install-recommends jackd2 hydrogen sooperlooper xvfb x11vnc openbox` | serveur JACK, batterie, looper ; `xvfb x11vnc openbox` : affichage des interfaces graphiques à distance (mode `PB_GUI=1`) | `jackd2 hydrogen sooperlooper` : oui ; `xvfb x11vnc openbox` : seulement pour le mode avec interface |
| 2026-11-14 | `apt install --no-install-recommends build-essential pkg-config cmake git lv2-dev libjack-jackd2-dev libzita-resampler-dev lilv-utils` | compiler pedal-bridge (plugins LV2 et client) ; `lilv-utils` : `lv2info` pour les tests | tant qu'on compile sur le Pi |
| 2026-11-18 | `apt install --no-install-recommends qtbase5-dev libqt5svg5-dev qttools5-dev qttools5-dev-tools libsndfile1-dev libarchive-dev liblo-dev libasound2-dev libpulse-dev ladspa-sdk zlib1g-dev` (79 paquets avec leurs dépendances) | compiler Hydrogen 1.2.7 (bug #1953 : son doublé après un changement de tempo) | seulement le temps de compiler Hydrogen |

Pour enlever un groupe de paquets de développement (leurs dépendances
installées automatiquement partent avec `autoremove`) — **à vérifier par une
simulation `-s` d'abord** :

```bash
sudo apt purge -s qtbase5-dev libqt5svg5-dev qttools5-dev qttools5-dev-tools libsndfile1-dev libarchive-dev liblo-dev libasound2-dev libpulse-dev ladspa-sdk zlib1g-dev
sudo apt autoremove -s
```

## 2. Fichiers système (sudo)

| Où | Quoi | Enlever |
|---|---|---|
| `/usr/local/lib/lv2/pedal-bridge.lv2/` | plugins DLS Looper, DLS Drum, Send/Return (`*.ttl`, `pedal_bridge.so`, `browse.txt`) | `sudo rm -r` le dossier, puis redémarrer pipedald |
| `/usr/local/lib/lv2/pedal-bridge-test.lv2/` | plugin de test (banc d'essai) | idem |
| `/usr/local/lib/lv2/lsp-plugins.lv2/` | plugins LSP (Send/Return de LSP) | **à garder tant que le musicien ne demande pas** (consigne) |

## 3. Dossier personnel (`/home/pedalboard`)

| Où | Taille | Quoi |
|---|---|---|
| `~/pedal-bridge-build/` | 5 Mo | sources copiées par `deploy.sh` et compilation (`build/`) |
| `~/dls-batterie/` | 115 Mo | gros morceaux par kit (`gros/`), rendus, anciennes variantes, `choix.txt` (mémoire des variations, intro, outro, Browse) |
| `~/dls-modeles/` | 0,4 Mo | modèles `.h2song` des kits (instruments) |
| `~/.hydrogen/data/drumkits/` | 72 Mo | kits AVL Black Pearl, Red Zeppelin, Death Metal |
| `~/.hydrogen/hydrogen.conf.bak-*` | — | copies de sauvegarde de la configuration d'Hydrogen |
| `~/kits-dl/` | 50 Mo | kits téléchargés (`.h2drumkit`), déjà importés dans `~/.hydrogen` |
| `~/lsp/`, `~/lsp-plugins-1.2.35-Linux-aarch64.7z` | 169 Mo | archive et fichiers des plugins LSP (déjà installés dans `/usr/local/lib/lv2`) |
| `~/bridge-env/` | 28 Mo | environnement Python (python-osc, websockets) des premiers essais |
| `~/pedal-bridge/` | 6 Mo | premiers essais (`chansons/`, `travail/`) |
| `~/pp_set.py`, `~/sl_init.py`, `~/sl_meter.py`, `~/sl_test.py` | — | scripts des premiers essais |
| `~/Downloads/pipedal_2.0.110_arm64.deb` | 31 Mo | paquet PiPedal d'origine |
| `~/src/hydrogen-1.2.7/` (à venir) | — | sources et compilation d'Hydrogen 1.2.7 |
| `~/hydrogen-1.2.7/` (à venir) | — | Hydrogen 1.2.7 installé hors système ; `session.sh` l'utilise s'il existe. Retour au Hydrogen de Debian : renommer ce dossier |

## 4. Autres

| Où | Quoi |
|---|---|
| `/var/pipedal/audio_uploads/DLS Drums/` (40 Mo) | chansons de batterie (Style/Type/Beat), `.alias.txt`, `.X.parts.json` cachés |
| `/tmp/pedal-bridge-session/` | journaux de la session (effacés au redémarrage du Pi) |
| `/dev/shm/pedal-bridge-*` | mémoire partagée plugins ↔ client (disparaît au redémarrage) |
