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

## 4 bis. Audio intégré aux plugins de contrôle (décidé)

Les plugins **Bridge Looper** et **Bridge Batterie** font **leur audio et leurs
contrôles** au même endroit de la chaîne. But : pouvoir enregistrer la guitare
**sèche** et choisir ou changer ses effets **après coup** (réamping), la
batterie restant hors des effets.

| Plugin | Audio | Lien fixe | Où le placer |
|---|---|---|---|
| **Bridge Looper** | copie l'entrée vers SooperLooper (Send) **puis** ajoute les boucles (Return, Add) | lien 1 (send + return) | **début** de chaîne : boucles sèches qui passent dans les effets ; **fin** : boucles enregistrées avec leurs effets |
| **Bridge Batterie** | ajoute la batterie (Return, Add) | lien 2 (return) | **fin** de chaîne : la batterie ne passe dans aucun effet |

- Dans Bridge Looper, la copie vers le looper se fait **avant** l'ajout des
  boucles : les boucles ne peuvent pas se réenregistrer dans elles-mêmes.
- **La position du plugin décide du son** (sec ou avec effets), sans réglage.
- Liens fixes : plus de numéro de lien à choisir. Branchements JACK :
  `pedal_bridge:send1` → SooperLooper, sortie de SooperLooper → `return1`,
  Hydrogen → `return2`. La compensation de SooperLooper reste calculée sur le
  lien 1 (envoi + retour du looper), comme aujourd'hui.
- **Versions mono et stéréo** de chaque plugin (le nombre de canaux d'un plugin
  LV2 est fixé dans sa description : pas une option). Même code, deux
  descriptions, comme ToobAmp (« TooB Tone (Mono) », « … (Stereo) »).
  - début d'une chaîne mono : **Bridge Looper (Mono)**, pour que les effets mono
    qui suivent (NAM) le restent ;
  - fin de chaîne après une réverb stéréo : versions **stéréo**.
  Le tampon partagé reste stéréo : la version mono écrit le même son à gauche
  et à droite, et additionne le retour en mono.
- Les **Bridge Send / Bridge Return** génériques restent, pour les usages
  particuliers (boucle sur sa propre branche d'effets en mode Replace, autre
  programme JACK…).
- À vérifier dans PiPedal : qu'un plugin mono en début de chaîne garde bien le
  reste de la chaîne en mono, et ce que fait PiPedal d'un plugin stéréo placé
  dans la partie mono.

## 5. Plugin « Bridge Batterie »

Audio : retour de la batterie, lien 2 (§4 bis). Mono et stéréo.

| Contrôle | Type | Effet (exécuté par le client) |
|---|---|---|
| Tempo | molette 40–240 BPM | nouveau tempo, appliqué au prochain début de mesure (§8) |
| Play / Stop | bouton | **global** : batterie et boucles ensemble, toujours depuis la mesure 1. Voir §8 bis (boucles « normalisées » pour que trigger tombe juste) |
| Début | bouton | retour à la mesure 1 |
| Morceau | **sélecteur de fichier** (`.h2song`) | Hydrogen ouvre le morceau (`OPEN_SONG`), le client lit ses patterns (voir ci-dessous) |
| Groove | **sélecteur de fichier** (un fichier par groove) ; secours : liste « Groove 1 à 16 » | pattern Hydrogen, changé à la prochaine mesure ; cycle de SooperLooper = sa longueur |
| Groove suivant / précédent | boutons | pour le pied (un choix de fichier ne se lie pas à une pédale) |
| Fill | bouton | joue le pattern de fill pendant 1 mesure à la prochaine barre, puis revient au groove |
| Tap tempo | bouton | tempo calculé à partir des derniers appuis |
| Volume | dB | gain de la batterie (sur le retour, ou dans Hydrogen) |

Sorties (voyants) : **Temps** (1–4), **Sur le 1** (clignote d'une autre couleur
sur le premier temps), **En lecture**, **Fill en attente**, **Groove refusé**.

### Choisir le morceau et le groove par leur vrai nom

Un menu déroulant LV2 a une liste **fixée dans la description du plugin** : il
ne peut pas montrer les patterns d'un morceau. Le **sélecteur de fichier** de
PiPedal (celui du NAM de ToobAmp) liste, lui, le **contenu d'un dossier** au
moment où on l'ouvre. On s'en sert :

- **Morceau** : un champ fichier qui liste les `.h2song` d'un dossier.
- **Groove** : quand un morceau est chargé, un dossier reçoit **un fichier vide
  par groove compatible**, nommé avec le vrai nom et la longueur, par exemple
  `03 - main 2-1 (1 mesure).groove`, `12 - refrain (2 mesures).groove`. Les
  patterns incompatibles (pas en 4/4, longueur pas entière) **n'y sont pas** :
  ils ne sont pas proposés.
- Choisir un fichier : le plugin transmet le chemin au client par le fichier de
  contrôle (§4), le client lit le numéro au début du nom, sélectionne le pattern
  (`SELECT_AND_PLAY_PATTERN`) et règle le cycle.
- **Secours** si le sélecteur ne convient pas : menu « Groove 1 à 16 » ; le vrai
  nom et la longueur s'affichent sur l'écran OLED (et l'écran du Captain) ; un
  numéro inexistant ou incompatible allume « Groove refusé » et le groove
  précédent reste.

À vérifier (plugin minimal, étape 3 du §12) :

- **Dossiers acceptés** par le sélecteur de PiPedal. Probablement seulement les
  siens (`/var/pipedal/audio_uploads/…`), qui appartiennent à `pipedal_d` : le
  client (compte `pedalboard`) n'a pas le droit d'y écrire. Deux solutions :
  le **plugin crée lui-même les fichiers** (il tourne dans PiPedal ; le client
  lui passe la liste des grooves ; écriture hors de `run()`, par le worker LV2),
  ou ajouter `pedalboard` au groupe `pipedal_d` (changement système : avec
  accord).
- **Mise à jour de la liste** quand on change de morceau (relue à l'ouverture ?).
- Ce que PiPedal **enregistre dans le preset** pour un champ fichier (chemin
  complet ?), et ce qui se passe si le fichier n'existe plus.
- Liaison MIDI : impossible sur un choix de fichier, d'où les boutons
  suivant / précédent.

## 6. Plugin « Bridge Looper »

Audio : envoi de l'entrée au looper puis ajout des boucles, lien 1 (§4 bis).
Mono et stéréo. Plus un réglage de **gain des boucles**.

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

## 6 bis. Jouer avec ou sans batterie ; mode tempo et mode libre

### Batterie audible ou non : la présence du plugin décide

- Le **tempo et la grille des mesures** viennent de `pb_timebase`, pas
  d'Hydrogen : SooperLooper se cale (départs, arrêts, longueurs) **sans aucune
  batterie**.
- La batterie n'arrive dans PiPedal **que par Bridge Batterie**. Preset avec
  **seulement Bridge Looper** : on n'entend pas la batterie (le pont n'écrit
  même rien quand aucun plugin n'écoute). On **ajoute Bridge Batterie** (ou on
  passe à un preset qui l'a) : la batterie apparaît, déjà calée.
- Hydrogen **reste lancé**, silencieux, pour être prêt (≈ 27 % d'un cœur).
- Repères sans batterie : DEL du Captain qui clignotent au tempo (autre couleur
  sur le 1), écran OLED (mesure, temps). En option, un **clic** : métronome
  d'Hydrogen par OSC (`TOGGLE_METRONOME`), comme le genre « GUIDE » du RC-600.
- C'est l'usage « rythme éteint » des Boss RC : la mémoire garde un tempo et une
  mesure qui servent au calage des pistes.

### Mode tempo (mode principal)

- Le tempo est choisi **avant** d'enregistrer : réglage BPM (plugin Looper ou
  Batterie, commande `tempo`), ou **tap tempo** au pied.
- Mesure : 4/4 aujourd'hui ; 3/4, 6/8 à faire (`pb_timebase` à N temps).
- Tout est quantifié : départ et fin d'enregistrement sur la grille, longueur =
  nombre entier de cycles. La batterie ajoutée plus tard tombe juste.

### Mode libre (option)

- **Pas de tempo au départ** : la **1re boucle** enregistrée fixe la longueur.
  Elle commence et finit **exactement aux appuis** (rien à quantifier).
- Le client en déduit le tempo : `tempo = mesures × temps par mesure × 60 / durée`,
  le donne à `pb_timebase`, place la grille pour que le **début de la boucle
  soit le 1**, puis les boucles suivantes se calent normalement (mode tempo).
- **Nombre de mesures choisi d'avance** (1, 2, 4, 8 ; réglage MEASURE du RC-600) :
  sinon le tempo peut être faux du simple au double, ou ne pas tomber sur la
  phrase (phrase de 3 mesures comptée comme 4). Option : choix automatique du
  nombre qui donne un tempo entre 70 et 140 BPM, en signalant l'hypothèse.
- **Limites** :
  - la précision dépend des appuis : un départ en retard place le « 1 » de la
    future batterie à l'appui, pas sur le premier temps joué ; un départ ou une
    fin imprécis fait un petit accroc à chaque tour (vrai pour tout looper
    libre) ;
  - à faire au pied (Captain, double switch), pas par VNC (50 à 150 ms de délai).
- Usage conseillé : boucler seul. Pour ajouter une batterie ensuite, préférer le
  mode tempo (BPM d'avance ou tap tempo).

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

### Réalisé dans le client (`client/controle.cc`)

Commandes texte par UDP (`pedal_bridge --cmd "..."`, ou `session.sh play`,
`pause`, `save NOM`, `load NOM`, `list`, `boucles`) :

- **stop** : transport arrêté + boucles en pause, puis **normalisation** des
  boucles qui en ont besoin : `save_loop` (WAV float, ~20 ms pour 4 s),
  rotation de l'audio de D = latence envoi + retour **au moment de
  l'enregistrement** de la boucle (1088 trames aujourd'hui),
  `load_loop`. Refusé pendant un enregistrement en cours.
- **play** : stop (normalise au besoin), transport à la trame 0, `trigger` de
  toutes les boucles (armées, elles partent sur la première barre), départ du
  transport. Toujours depuis la mesure 1.
- **save NOM** : `~/pedal-bridge/chansons/NOM/` : `boucleN.wav` (normalisées),
  `chanson.txt` (tempo, D, fréquence, longueurs).
- **load NOM** : `load_loop` de chaque boucle (les autres sont vidées), tempo
  envoyé à `pb_timebase`, transport à la mesure 1, **sans aucun décalage**.
- Seul un **enregistrement neuf** (état record) rend une boucle « à normaliser » :
  un overdub sur une boucle normalisée tombe juste (mesuré : pic unique à
  −22,67 ms après overdub de la même batterie).
- **Une boucle normalisée est définitive.** Au playback, la boucle et la batterie
  passent par le même chemin de retour : elles restent alignées même si la
  latence change ensuite (période JACK, réglages de PiPedal). Mesuré : boucle
  normalisée avec D = 1088, puis pont relancé avec D = 1536 : toujours −22,67 ms,
  et chanson sauvée à 1088 rechargée à 1536 : −22,67 ms.
- **État gardé** dans `~/pedal-bridge/travail/etat.txt` (index, longueur, D) :
  après un redémarrage du pont, les boucles déjà normalisées sont reconnues
  (même longueur) et ne sont pas décalées deux fois.

Mesures (`pb_lag`, batterie branchée directement dans la boucle 2) : −22,67 ms
en lecture continue, après 3 × stop + play, après save + load + play, et après
un redémarrage du pont. Partout identique.

### Limites connues

- **Ne pas changer la période JACK pendant que SooperLooper tourne** : il
  plante (constaté, boucles perdues). `session.sh start` refuse maintenant de
  la changer dans ce cas.
- **Hydrogen** (fait, §10) : `save` écrit aussi `NOM.h2song` (`SAVE_SONG_AS`),
  `mode=pattern` et le groove retenu ; `load` rouvre le morceau (`OPEN_SONG`),
  remet le mode Pattern, le groove et l'affichage du tempo. Vérifié à l'écran
  (titre, pattern, BPM). Le groove n'est connu que s'il a été choisi par
  `groove N` : Hydrogen n'annonce pas un pattern cliqué dans son interface.
  `SAVE_SONG_AS` fait aussi de ce fichier le morceau « courant » d'Hydrogen.
- **Cycle = longueur du groove** (fait) : le client lit la taille des patterns
  dans le `.h2song` (au `save` et au `load` ; 48 ticks par noire, 192 = une
  mesure de 4/4) et, à chaque `groove N` ou `load`, règle `eighth_per_cycle` de
  SooperLooper = taille / 24. Un groove de 2 mesures donne un cycle de 2
  mesures : l'enregistrement part au **début du groove** et finit à sa **fin**.
  Mesuré avec un groove de test (A = 4 coups, B = 1 coup) : A sur les mesures
  1, 3, 5… ; record appuyé pendant A (mesure 1) → départ mesure 3, stop pendant
  B (mesure 4) → fin de la mesure 4 (2 mesures) ; record pendant B (mesure 8)
  → départ mesure 9, stop pendant A (mesure 11) → fin de la mesure 12
  (4 mesures).
- Le cycle n'est réglé que pour un morceau passé par `save` ou `load` (taille
  des patterns connue) ; sinon, 1 mesure.
- **Mesures autres que 4/4** (3/4, 6/8) : pas encore gérées (`pb_timebase` reste
  à 4 temps par mesure) ; le client laisse alors le cycle tel quel.
- Hydrogen crée un fichier `.NOM.autosave.h2song` dans le dossier du morceau
  ouvert.
- Un **undo** qui ramène une boucle à son contenu d'avant la normalisation
  (annuler le chargement) n'est pas détecté.
- Une boucle réenregistrée **pendant que le pont est arrêté**, avec exactement
  la même longueur, serait prise pour normalisée.

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

- **OSC d'Hydrogen activé** (accord donné) : `oscEnabled` = true dans
  `~/.hydrogen/hydrogen.conf` (sauvegarde : `hydrogen.conf.bak-20261114-224403`).
  Hydrogen écoute en UDP sur le port **9000, toutes interfaces** (accessible
  depuis le réseau local, comme l'OSC de SooperLooper).
- **Retour d'état** : un programme qui lui a écrit reçoit l'état de la table de
  mixage (volume, pan, mute, solo de chaque instrument), le métronome et le mute
  général. **Pas** le tempo, le mode ni le pattern sélectionné : c'est donc notre
  système qui doit retenir le pattern qu'il a demandé.
- **Commandes présentes dans la 1.2.0-beta** (liste tirée du programme) :
  `OPEN_SONG`, `SAVE_SONG`, `SAVE_SONG_AS`, `NEW_SONG`, `SELECT_AND_PLAY_PATTERN`,
  `SELECT_NEXT_PATTERN`, `SELECT_ONLY_NEXT_PATTERN`, `SONG_MODE_ACTIVATION`,
  `LOOP_MODE_ACTIVATION`, `BPM`, `BPM_INCR`/`DECR`, `TAP_TEMPO`, `PLAY`, `STOP`,
  `PAUSE`, `RELOCATE`, `NEXT_BAR`, `PREVIOUS_BAR`, `LOAD_DRUMKIT`,
  `TOGGLE_METRONOME`, `MUTE`/`UNMUTE`, `JACK_TRANSPORT_ACTIVATION`,
  `JACK_TIMEBASE_MASTER_ACTIVATION`, table de mixage par instrument.
  À tester une par une (arguments, comportement en écoute du maître du temps).
- Pour une chanson : `SAVE_SONG_AS` (le `.h2song` dans le dossier de la chanson),
  `OPEN_SONG` au chargement, mode (`SONG_MODE_ACTIVATION`), pattern retenu par
  le client (`SELECT_AND_PLAY_PATTERN`), et `BPM` pour que l'affichage
  d'Hydrogen suive `pb_timebase`.
- **Mode Pattern** : `SONG_MODE_ACTIVATION 0` met Hydrogen en mode Pattern
  (vérifié depuis le mode Song). `session.sh start` l'envoie à chaque démarrage.
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
   bouton, comportement d'un plugin mono ou stéréo placé en début de chaîne
   mono (§4 bis), et **sélecteur de fichier** : dossiers acceptés, droits
   d'écriture, mise à jour de la liste, ce qui est gardé dans le preset (§5).
4. **Fichier de contrôle + Bridge Looper** (mono et stéréo, audio intégré :
   envoi + retour des boucles sur le lien 1 ; record, overdub, undo, mute,
   boucle, état) ; `pb_timebase` intégré au client.
5. **Bridge Batterie** (mono et stéréo, retour de la batterie sur le lien 2) :
   tempo, play/stop, début, tap ; tempo sans saut (§8). Branchements JACK :
   SooperLooper → `return1`, Hydrogen → `return2`.
6. **Hydrogen OSC** : grooves et fills (OSC activé ; `groove N` fait).
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
