# pedal-bridge — Davis Loop Station (DLS)

Pont audio entre **PiPedal** (ALSA direct) et **JACK** (SooperLooper, Hydrogen)
sur Raspberry Pi 5, par tampons circulaires partagés dans `/dev/shm`.

- **DLS Send** (LV2, stéréo) : laisse passer l'audio et en envoie une copie
  sur un lien (1 à 8).
- **DLS Return** (LV2, stéréo) : reçoit l'audio d'un lien et l'ajoute au
  signal (Add) ou le remplace (Replace).
- **pedal_bridge** (client JACK) : relie les liens aux ports JACK, avec
  rééchantillonnage adaptatif (zita-resampler) contre la dérive des horloges.
- **pb_inspect** : lecteur de contrôle des tampons.

## État

| Étape | Contenu | État |
|---|---|---|
| (a) | Send + pb_inspect + tests | fait, validé à l'oreille |
| (b) | Return + `pb_inspect --tone/--loop` | fait, validé à l'oreille |
| (c) | Client JACK + rééchantillonnage adaptatif, maître du temps | fait, validé à l'oreille |
| (d) | Mesures (latence, dérive, xruns, CPU) | fait : 0 xrun et 0 coupure sur 10 min, voir plus bas |
| (e) | Contrôle au pied : batterie, looper, écran | en conception : [docs/controle.md](docs/controle.md) ; comparaison avec les Boss RC : [docs/rc600.md](docs/rc600.md) |

## Structure

```
CMakeLists.txt
common/pb_shm.h, pb_shm.c   format du tampon, file SPSC sans verrou
lv2/plugin.c, plugin.h      point d'entrée du bundle (lv2_descriptor)
lv2/send.c                  plugin Send
lv2/return.c                plugin Return
lv2/ttl/*.ttl               description LV2 (manifest, send, return)
client/pedal_bridge.cc      client JACK
client/controle.cc          play/stop, normalisation des boucles, chansons (commandes UDP)
tools/pb_inspect.c          lecteur de contrôle
tools/pb_timebase.c         maître du temps JACK (mesure et tempo corrects)
tools/pb_transport.c        démarrer/arrêter/lire le transport JACK (tests)
tools/pb_slhit.c            commande SooperLooper + changements d'état à la trame près
tools/pb_slphase.c          décalage entre le début d'une boucle et le 1 de la mesure
tools/pb_onsets.c           sur quel temps tombent les coups d'un port JACK (Hydrogen)
tests/test_ring.c           tampon : 2 threads, 6 M trames, ordre et intégrité
tests/test_plugin.c         Send chargé par dlopen, comme un hôte LV2
tests/test_return.c         Return chargé par dlopen ; le test joue le client
tests/test_alloc.h          compte les allocations pendant run()
tests/pb_fakehost.c         faux PiPedal (vrais plugins, horloge décalée) : latence, clics
scripts/deploy.sh           PC -> Pi, puis compilation
scripts/test.sh             tests unitaires, TTL, strace (se relance sur le Pi)
scripts/test-jack.sh        client JACK avec serveur JACK séparé et dérive simulée
scripts/session.sh          démarre/arrête JACK, VNC, SooperLooper, Hydrogen et le pont
scripts/mesure.sh           mesures sur la vraie session (dérive, xruns, CPU), lecture seule
scripts/sl-sync-test.sh     synchro SooperLooper sur serveur JACK de test (départ sur le 1)
scripts/install.sh          affiche les commandes sudo d'installation
```

## Compiler et tester (depuis le PC, Git Bash)

```bash
bash scripts/deploy.sh     # copie dans ~/pedal-bridge-build/src, compile dans .../build
bash scripts/test.sh       # ctest + lv2info + vérification strace
bash scripts/test-jack.sh  # client JACK : 0, +300, -300 ppm (40 s chacun par défaut)
```

Hôte SSH par défaut : `pi-dev` (variable `PB_HOST` pour en changer).

Dépendances sur le Pi (déjà présentes) : `build-essential cmake pkg-config lv2-dev
lilv-utils strace libjack-jackd2-dev libzita-resampler-dev jackd2`.

`test-jack.sh` ne touche ni à PiPedal ni à ton serveur JACK : il lance un
serveur séparé nommé `pbtest` et met ses tampons dans un dossier temporaire
(`PB_SHM_DIR`). Le faux PiPedal injecte aussi un clic exprès pour vérifier que
le détecteur fonctionne.

## Installer

`bash scripts/install.sh` affiche les commandes à lancer sur le Pi. Seuls ces
fichiers sont installés :

```
/usr/local/lib/lv2/pedal-bridge.lv2/manifest.ttl
/usr/local/lib/lv2/pedal-bridge.lv2/send.ttl
/usr/local/lib/lv2/pedal-bridge.lv2/return.ttl
/usr/local/lib/lv2/pedal-bridge.lv2/pedal_bridge.so
```

puis `sudo systemctl restart pipedald` (le son coupe quelques secondes).

**Désinstaller** : `sudo rm -r /usr/local/lib/lv2/pedal-bridge.lv2` puis
redémarrer pipedald. Les fichiers `/dev/shm/pedal-bridge-*` disparaissent au
prochain démarrage du Pi.

## Session audio (JACK, SooperLooper, Hydrogen, pont)

PiPedal n'utilise **pas** JACK : il parle directement à l'Axe I/O One (ALSA).
Le serveur JACK (pilote dummy, sans carte son) sert seulement à relier
Hydrogen, SooperLooper et `pedal_bridge`. Tout se lance avec un script (depuis
le PC ou sur le Pi) :

```bash
bash scripts/session.sh start      # JACK 128, écran virtuel :1 + VNC, SooperLooper, Hydrogen, pont, branchements
bash scripts/session.sh status     # état, branchements, dernières stats du pont
bash scripts/session.sh connect    # refaire les branchements
bash scripts/session.sh tempo 115  # changer le tempo (pb_timebase)
bash scripts/session.sh play       # batterie + boucles repartent ensemble de la mesure 1 (exact)
bash scripts/session.sh pause      # vrai stop ; les nouvelles boucles sont normalisées
bash scripts/session.sh save NOM   # sauvegarde la chanson (boucles, tempo, morceau Hydrogen, groove)
bash scripts/session.sh load NOM   # recharge une chanson, prête à jouer de la mesure 1
bash scripts/session.sh list       # chansons sauvegardées
bash scripts/session.sh boucles    # état des boucles (normalisées ou non)
bash scripts/session.sh groove 3   # pattern 3 d'Hydrogen (1 = premier), retenu par save ;
                                   # cycle SooperLooper = longueur du pattern (1, 2, 4 mesures)
bash scripts/session.sh stop       # arrêter pont, Hydrogen, SooperLooper
bash scripts/session.sh stop-all   # arrêter aussi JACK, pb_timebase, l'écran virtuel et VNC
```

- `start` ne relance pas ce qui tourne déjà. Si JACK tourne avec une autre
  période, il la change à chaud (`jack_bufsize`).
- Interfaces graphiques : écran virtuel `:1`, VNC seulement en local sur le Pi
  (`localhost:5900`). Depuis le PC : `ssh -L 5900:localhost:5900 pi-dev`, puis
  TigerVNC sur `localhost:5900`.
- **Tempo et mesure : `pb_timebase`** (maître du temps JACK, lancé par `start`).
  Hydrogen 1.2.0-beta, en maître du temps, publie une mesure fausse (le temps
  reste à 1, un « temps » dure une mesure entière) : SooperLooper calcule alors
  des cycles de 4 mesures et démarre parfois sur le 2e ou 3e temps. Sans aucun
  maître, SooperLooper (synchro Jack/Host) n'a pas de tempo et attend sans fin.
  `pb_timebase` publie une mesure correcte calculée depuis la trame 0 du
  transport, comme la grille de SooperLooper (qui ne lit que le tempo et la
  trame). Change le tempo transport arrêté : en lecture, la position saute.
  Dans Hydrogen : **J.Trans allumé, J.Master éteint** ; `start` le met en **mode
  Pattern** par son OSC (activé dans `hydrogen.conf`, port 9000). Tempo :
  `bash scripts/session.sh tempo 115` (sans nombre : affiche le tempo).
  `hydrogen.conf` est revenu à J.Master éteint au démarrage (sauvegarde des
  essais : `hydrogen.conf.bak-20261114-171658`).
- **Compensation de latence** : le pont déclare sa latence à JACK (ports `sendN` :
  entrée de PiPedal + trajet aller ; `returnN` : trajet retour + sortie de PiPedal ;
  latence de PiPedal lue dans `/var/pipedal/AudioConfig.json`, partagée moitié-moitié).
  SooperLooper 1.7.8 ne lit pas ces valeurs : le pont les lui envoie lui-même par
  OSC (`pedal_bridge --sl-osc 9951`, mis par `session.sh`) : `autoset_latency` 0,
  `input_latency` = envoi, `output_latency` = retour, pour toutes les boucles. Il
  les renvoie dès qu'elles changent (période JACK, réglages audio de PiPedal relus
  chaque seconde) et toutes les 5 s, au cas où SooperLooper aurait redémarré. Si
  les boucles sont encore un peu en avance ou en retard à l'oreille, ajuster avec
  `pedal_bridge --pipedal-latency-ms X`.
- Journaux : `/tmp/pedal-bridge-session/*.log`.

## Utiliser

1. Dans PiPedal, ajouter **DLS Send** à l'endroit voulu de la chaîne, choisir
   le **Lien**.
2. Sur le Pi, vérifier que l'audio arrive :

   ```bash
   ~/pedal-bridge-build/build/pb_inspect                  # état de tous les liens
   ~/pedal-bridge-build/build/pb_inspect --read 1         # niveaux du lien 1 en direct
   ~/pedal-bridge-build/build/pb_inspect --read 1 --seconds 10 --wav /tmp/l1.wav
   ```

   Le voyant **Connecté** du plugin s'allume quand un lecteur (pb_inspect ou le
   client JACK) consomme le lien.

3. Ajouter **DLS Return** plus loin dans la chaîne (mode **Add**), choisir un
   lien. Sans client JACK, on peut l'essayer avec pb_inspect :

   ```bash
   ~/pedal-bridge-build/build/pb_inspect --tone 1        # son test 440 Hz -> Return 1
   ~/pedal-bridge-build/build/pb_inspect --loop 1 1      # Send 1 -> Return 1 (ta guitare doublée)
   ```

   `--target T` règle la réserve visée en trames (défaut : 1024 pour --tone,
   512 pour --loop).

4. Lancer le client JACK (sous ton compte, serveur JACK déjà démarré) :

   ```bash
   ~/pedal-bridge-build/build/pedal_bridge --links 1
   ```

   Ports créés : `pedal_bridge:send1_L/R` (sorties : le son de PiPedal) et
   `pedal_bridge:return1_L/R` (entrées : ce qui retourne dans PiPedal). Brancher
   avec `jack_connect` (vérifier les noms avec `jack_lsp`), par exemple :

   ```bash
   jack_connect pedal_bridge:send1_L sooperlooper:common_in_1
   jack_connect pedal_bridge:send1_R sooperlooper:common_in_2
   jack_connect sooperlooper:common_out_1 pedal_bridge:return1_L
   jack_connect sooperlooper:common_out_2 pedal_bridge:return1_R
   jack_connect Hydrogen:out_L pedal_bridge:return1_L
   jack_connect Hydrogen:out_R pedal_bridge:return1_R
   ```

   Options : `--links 1,2`, `--target-ms X` (réserve par tampon, défaut
   automatique : une période JACK + un bloc PiPedal), `--stats S`,
   `--server NOM`, `--name NOM`. Le client attend tout seul que les plugins
   apparaissent, et suit les changements de fréquence ou de période.

Un seul Send et un seul Return par lien : si deux plugins visent le même lien,
le dernier activé travaille, l'autre laisse passer le signal sans y toucher.

### Comportement du Return

- **Attente** : il ne joue rien tant que le tampon n'a pas la réserve annoncée
  par l'écrivain (`writer_target`, 256 trames par défaut). Le surplus (vieux son
  accumulé pendant un arrêt) est jeté : la latence ne s'accumule pas.
- **Lecture** : fondu d'entrée sur le premier bloc.
- **Tampon vide** : fondu de sortie sur ce qui reste ; s'il ne reste rien, la
  dernière valeur jouée redescend en ligne droite à zéro. Sous-débit compté,
  retour en attente.
- Changements de mode et de gain lissés sur un bloc.

### Client JACK

- Un rééchantillonneur zita `VResampler` par sens (filtre de 32 points).
- Boucle PI sur le remplissage (passe-bas 0,3 s, ω = 2π·0,02 Hz, ζ = 1) ; la
  partie intégrale estime la dérive entre les horloges (affichée en ppm).
- Même logique d'attente / fondus / recalage que le Return, côté envoi.
- Côté retour, il n'écrit que si un Return est actif, et annonce sa réserve
  visée (`writer_target`) au plugin.
- Callback JACK sans allocation ni appel système ; ouverture des tampons et
  (re)construction des rééchantillonneurs dans le thread principal.

Mesures avec `test-jack.sh` (JACK dummy 48 kHz, PiPedal simulé 48 kHz / blocs de 128,
dérive ±100 ppm, aller-retour Send → JACK → Return) :

| Période JACK | Réserve par tampon | Aller-retour | Coupures |
|---|---|---|---|
| 256 | 10,7 ms (ancien défaut) | 22,7 ms | 0 à 2 (à-coups du système) |
| 256 | 8 ms (défaut actuel) | 17,6 ms | 0 |
| 128 | 5,3 ms (défaut actuel) | 12,0 ms | 0 |
| 128 | 5 ms | 11,5 ms | 0 |
| 64 | 6,7 ms / 4 ms | — | xruns JACK, coupures : à éviter |

CPU : client environ 1,7 % d'un cœur, serveur JACK dummy environ 0,7 %. La dérive
est estimée à ±300 ppm près en moins d'une minute.

### Mesures sur la vraie session (étape d)

`bash scripts/mesure.sh 600` (lecture seule) : 10 minutes avec le vrai PiPedal
(48 kHz, blocs de 128 × 4), JACK en période 128, Hydrogen qui joue, SooperLooper
en record/overdub/undo et guitare jouée.

| Mesure | Résultat |
|---|---|
| Sous-débits / sur-débits du pont | **0 / 0** |
| xruns JACK / xruns vus par SooperLooper | **0 / 0** |
| Latence aller-retour du pont (banc, période 128) | **12,0 ms** (12,3 ms au 2e essai) |
| Température max / bridage | 51,2 °C / aucun (`0x0`) |
| CPU total du Pi | 8 à 32 % (sur 4 cœurs) |

CPU moyen par programme (% d'un cœur) :

| Reste à terme | | Partira (VNC) | |
|---|---|---|---|
| PiPedal | 28,3 | x11vnc | 6,7 |
| Hydrogen (interface comprise) | 27,5 | slgui | 3,9 |
| Xvfb (pour Hydrogen) | 5,0 | | |
| SooperLooper | 3,9 | | |
| pedal_bridge | 2,0 | | |
| jackd | 0,8 | | |
| pb_timebase | 0,4 | | |
| **Total** | **≈ 68 % d'un cœur (≈ 17 % du Pi)** | | |

Dérive : l'estimation revient vers 0 ppm, mais toutes les ~148 s le remplissage
fait un saut de 20 à 30 trames (0,5 ms) dans les deux sens à la fois ; la
correction monte alors à 30-56 ppm, puis redescend en ~100 s. Effet inaudible
(écart de vitesse ≤ 0,006 %, résidu du sinus inchangé), aucune coupure. La cause
(PiPedal/ALSA ou minuterie du pilote dummy de JACK) reste à identifier.

**Attention** : ne pas lancer `test-jack.sh` ni `sl-sync-test.sh` pendant une
vraie session. Un 2e serveur JACK et le faux PiPedal temps réel provoquent des
xruns des deux côtés (mesuré : 2 xruns sur la vraie session, 12 sur le banc).

## Format du tampon (version 1)

Un fichier par lien et par sens : `/dev/shm/pedal-bridge-L<n>-send`
(PiPedal → JACK) et `/dev/shm/pedal-bridge-L<n>-return` (JACK → PiPedal).
Taille : 256 octets d'en-tête + 8192 trames × 2 canaux × float32 (65 792 octets).

| Octets | Ligne | Contenu |
|---|---|---|
| 0–63 | constantes | magic `PBRG` (publié en dernier), version, taille d'en-tête, canaux, capacité, lien, sens, uid du créateur |
| 64–127 | écrivain | `write_idx`, sur-débits, propriétaire, heartbeat, fréquence, taille de bloc, réserve visée |
| 128–191 | lecteur | `read_idx`, sous-débits, propriétaire, heartbeat, fréquence, taille de bloc |
| 192–255 | réservé | |

- File SPSC sans verrou : indices `uint64` qui ne font que monter
  (remplissage = `write_idx − read_idx`), acquire/release.
- **Présence du correspondant** : son heartbeat doit bouger. Sans changement
  pendant 250 ms, il est considéré absent. Le Send n'écrit que si un lecteur est
  présent ; sinon il fait seulement pass-through.
- **Propriété** : un champ propriétaire par côté ; la dernière instance activée
  prend la place (changement de preset dans PiPedal), les autres n'écrivent pas.
- **Création** : seuls les plugins (compte `pipedal_d`) créent les fichiers,
  en 0666 (`fchmod`, peu importe l'umask). Le client et pb_inspect ouvrent sans
  `O_CREAT`. Raisons : `fs.protected_regular=2` refuse `O_CREAT` sur le fichier
  d'un autre compte dans `/dev/shm`, et `RemoveIPC` de systemd efface les
  fichiers d'un compte ordinaire à sa déconnexion (pas ceux d'un compte système).
- **Fréquence et taille de bloc** : jamais codées en dur. Chaque côté inscrit sa
  fréquence dans l'en-tête ; la taille de bloc peut varier à chaque `run()`.

## Temps réel

Les 8 liens sont créés, mappés (`MAP_POPULATE`), verrouillés en RAM (`mlock`)
et touchés page par page à l'instanciation. `run()` ne fait que des atomiques,
du calcul et des copies. `scripts/test.sh` le vérifie :

- allocations pendant `run()` : comptées par interception de `malloc` ;
- appels système pendant `run()` : `strace` sur des milliers de blocs, avec
  changements de gain, de mode, de lien et des sous-débits.
