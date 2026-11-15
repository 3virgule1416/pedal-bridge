# Contrôle au pied : batterie, looper, écran — document de conception

État : **proposition à discuter**. Rien n'est encore codé, à part ce qui est
indiqué comme « existe ».

## 1. But

Jouer de la guitare dans PiPedal et, **sans toucher au PC ni à VNC** :

- lancer et arrêter la batterie (Hydrogen), changer de groove, déclencher un fill ;
- enregistrer, superposer, annuler des boucles (SooperLooper), passer d'une boucle
  à l'autre et enchaîner des sections (ex. boucle 1 × 4, boucle 2 × 4, retour à 1) ;
- régler le tempo ;
- tout ça au pied (MIDI Captain 10 + double switch), avec un retour visuel
  (DEL du Captain, écran OLED sur le Pi, voyants dans PiPedal).

Tout ce qui change quelque chose doit tomber **sur la mesure** (sur le 1, ou sur
le début du cycle), jamais au moment approximatif de l'appui.

## 2. Ce qui existe déjà

| Pièce | Rôle | État |
|---|---|---|
| Bridge Send / Bridge Return (LV2) | audio PiPedal ↔ JACK | fait, validé à l'oreille |
| `pedal_bridge` (client JACK) | rééchantillonnage, latence déclarée, compensation de SooperLooper par OSC | fait, validé |
| `pb_timebase` | maître du temps JACK : tempo, mesure et temps corrects | fait, validé (départ sur le 1) |
| `session.sh` | démarre JACK, VNC, SooperLooper, Hydrogen, le pont | fait |
| Outils de mesure | `pb_slhit`, `pb_slphase`, `pb_onsets`, `pb_transport` | faits |

Ce qu'on a appris et qui guide la suite :

- **Hydrogen 1.2.0-beta** publie une mesure fausse quand il est maître du temps.
  Il doit rester en écoute (J.Trans oui, J.Master non). Il adopte alors le tempo
  du maître (vérifié), et son champ BPM est bloqué.
- **SooperLooper 1.7.8** ne lit que le tempo et la position en trames du
  transport. Il place sa grille de cycles depuis la trame 0. Il ne lit pas la
  latence des ports JACK. Il n'a pas de mode « chanson ».
- **Changer le tempo pendant la lecture** fait sauter la position, puisque la
  mesure est calculée depuis la trame 0 (voir §8 pour la correction prévue).

## 3. Architecture

```
  MIDI Captain 10 ───── MIDI ──────────> PiPedal               écran OLED (SPI)
        ▲                                  ├─ liaisons ─> Bridge Batterie (LV2)
        │ SysEx (DEL), CC 20 (page)        ├─ liaisons ─> Bridge Looper   (LV2)
        │                                  │       │ commandes / état
  Axe I/O One ─ MIDI ─> routeur ── court ──┘       ▼ /dev/shm/pedal-bridge-control
  (double switch)       (dans le client)                                ▲
                          │ long : mode suivant                         │ état
                          ▼                                             │
                   pedal_bridge (le « cerveau ») ───────────────> pb_display
                     ├─ maître du temps JACK (pb_timebase intégré)
                     ├─ transport JACK (play/stop) ─> Hydrogen suit
                     ├─ OSC ─> SooperLooper ; OSC ─> Hydrogen (patterns, fills)
                     ├─ séquenceur de sections, calé sur la mesure
                     ├─ mode en cours ; DEL du Captain selon le mode
                     └─ audio : Send/Return (existe)
```

Principes :

1. **Les plugins sont des surfaces de contrôle.** Ils ne font que publier les
   valeurs de leurs contrôles et afficher l'état reçu. Aucune logique musicale,
   aucun réseau dans `run()`.
2. **Le client décide et calcule le moment.** Toute commande est exécutée par
   `pedal_bridge` au bon instant de la grille : début de mesure, de cycle ou de
   section.
3. **Une seule source de tempo** : le maître du temps intégré au client.
4. **Les liaisons MIDI restent dans PiPedal** (son interface habituelle). Seule
   exception : le double switch passe par le routeur du client (appui long, §7).

## 4. Communication plugins ↔ client

Nouveau fichier partagé `/dev/shm/pedal-bridge-control`, même principe que les
tampons audio : créé par les plugins en 0666, ouvert sans `O_CREAT` par le
client, écrivains uniques par champ, atomiques.

- **Commandes (écrites par les plugins)**
  - valeurs continues : tempo, boucle choisie, groove… (le client lit la valeur) ;
  - boutons : un **compteur par bouton**, incrémenté à chaque appui. Le client
    compare au dernier compteur vu : aucun appui n'est perdu, et aucun n'est
    exécuté deux fois.
- **État (écrit par le client)** : mesure, temps, tempo, transport, état de
  chaque boucle (vide, enregistrement, lecture, muet, attente), boucle choisie,
  section en cours et suivante, groove, fill en attente, et un compteur de
  version.
- Les plugins recopient l'état vers leurs **ports de sortie** : voyants et
  valeurs affichés par PiPedal.

## 5. Plugin « Bridge Batterie »

| Contrôle | Type | Effet (exécuté par le client) |
|---|---|---|
| Tempo | molette 40–240 BPM | nouveau tempo, appliqué au prochain début de mesure (§8) |
| Play / Stop | bouton | **global** : batterie et boucles ensemble, toujours depuis la mesure 1. Voir §8 bis (boucles « normalisées » pour que trigger tombe juste) |
| Début | bouton | retour à la mesure 1 |
| Groove | liste (1–N) | pattern Hydrogen, changé à la prochaine mesure |
| Fill | bouton | joue le pattern de fill pendant 1 mesure à la prochaine barre, puis revient au groove |
| Tap tempo | bouton | tempo calculé à partir des derniers appuis |
| Volume | dB | gain de la batterie (sur le retour, ou dans Hydrogen) |

Sorties (voyants) : **Temps** (1–4), **Sur le 1** (clignote d'une autre couleur
sur le premier temps), **En lecture**, **Fill en attente**.

## 6. Plugin « Bridge Looper »

| Contrôle | Type | Effet |
|---|---|---|
| Boucle | liste 1–8 | boucle sélectionnée |
| Record | bouton | enregistre (départ et fin quantifiés) |
| Overdub | bouton | superpose sur la boucle |
| Undo / Redo | boutons | annuler ou refaire la dernière couche |
| Mute | bouton | coupe ou réactive la boucle, à la fin du cycle |
| Effacer | bouton (appui long ?) | vide la boucle |
| Section suivante | bouton | passe à la section suivante au prochain cycle (§9) |
| Play / Stop | bouton | le même Play/Stop global que la batterie |
| Tempo | molette | **le même tempo** que la batterie (une seule valeur, deux endroits) |

Sorties : **État de la boucle** (vide, attente, enregistrement, lecture, muet),
**Temps** et **Sur le 1** (clignotant au tempo, couleur différente sur le 1),
**Section**.

À explorer dans SooperLooper : `multiply`, `insert`, `reverse`, `once` (jouer une
fois), `trigger`, `rate`/`stretch` (« t. stretch » : la boucle suit le tempo ?),
`sync`/`play sync`, la quantification du mute (`mute_quantized`), et les pistes
synchronisées entre elles (« sync to loop »).

## 7. MIDI Captain et modes

**Trois modes** : **PiPedal** (presets, effets, snapshots), **Batterie**,
**Looper**. Les 10 boutons du Captain changent de rôle selon le mode.

### Ce qu'on sait du Captain (firmware officiel), d'après `captain-manager`

Le dépôt voisin `captain-manager` (`pipedal_leds.py`, `blinker.py`,
`testmido.py`) a déjà vérifié sur le vrai matériel :

- **DEL pilotables par MIDI** : SysEx `F0 00 04 10 <touche 1-10> <R G B> × 3 F7`,
  couleurs sur 7 bits (0-127). Chaque touche a **3 DEL réglables séparément**.
- **Changer de page depuis le Pi** : **CC 20, valeur N** → le Captain saute à son
  preset (page) N, avec les CC et couleurs de cette page.
- Le Captain apparaît sur le Pi comme port MIDI « Feather RP2040 ».
- `pipedal_leds.py` suit PiPedal par son WebSocket (`ws://127.0.0.1/pipedal`,
  origine `http://pedalboard`) et colore les touches : effets liés par CC
  (actif/inactif) et snapshots (couleur de PiPedal, sélectionné ou pâle).
- `blinker.py` montre l'idée d'un **routeur MIDI** : il lit les pédales de
  l'Axe I/O One et renvoie lui-même suivant/précédent à PiPedal.

### Choix proposé : un routeur MIDI pour le double switch

Le double switch est branché sur l'Axe I/O One. Aujourd'hui, PiPedal écoute l'Axe
directement et change de preset **dès l'appui** (CC 65/67) : impossible d'y
ajouter un appui long. Proposition :

1. Dans les réglages MIDI de PiPedal, **ne plus écouter l'Axe directement**, mais
   un port virtuel du client. C'est un réglage de PiPedal : à faire **avec ton
   accord**.
2. Le client lit l'Axe et mesure la durée de chaque appui :
   - **appui court** (relâché avant ~0,5 s) → envoie « preset précédent/suivant »
     à PiPedal (comme aujourd'hui, mais au relâchement) ;
   - **appui long** sur un des deux boutons → **mode suivant/précédent** :
     CC 20 au Captain (sa page change), mise à jour des DEL et de l'écran.
3. Le Captain reste branché à PiPedal comme maintenant. Chaque page envoie ses
   propres CC, liés dans PiPedal aux contrôles de Bridge Batterie, Bridge Looper
   ou aux effets.

Contrepartie : en appui court, le changement de preset part au **relâchement** et
non à l'appui (quelques centaines de ms plus tard). À juger à l'usage. Sinon :
appui long sur une touche du Captain, si sa page le permet.

À vérifier : l'Axe envoie-t-il un message au **relâchement** du switch (valeur 0)
en plus de l'appui ? Sans ça, pas d'appui long possible sur le double switch.

### DEL : un seul responsable à la fois

`pipedal_leds.py` et le client ne doivent pas écrire en même temps dans les DEL :

- **mode PiPedal** : DEL d'effets et de snapshots (logique de `pipedal_leds.py`) ;
- **mode Batterie** : play (vert/rouge), groove choisi, fill en attente, temps qui
  clignote, le 1 d'une autre couleur ;
- **mode Looper** : état de chaque boucle (rouge = enregistrement, jaune =
  overdub, vert = lecture, pâle = muet, éteint = vide), clignotement au tempo.

Proposition : **un seul programme écrit dans les DEL**, et c'est celui qui connaît
le mode. Soit la logique de `pipedal_leds.py` est intégrée au système du client,
soit `pipedal_leds.py` reste en Python et reçoit le mode du client (il se tait
hors du mode PiPedal). À décider.

## 8. Tempo sans saut

Problème : SooperLooper place sa grille depuis la trame 0 avec le tempo actuel.
Si le tempo change en cours de lecture, la grille saute.

Correction prévue dans le maître du temps : appliquer un nouveau tempo **au
prochain début de mesure**, et à ce moment **repositionner le transport** à la
trame qui correspond à ce numéro de mesure au nouveau tempo
(`trame = (mesure − 1) × 4 temps × trames par temps`). Mesure, Hydrogen et grille
de SooperLooper restent alors d'accord, sans saut audible de la batterie. À
valider sur le banc (`sl-sync-test.sh`).

Les boucles déjà enregistrées à l'ancien tempo ne s'étirent pas, sauf si l'option
« t. stretch » de SooperLooper le fait proprement (à tester).

## 8 bis. Stop, Play et chansons sauvegardées

But : un **vrai stop** puis un **play depuis la mesure 1**, batterie et boucles
ensemble, et pouvoir **sauvegarder** boucles + batterie pour y revenir plus tard.

### Ce qu'on a mesuré (`pb_lag`, batterie enregistrée dans une boucle)

| Situation | Boucle vs batterie |
|---|---|
| Lecture continue après l'enregistrement | −22,67 ms (compensation de SooperLooper appliquée) |
| Relance par `trigger` (play depuis la mesure 1) | 0 ms : **compensation perdue** (22,7 ms de retard) |
| Idem avec toute la compensation à l'entrée | 0 ms : pareil |
| Pause/reprise (pause_on/pause_off + transport) | garde la compensation, mais +3 à 4 ms à **chaque** pause (cycles JACK différents) |
| **Boucle sauvée, audio décalé de 1088 trames, rechargée, puis `trigger`** | **−22,67 ms, identique 3 fois de suite** |

Conclusion : SooperLooper ne compense pas en modifiant l'audio, il **décale la
lecture**, et `trigger` oublie ce décalage. Si l'**audio de la boucle est
décalé lui-même** du même nombre de trames, `trigger` tombe juste, et une boucle
rechargée d'un fichier aussi.

### Fonctionnement prévu

- **Stop** : transport arrêté + boucles en pause. Puis, pendant l'arrêt, le
  client **normalise** chaque boucle qui n'est pas déjà normalisée :
  `save_loop` (WAV float), rotation de l'audio de D = entrée + sortie (1088
  trames aujourd'hui) vers l'avant, `load_loop`. Une boucle normalisée est
  marquée (le client garde la liste).
- **Play** : transport à la trame 0, `trigger` de toutes les boucles (armées, elles
  partent sur la première barre), départ du transport. Exact à la trame.
- **Sauvegarder une chanson** : dossier `~/pedal-bridge/chansons/<nom>/` avec les
  boucles normalisées (WAV), le tempo, le morceau Hydrogen (`.h2song`), les
  réglages utiles des boucles, et D au moment de l'enregistrement.
- **Recharger une chanson** : `load_loop` de chaque boucle, tempo, morceau
  Hydrogen, transport à la mesure 1. Puis Play.
- Si la latence change entre la sauvegarde et le rechargement (autre période
  JACK ou autres réglages de PiPedal), le client refait la rotation de la
  différence.

### À vérifier

- **Overdub** sur une boucle normalisée qui a été relancée par `trigger` : la
  nouvelle couche est-elle placée au bon endroit ? (SooperLooper applique sa
  compensation à l'overdub par rapport à la position de lecture.)
- Durée de `save_loop` + `load_loop` pour de longues boucles (le Stop doit rester
  rapide ; la normalisation peut se faire en arrière-plan pendant l'arrêt).
- Commandes OSC d'Hydrogen pour sauver/charger un morceau (sinon par son
  interface).

## 9. Sections (enchaîner les boucles)

SooperLooper n'a pas de mode chanson ; le client en fait un, calé sur la mesure.

- Une **section** = quelles boucles jouent (les autres sont muettes) + une
  longueur en cycles (ex. 4).
- Une **chanson** = une liste de sections, ex. `A×4, B×4, A×4`, en boucle ou non.
- Au pied :
  - « Section suivante » : bascule au prochain début de cycle ;
  - mode auto : la liste s'enchaîne toute seule.
- Bascule = mute et unmute envoyés par OSC **juste avant** la barre, avec la
  quantification du mute de SooperLooper pour que ça tombe pile.
- Définition des chansons : petit fichier `~/.config/pedal-bridge/chansons.json`
  au début. Plus tard, enregistrement au pied (« apprendre » : la section dure le
  nombre de cycles joués avant l'appui suivant).

## 10. Hydrogen : grooves et fills

- Le contrôle par OSC d'Hydrogen est **désactivé** dans `~/.hydrogen/hydrogen.conf`
  (`oscEnabled` = false). Il faudra l'activer (**avec ton accord**).
- **À vérifier dans la 1.2.0-beta** : les commandes OSC disponibles (choisir et
  jouer un pattern, pattern suivant, mode « stacked »), et si elles marchent
  quand Hydrogen est en écoute du maître du temps.
- Fill = pattern dédié, joué une mesure, puis retour au groove. Le client envoie
  le changement juste avant la barre.
- Alternative si l'OSC est trop limité : piloter Hydrogen par MIDI (notes et
  changements de programme), ou mettre à jour Hydrogen (§13).

## 11. Écran OLED (Waveshare 2,42 po, SSD1309, 128 × 64)

- **SPI** (réglage d'usine). Branchement prévu :

  | Module | Rôle | Pi (broche / GPIO) |
  |---|---|---|
  | VCC | 3,3 V | 1 |
  | GND | masse | 6 |
  | DIN | MOSI | 19 / GPIO 10 |
  | CLK | horloge | 23 / GPIO 11 |
  | CS | sélection | 24 / GPIO 8 (CE0) |
  | DC | données/commande | 22 / GPIO 25 |
  | RST | reset | 13 / GPIO 27 |

- À activer : `dtparam=spi=on` dans `/boot/firmware/config.txt`, puis
  redémarrage (**commande sudo donnée au moment voulu**). Le compte `pedalboard`
  est déjà dans les groupes `spi` et `gpio`.
- Programme `pb_display` (C, `spidev` + `libgpiod`, sans RPi.GPIO qui ne marche
  pas sur le Pi 5). Il lit l'état du client et le nom du preset PiPedal (par le
  WebSocket de PiPedal), et rafraîchit environ 20 fois par seconde.
- Écrans :
  - **PiPedal** : nom du preset, effets actifs ;
  - **Batterie** : tempo en gros, groove, fill en attente, mesure et temps ;
  - **Looper** : 8 cases de boucles avec leur état, boucle choisie, section
    (en cours → suivante), mesure et temps, avec le **1 mis en évidence**.
- Avant de recevoir l'écran : rendu en **images PNG** 128 × 64 pour régler la mise
  en page sur le PC.

## 12. Étapes de réalisation

Chaque étape est testée (banc JACK séparé quand c'est possible), puis validée à
l'oreille avant la suivante.

1. **Mesures (d)** : latence, dérive, xruns, CPU sur 10 minutes ; origine des
   xruns vus par SooperLooper.
2. **Modes et routeur MIDI** (§7) : vérifier que l'Axe envoie le relâchement du
   switch ; lire les pages du Captain (quels CC par page) ; routeur
   court/long dans le client ; changement de page par CC 20.
3. **Vérifier PiPedal** avec un plugin minimal : rendu des boutons momentanés
   (`lv2:trigger` ou équivalent), des voyants de sortie, liaison d'un CC à un
   bouton.
4. **Fichier de contrôle + Bridge Looper** (record, overdub, undo, mute, boucle,
   état) ; `pb_timebase` intégré au client.
5. **Bridge Batterie** : tempo, play/stop, début, tap ; tempo sans saut (§8).
6. **Hydrogen OSC** : grooves et fills (après accord pour `hydrogen.conf`).
7. **Sections** (§9).
8. **Écran** : rendu PNG, puis vrai écran après activation du SPI.
9. **DEL du Captain** par SysEx, selon le mode ; un seul programme responsable
   (intégration de `pipedal_leds.py`).

## 13. Questions ouvertes

- L'Axe I/O One envoie-t-il le relâchement du double switch (valeur 0) ?
- Pages du Captain : combien, et quels CC envoie chaque touche dans chaque page ?
  (Le firmware officiel se configure par fichiers sur son lecteur USB.)
- `pipedal_leds.py` : l'intégrer au client, ou le garder en Python et lui
  transmettre le mode ?
- PiPedal 2.0.111 : boutons momentanés, voyants de sortie, liaison MIDI sur un
  bouton momentané (appui ou relâchement) ?
- Hydrogen 1.2.0-beta : quelles commandes OSC ? Faut-il le mettre à jour (version
  stable 1.2.x compilée depuis les sources) ?
- SooperLooper : « t. stretch » utilisable au changement de tempo ?
- Changement de preset au relâchement (appui court) : acceptable à l'usage ?
