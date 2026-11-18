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
| DLS Send / DLS Return (LV2) | audio PiPedal ↔ JACK | fait, validé à l'oreille |
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
        ▲                                  ├─ liaisons ─> DLS Drum (LV2)
        │ SysEx (DEL), CC 20 (page)        ├─ liaisons ─> DLS Looper   (LV2)
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

Les plugins **DLS Looper** et **DLS Drum** font **leur audio et leurs
contrôles** au même endroit de la chaîne. But : pouvoir enregistrer la guitare
**sèche** et choisir ou changer ses effets **après coup** (réamping), la
batterie restant hors des effets.

| Plugin | Audio | Lien fixe | Où le placer |
|---|---|---|---|
| **DLS Looper** | copie l'entrée vers SooperLooper (Send) **puis** ajoute les boucles (Return, Add) | lien 1 (send + return) | **début** de chaîne : boucles sèches qui passent dans les effets ; **fin** : boucles enregistrées avec leurs effets |
| **DLS Drum** | ajoute la batterie (Return, Add) | lien 2 (return) | **fin** de chaîne : la batterie ne passe dans aucun effet |

- Dans DLS Looper, la copie vers le looper se fait **avant** l'ajout des
  boucles : les boucles ne peuvent pas se réenregistrer dans elles-mêmes.
- **La position du plugin décide du son** (sec ou avec effets), sans réglage.
- Liens fixes : plus de numéro de lien à choisir. Branchements JACK :
  `pedal_bridge:send1` → SooperLooper, sortie de SooperLooper → `return1`,
  Hydrogen → `return2`. La compensation de SooperLooper reste calculée sur le
  lien 1 (envoi + retour du looper), comme aujourd'hui.
- **Versions mono et stéréo** de chaque plugin (le nombre de canaux d'un plugin
  LV2 est fixé dans sa description : pas une option). Même code, deux
  descriptions, comme ToobAmp (« TooB Tone (Mono) », « … (Stereo) »).
  - début d'une chaîne mono : **DLS Looper (Mono)**, pour que les effets mono
    qui suivent (NAM) le restent ;
  - fin de chaîne après une réverb stéréo : versions **stéréo**.
  Le tampon partagé reste stéréo : la version mono écrit le même son à gauche
  et à droite, et additionne le retour en mono.
- Les **DLS Send / DLS Return** génériques restent, pour les usages
  particuliers (boucle sur sa propre branche d'effets en mode Replace, autre
  programme JACK…).
- À vérifier dans PiPedal : qu'un plugin mono en début de chaîne garde bien le
  reste de la chaîne en mono, et ce que fait PiPedal d'un plugin stéréo placé
  dans la partie mono.

## 5. Plugin « DLS Drum »

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
  (`SELECT_ONLY_NEXT_PATTERN`, sans démarrer) et règle le cycle.
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

**Résultats avec DLS Test** (PiPedal 2.0.111, 2026-11-15) :

- Le plugin crée lui-même `/var/pipedal/audio_uploads/PedalBridgeTest/` et ses
  fichiers à l'instanciation (« Dossier créé » allumé) : c'est la solution
  retenue, aucun changement système.
- Le sélecteur (`pipedal_ui:directory "PedalBridgeTest"`) liste les fichiers
  du dossier, filtrés par extension, et affiche le nom **sans l'extension**.
  Le plugin reçoit le chemin (patch:Set, atom:Path).
- Le choix est **gardé dans le preset** (changer de preset puis revenir : le
  fichier est encore sélectionné).
- À voir encore : fichier supprimé entre-temps, liste mise à jour pendant que
  le sélecteur est ouvert.

### Réalisé (1re version, 2026-11-15)

`lv2/drum.c`, URI `…/drum-mono` et `…/drum-stereo` (« DLS Drum », « DLS Drum
St. »). Batterie du lien 2 **ajoutée** au signal (en mono : (G + D) / 2) ; à
placer **en fin de chaîne** (la batterie ne passe dans aucun effet). Le Looper
reste en début de chaîne (lien 1). Testé à l'oreille et sur le Pi.

| Section | Contrôle | Effet |
|---|---|---|
| Drums | **Groove** (menu 1–16) | pattern N du morceau d'Hydrogen, **sans démarrer** |
| | **Previous / Next** | groove précédent / suivant (bornés au nombre de patterns lus dans le `.h2song`) |
| | **Playing** (sortie) | groove qui joue vraiment (« - » si inconnu) |
| Transport | **Play / Stop** | le même que celui du Looper (§8 bis) ; marche aussi sans SooperLooper (batterie seule) |
| | **Tempo** (40–240) | appliqué quand le bouton ne bouge plus depuis 0,3 s |
| | **BPM + / BPM -** (momentanés) | ±1 BPM ; tenus : répétition après 0,4 s (toutes les 120 ms), plus vite après 2 s (40 ms) |
| | **Tap** | au moins 3 coups ; le tempo change **une fois**, 1 s après le dernier coup (1,6 intervalle aux tempos lents), BPM entier, moyenne des 8 derniers coups |
| | **Metronome** (bascule) | métronome d'Hydrogen |
| | Beat, ♩, 1, ▶, BPM (sorties) | comme le Looper ; Beat compte 3 ou 4 temps selon le groove |
| Bridge | Volume, Link | gain de la batterie ; état du lien 2 |

Comportements à retenir :

- **Chargement du preset** (ou redémarrage du pont) : le groove du menu et
  l'interrupteur Metronome sont **appliqués**, sans démarrer la batterie. Les
  autres réglages (tempo) ne s'appliquent qu'en changeant.
- **Groove sans démarrer** : `SELECT_ONLY_NEXT_PATTERN` (Hydrogen choisit le
  pattern tout de suite en mode « pattern sélectionné »). `SELECT_AND_PLAY_PATTERN`
  démarrait le transport : seul Play démarre maintenant.
- **Métronome** : l'OSC d'Hydrogen n'a qu'une bascule (`TOGGLE_METRONOME`,
  argument ignoré). Le client lit le **retour d'état** d'Hydrogen
  (`/Hydrogen/TOGGLE_METRONOME` 0 ou 1, envoyé à toute adresse qui lui a parlé)
  et ne bascule que si l'état diffère. État inconnu : un message d'une adresse
  nouvelle pour Hydrogen lui fait renvoyer tout son état.
- **BPM + / -** : le plugin compte les pas (répétition dans `run()`, sans
  appel système) ; le client les regroupe (10 tempos par seconde au plus,
  sinon Hydrogen déborde) et part toujours du **dernier tempo appliqué**.

**Affichage dans PiPedal par son websocket** (`client/pipedal_ws.cc`) :

- Un plugin LV2 ne peut pas changer ses propres réglages. Après Next/Previous,
  Tap ou BPM +/-, le client remet **Groove** (Drum) et **Tempo** (Drum et
  Looper) à la bonne valeur comme le fait la page web : connexion à
  `ws://127.0.0.1:80/pipedal`, `currentPedalboard` (pour trouver les
  `instanceId` par URI), puis `setControl` ; connexion fermée aussitôt (pas de
  `hello` : pas d'abonnement aux notifications). PiPedal marque le preset
  modifié, comme un changement à la main.
- **Échos** : le plugin renvoie ensuite la valeur reçue. Chaque valeur envoyée
  est **attendue et ignorée une fois** (oubliée après 2 s) ; sans ça, un écho en
  retard ramenait un ancien tempo après un Tap.
- Le bouton qu'on **tourne** ne reçoit pas sa propre valeur (sinon il se bat
  avec le geste) ; pendant qu'on **tient** BPM +/-, les boutons Tempo ne sont
  mis à jour qu'au relâchement.
- `PB_PIPEDAL_PORT=0` désactive tout ça (banc de test).
- Le pas des flèches de la case Tempo vient de PiPedal : 10 si la plage dépasse
  100 (40–240), 1 sinon. D'où BPM + / BPM -.

**Texte dans PiPedal** : un plugin ne peut afficher que des étiquettes
**écrites d'avance** dans sa description (`lv2:scalePoint`) ; aucun texte qui
change pendant que ça tourne (`pipedal_ui:text` existe mais n'est pas lu). Le
nom des patterns ira donc dans la description à l'installation (collection
fixe) et sur l'écran OLED (§11).

## 6. Plugin « DLS Looper »

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

### Réalisé (1re version, 2026-11-15)

- Plugins **DLS Looper (Mono)** (`.../looper-mono`, étiquette « DLS Looper ») et
  **DLS Looper (Stéréo)** (`.../looper-stereo`, « DLS Looper St. »), dans le
  bundle principal. TTL généré par `scripts/gen_looper_ttl.py`.
- Audio sur le lien 1 (`lv2/io.c`, même comportement que Send/Return en mode
  Add) : l'entrée part vers SooperLooper **avant** l'ajout des boucles. Mono :
  même son à gauche et à droite vers le looper, retour ramené à (G + D) / 2.
- Contrôles : Boucle (1-8), Record, Overdub, Undo, Redo, Mute, Effacer,
  Play / Stop (boutons momentanés), Tempo (40-240, appliqué quand on le
  **change** : charger un preset ne change pas le tempo), Volume boucles.
- Sorties : State (texte : offline, empty, waiting, recording, overdub,
  playing, muted, paused, other), Beat (texte), voyants ♩ (chaque temps, vert),
  1 (premier temps, rouge), ▶ (lecture), BPM (texte), Link (no bridge, no
  audio, OK).
- Interface en anglais (noms affichés seulement : les symboles des ports, que
  les presets gardent, restent les mêmes). Trois sections (pg:group) : Loop,
  Transport, Bridge ; les ports sont rangés par section, car PiPedal ouvre une
  nouvelle boîte dès que la section change d'un port au suivant. Les voyants
  ont un nom d'un caractère (un voyant est trop étroit pour un mot).
- **Fichier de contrôle** `/dev/shm/pedal-bridge-control` (`common/pb_ctl.h`) :
  créé par le plugin ; compteurs de boutons ; la dernière instance activée
  commande (changement de preset) ; le client se recale sur ses compteurs sans
  rien rejouer. Le client y écrit le transport (callback JACK) et l'état des
  boucles (toutes les 0,2 s, et juste après chaque commande). Il lit les
  boutons toutes les 5 ms.
- Exécution dans le client (`controle.cc`) : `/sl/N/hit` record, overdub,
  undo, redo, mute, undo_all sur la boucle choisie (et `selected_loop_num`
  pour l'interface de SooperLooper) ; Play / Stop = `stop` (avec normalisation)
  si le transport roule, sinon `play` depuis la mesure 1.
- Outils : `pb_ctl` (lire l'état, simuler les boutons), banc
  `scripts/test-looper.sh` (JACK séparé, SooperLooper, pb_timebase, client) :
  tempo, play, record (départ et fin au cycle), boucle 2, mute, overdub,
  stop avec normalisation, play depuis la mesure 1, effacer, changement
  d'instance. Tout passe. Test hors ligne `test_looper` (40 vérifications,
  aucun appel système dans `run()`).
- **Mesure en direct à travers le plugin** (2026-11-15, période 128, `pb_lag`) :
  batterie amenée dans la chaîne par DLS Return (lien 2) placé avant DLS
  Looper, enregistrée en boucle avec les boutons du plugin. Boucle vs batterie :
  −10,6 ms juste après l'enregistrement, −10,6 ms après Stop (normalisation)
  puis Play, trois fois. Attendu : −10,7 ms = latence propre de PiPedal
  (entrée + sortie de l'Axe), que la batterie ne traverse pas dans ce test.
  Le trajet du plugin et du pont est donc exact.
- **Test au médiator** (même jour, métronome d'Hydrogen, coups étouffés sur
  chaque temps, `pb_onsets` sur la boucle) : 12 coups, écart moyen au temps
  **−6,4 ms**, dispersion 22 ms, sans dérive. Pas de latence cachée de l'Axe
  mesurable (elle donnerait un retard constant) : les 10,7 ms comptées
  suffisent. Un premier essai qui accélérait (jusqu'à −136 ms à la 4e mesure)
  montre que la sensation de décalage venait du jeu, pas du pont.
- **Batterie sur le lien 2** (`session.sh`, liens 1,2 par défaut) : DLS Looper
  au début de la chaîne, DLS Return (lien 2, Add) à la fin. Vérifié à
  l'oreille : batterie sans effets. Remplissage des deux retours à quelques
  trames l'un de l'autre (251,0 et 252,2 pour 256 visées, soit < 0,1 ms) :
  boucles et batterie restent alignées.
- Limites : SooperLooper suit le transport : **Record ne part que si le
  transport roule** (appuyer sur Play d'abord). La batterie arrive aussi par le
  lien 1 tant que DLS Drum n'existe pas (branchement de `session.sh`). Ne pas
  mettre DLS Send / DLS Return sur le lien 1 en même temps que DLS Looper : la
  dernière instance activée prend le lien.

À explorer dans SooperLooper : `multiply`, `insert`, `reverse`, `once` (jouer une
fois), `trigger`, `rate`/`stretch` (« t. stretch » : la boucle suit le tempo ?),
`sync`/`play sync`, la quantification du mute (`mute_quantized`), et les pistes
synchronisées entre elles (« sync to loop »).

## 6 bis. Jouer avec ou sans batterie ; mode tempo et mode libre

### Batterie audible ou non : la présence du plugin décide

- Le **tempo et la grille des mesures** viennent de `pb_timebase`, pas
  d'Hydrogen : SooperLooper se cale (départs, arrêts, longueurs) **sans aucune
  batterie**.
- La batterie n'arrive dans PiPedal **que par DLS Drum**. Preset avec
  **seulement DLS Looper** : on n'entend pas la batterie (le pont n'écrit
  même rien quand aucun plugin n'écoute). On **ajoute DLS Drum** (ou on
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
   propres CC, liés dans PiPedal aux contrôles de DLS Drum, DLS Looper
   ou aux effets.

Contrepartie : en appui court, le changement de preset part au **relâchement** et
non à l'appui (quelques centaines de ms plus tard). À juger à l'usage. Sinon :
appui long sur une touche du Captain, si sa page le permet.

À vérifier : l'Axe envoie-t-il un message au **relâchement** du switch (valeur 0)
en plus de l'appui ? Sans ça, pas d'appui long possible sur le double switch.

### Disposition décidée (2026-11-17) : double switch et page DrumKit

Le double switch reste **le même dans tous les modes** : il porte ce qui sert
partout. Il ne change donc plus de preset (CC 65/67) : le changement de preset
passe sur la page du mode PiPedal.

| Double switch | Court | Long |
|---|---|---|
| **A** | Play / Stop (batterie et boucles) | **Outro** : la batterie finit la toune, puis tout s'arrête |
| **B** | **Partie suivante** (couplet → refrain…) | **Changer de mode** (DrumKit → Looper → PiPedal) |

Page **DrumKit** du Captain : la rangée du **bas** (la plus facile d'accès) sert
**en jouant**, celle du **haut** sert à **explorer et préparer**. *Long* = appui
long (le pont mesure la durée de l'appui : le Captain doit envoyer l'appui ET le
relâchement).

| | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| **Haut** (explorer, préparer) | Tap | BPM - | BPM + | Groove ◀ | Groove ▶ |
| *long* | Métronome on/off | BPM - en continu | BPM + en continu | Beat ◀ | Beat ▶ |
| *très long* (> 1,5 s) | — | — | — | Style ◀ | Style ▶ |
| **Bas** (en jouant) | Fill | Variation | Batterie on/off | Partie ◀ | Break |
| *long* | Crash / accent | Batterie légère | — | Intro | Fill long |

Navigation au pied dans la liste Song (Style / Groupe / Beat) : **Groove ◀ ▶**
(court) parcourt les patterns du beat ; **Beat ◀ ▶** (long) parcourt les beats
du style **en file**, d'un groupe à l'autre ; **Style ◀ ▶** (très long, plus de
1,5 s) change de style. Le tempo ne change jamais avec ces choix. Sans écran,
on ne voit pas où on est : l'écran OLED (§11) affichera « Style / Groupe /
Beat / Groove ».

La rangée du haut est fixée. La rangée du bas est la proposition retenue ; on la
confirmera après avoir essayé les fonctions dans PiPedal (boutons de DLS Drum),
puisque les boutons du Captain ne sont que des réglages.

### Rangement de la liste Song (fait, 2026-11-18)

Trois niveaux partout : **Style / Groupe (type de beat) / Beat**, par
`scripts/ranger.py` (fichiers déplacés, rien d'effacé ; `.alias.txt` dans le
dossier : le pont retrouve la chanson d'un ancien preset). Les noms ne répètent
pas le style et n'ont pas de tempo (le tempo est toujours celui choisi ; celui
d'origine reste dans les `.parts.json`). Les ensembles JJ sont coupés par
famille (Straight, Backbeat, grooves 4/4, Odd Meters), chacune avec les
fills et intros de sa plage, nommée par son allure (slow, medium, fast, very
fast, extreme ; noms en anglais). Exemples : `Metal/Straight/Lamb Chops very fast`,
`Rock/Straight/Classic S03`, `Latin/Bossa and Samba/Bossa`. Gros morceau :
3 134 patterns, ouvert en 3,5 s.

Au pied : Beat ◀ ▶ (appui long sur Groove ◀ ▶) fait défiler les beats du style ; Style ◀ ▶ : appui très long (voir le tableau)
en file, d'un groupe à l'autre.

### Structure des parties et Fill (fait, 2026-11-18)

- `gros_morceau.py` écrit `<kit>.h2song.parts.txt` : pour chaque chanson,
  `part:<Partie>` (grooves), `fill:<Partie>`, `fill:*` (fills communs),
  `intro`, `outro`, `transition`, `hits` -> numéros de groove dans la chanson.
  Rôle tiré de la catégorie (midi2h2 : la partie) ou du nom (DLS : Verse,
  Chorus fill… ; RP Bardet : Pattern = groove, Break = fill). Exemple Classic
  S03 : intro 1-3, Verse 4-13 + fills 14-19, Chorus 20-27 + fills 28-31,
  transitions 32-33, Bridge 34-35, outro 37-39.
- **Fill** (bouton de DLS Drum, `PB_DBTN_FILL`) : un fill de la partie en cours
  (sinon un fill commun) pendant la mesure suivante, puis retour au groove ; les
  fills tournent à chaque appui ; un fill plus court qu'une mesure finit avec
  elle. Le pont envoie le changement ~10 ms avant la barre (heure tirée de la
  position du transport JACK) ; le cycle de SooperLooper ne change pas.
  Vérifié : fill parti à « mesure 2 temps 4 +99 % », retour à « mesure 3 temps
  4 +99 % ».

### Parties, variations, intro et outro (fait, 2026-11-18)

Modèle à la BeatBuddy / RC-600. Dans DLS Drum :

| Contrôle | Valeurs | Effet |
|---|---|---|
| Intro | Off, Metronome, A, B… | au Play : rien, 1 mesure de clic (pattern « Silence » + métronome), ou l'intro choisie, **une fois** |
| Part | Verse, Pre-Chorus, Chorus, Bridge | partie qui tourne en boucle (absente : la plus proche ; chanson sans parties : « Groove ») |
| Variation | A, B, C… | variation de la partie affichée ; chaque partie retient la sienne |
| Outro | A, B… | fin jouée par End, une fois, puis tout s'arrête |
| Var ◀ ▶ | boutons | variation précédente / suivante, à la mesure suivante |
| Fill | bouton | fill de la partie, puis la phrase reprend (même pattern, ou le suivant si le fill le finissait) |
| Part ◀ ▶ | boutons | partie précédente / suivante : transition de la chanson, sinon un fill de la partie quittée, puis la nouvelle partie |
| End | bouton | outro choisie puis arrêt (pas d'outro : arrêt à la barre) |
| Playing, Var | sorties | partie jouée (Intro, Verse…, Outro, Fill) et variation |

- **Phrase** : les moitiés « Verse (1) », « Verse (2) » (phrase de 8 mesures
  coupée à 4, limite d'Hydrogen) forment **une** variation, jouée d'affilée.
  `.parts.txt` : `part:Verse	4+5,6+7` (variations séparées par `,`, patterns
  enchaînés par `+`). Cycle de SooperLooper = longueur de la phrase.
- Le pont choisit chaque pattern ~10 ms avant son départ, d'après la position
  du transport JACK, et le note dans son journal (« pattern 12 (mesure 3 temps
  4.99, une fois) »).
- Les listes Part et Variation de PiPedal suivent les boutons (websocket).
- **Pack de grooves** (JJ… : intros, grooves, fills, sans Verse ni Chorus) :
  les 4 parties existent quand même et puisent dans les mêmes grooves ; au
  départ Verse = A, Pre-Chorus = B, Chorus = C, Bridge = D. Pour changer :
  Part = Chorus puis Variation = G (ou Part ▶ puis Var ▶ au pied).
- **Mémoire** : la variation de chaque partie de chaque chanson est retenue
  dans `~/dls-batterie/choix.txt` (`chanson<TAB>partie<TAB>lettre`), écrite à
  chaque changement, quel que soit le preset. Au chargement : mémoire, sinon
  la Variation du preset (partie affichée), sinon A (pack : A à D).
- Les fills tournent par liste de fills (pas par partie) : deux parties qui
  partagent les fills communs n'enchaînent pas le même.
- **Variation = None** : la partie est sautée par Part ◀ ▶ (retenu dans la
  mémoire : `-`). La musique ne change pas tant qu'on ne change pas de partie ;
  au Play, une partie affichée à None laisse la place à la suivante. Il reste
  toujours au moins une partie.
- **Part ◀ ▶ reviennent au début** (Bridge -> Verse, Verse -> Bridge), comme
  la BeatBuddy qui revient à la 1re partie après la dernière. Avec Verse et
  Chorus seulement, Part ▶ alterne Verse / Chorus.
- Comparaison : BeatBuddy (appui = fill, appui long = transition puis partie
  suivante, double appui = outro ; la chanson n'a que les parties créées) ;
  RC-600 (variations A à D sans parties, changement à la fin de la mesure ou
  de la boucle : réglage VAR.CHANGE).
- Vérifié (Contemporary S06) : Fill à la barre puis retour ; Part ▶ avec fill
  de transition ; Var ▶ ; End : outro de 3 patterns puis arrêt.
- Limite : un fill au milieu d'un pattern de 4 mesures le fait repartir à sa
  mesure 1 (Hydrogen redémarre un pattern choisi au début) ; la phrase est
  alors décalée jusqu'à la fin de ce pattern.

### Fonctions de batterie (définitions)

Chaque chanson (style) devient une petite chanson à la BeatBuddy, construite à
partir des `.parts.json` (automatique pour les packs étiquetés, devinée pour
ceux qui n'ont que « Groove » et « Fill », modifiable par un petit fichier par
chanson) :

| Partie | Contenu |
|---|---|
| Intro | intro ou décompte |
| Partie 1 (Verse / couplet) | groove(s) du couplet + leurs fills |
| Partie 2 (Chorus / refrain) | groove(s) du refrain + leurs fills |
| Partie 3 (Bridge / pont) | facultative |
| Transition | fill qui fait passer d'une partie à la suivante |
| Outro | la fin |

Les moitiés d'une phrase coupée à 4 mesures (« Verse (1) », « Verse (2) ») sont
enchaînées automatiquement comme un seul groove.

| Fonction | Effet |
|---|---|
| **Groove ◀ ▶** | pattern précédent/suivant de la chanson, tous confondus : pour explorer |
| **Beat ◀ ▶** | beat précédent/suivant du style, en file à travers les groupes (instantané : gros morceau) |
| **Style ◀ ▶** | style précédent/suivant (Metal, Rock…) ; premier beat du style |
| **Partie ◀ ▶** | section précédente/suivante : fill de transition, puis le groove de la partie, au début de la mesure |
| **Variation** | autre version du groove de la partie en cours (Verse A → B → C → A), sans fill ni changement de partie |
| **Fill** | un fill de la partie en cours pendant 1 mesure, puis retour automatique au groove ; les fills de la partie tournent à chaque appui ; compte comme une mesure normale pour les boucles |
| **Fill long** | fill de 2 mesures ou plus (« 2-Bar Fill », fills longs JJ) ; à défaut, deux fills courts ou le fill normal |
| **Batterie on/off** | coupe la batterie (mute d'Hydrogen), boucles et tempo continuent ; elle revient pile sur le 1 |
| **Batterie légère** | coupe grosse caisse et caisse claire (mute par instrument d'Hydrogen), garde les cymbales : montées, passages calmes |
| **Break** | 1 mesure de silence (ou un seul coup sur le 1), puis le groove repart |
| **Crash / accent** | un coup de crash sur le prochain 1 |
| **Intro** | relance l'intro de la chanson |
| **Outro** | joue la fin de la chanson (« Outro », « Ending »…) puis arrête tout (batterie et boucles) ; sans fin prévue : coup final (crash + grosse caisse sur le 1) puis arrêt |

**Moment du changement** : Fill, Partie, Variation, Break et Outro partent **au
début de la mesure suivante** (le fill est toujours complet ; il faut appuyer
une mesure d'avance). Façon BeatBuddy (le fill commence tout de suite et finit
avec la mesure en cours) : non retenue pour l'instant. Réglage prévu pour
l'étape des boucles, comme le VAR.CHANGE de la RC-600 : changer de partie à la
**mesure** ou à la **fin du tour de boucle**.

**Comparaison** : la RC-600 (Parameter Guide p. 7) a 4 variations (A–D), un
fill automatique au changement de variation (réglage FILL on/off, pas de fill
sur demande dans les pages lues), intro et fin en on/off, et le changement de
variation à la fin de la mesure ou de la boucle. Le BeatBuddy : un groove par
partie, fill sur demande, appui tenu = partie suivante, double appui = fin.
Notre système : fill sur demande ET transition, toutes les variations des
packs, un bouton par fonction.

**Ordre de réalisation** : (1) structure des parties de chaque chanson ;
(2) Fill, puis Partie ◀ ▶ et Outro ; (3) Variation, Batterie on/off, Break ;
(4) Style ◀ ▶ au pied ; (5) Fill long, Batterie légère, Crash, Intro ;
(6) boucles par partie et répétitions (§9, à la RC-600) ; puis le routeur MIDI
et les pages du Captain.

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
- Taille des patterns connue pour le morceau ouvert par `batterie CHEMIN`,
  `load`, ou au démarrage : le client **retient le morceau qu'il a ouvert**
  (`travail/morceau-batterie.txt`), car Hydrogen n'écrit son
  `lastSongFilename` qu'en quittant ; à défaut, ce `lastSongFilename`.
- **Mesures de 3 temps** (fait) : groove de 144 ticks = 3 temps, 192 = 4 temps.
  Le client envoie `beats N` à `pb_timebase` (temps par mesure : Beat, voyant 1,
  mesure 1) et règle le cycle de SooperLooper sur la longueur du groove.
  Vérifié avec la valse de la collection RP Bardet : compteur 1-2-3, boucle
  enregistrée juste. Changer le nombre de temps pendant la lecture fait sauter
  la position (la mesure est comptée depuis la trame 0).
- **4/4 swing « tassé »** : certains morceaux (Jazz, Shuffle, Swing, une partie
  de Reggae chez RP Bardet) écrivent une mesure de 4/4 dans 144 ticks (temps à
  0, 36, 72, 108). `scripts/h2_swing44.py` les réécrit en vraies mesures de
  192 ticks (positions × 4/3 : triolets à 16 ticks, comme Hydrogen les écrit).
- **Boucles muettes** (fait) : une boucle muette le reste après Stop puis Play,
  et un Mute fait à l'arrêt est retenu (État « muted »). SooperLooper efface un
  `mute_on` reçu avant le vrai départ de la boucle (vérifié) : le client
  l'envoie juste après le départ du transport et le répète pendant 40 ms. Même
  idée que la RC-600 (piste arrêtée avec LOOP SYNC : elle revient en place).
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
  le client (`SELECT_ONLY_NEXT_PATTERN` : choisit sans démarrer ;
  `SELECT_AND_PLAY_PATTERN` démarre le transport), et `BPM` pour que l'affichage
  d'Hydrogen suive `pb_timebase`.
- **Mode Pattern** : `SONG_MODE_ACTIVATION 0` met Hydrogen en mode Pattern
  (vérifié depuis le mode Song). `session.sh start` l'envoie à chaque démarrage.
- Fill = pattern dédié, joué une mesure, puis retour au groove. Le client envoie
  le changement juste avant la barre.
- **Bibliothèque de rythmes DLS** (`scripts/gen_rythmes.py`) : rythmes écrits
  pour ce projet (grilles de 16 ou 12 pas), un `.h2song` par style, toujours
  la même structure (1 Intro, 2 Verse, 3 Verse fill, 4 Chorus, 5 Chorus fill,
  6 Transition, 7 Ending), pensée pour un futur bouton Fill / passage de
  partie à la BeatBuddy. Styles : Rock, Funk, Shuffle, Punk, Metal.
  - **Kits** : les grilles utilisent des rôles (K, S, H, C…) ; un fichier
    `scripts/kits/*.kit` donne l'instrument de chaque rôle dans un kit, avec
    des remplaçants. Modèle de morceau par kit : fait par Hydrogen
    (`NEW_SONG`, `LOAD_DRUMKIT`, `SAVE_SONG`), dans `~/dls-modeles` sur le Pi.
  - Kits installés dans `~/.hydrogen/data/drumkits` (sans sudo) depuis la
    bibliothèque officielle d'Hydrogen (SourceForge) : AVLDrumkits-BlackPearl-2023
    et AVLDrumkits-RedZeppelin-2023 (CC BY-SA, Glen MacArthur ; 5 nuances de
    frappe), DeathMetal (CC-BY, Archman). Fichiers produits : « DLS Rock »
    (GMRockKit), « DLS Rock - Black Pearl », etc.
  - **Kits de l'ancien format** (DeathMetal, 2008) : Hydrogen 1.2 n'arrive pas
    à les convertir (instruments sans son et en sourdine). `scripts/modele_kit.py`
    fabrique le modèle à partir du `drumkit.xml` ; `--gain 5` pour DeathMetal,
    enregistré trop bas (mesuré : 0,03 au plus fort contre 0,27 pour Black
    Pearl ; 0,16 après).
- **Gros morceau unique (fait, 2026-11-17)** : `scripts/gros_morceau.py` réunit
  toutes les chansons du dossier DLS Drums (et sous-dossiers de style), rendues
  pour un kit, dans `~/dls-batterie/gros/<kit>.h2song` (+ `.index.txt` :
  premier pattern et nombre par chanson). Mesuré : 88 chansons, 2 609
  patterns, 28 Mo, fabriqué en 1,7 s, chargé en moins de 4 s ; Hydrogen passe
  de 515 Mo à 1 Go de mémoire (2,4 Go restent libres), processeur inchangé.
  Le pont : changer de **chanson** = sauter à sa fenêtre de patterns (Groove,
  Previous/Next, Playing comptés dans la chanson), **même en jouant** (vérifié :
  la lecture continue) ; changer de **kit** = charger le gros morceau de ce kit,
  à l'arrêt ; le gros morceau est refait s'il manque ou s'il est plus vieux
  qu'une chanson, que le `.kit` ou que le script.
- **Gros morceau allégé et Hydrogen sans fenêtre (2026-11-17)** : les
  réglages de note égaux aux valeurs par défaut d'Hydrogen sont retirés
  (27 -> 10,6 Mo, 92 380 notes identiques une fois relues). `session.sh`
  lance par défaut `h2cli` (Hydrogen sans fenêtre, même OSC et même JACK) et
  ni slgui ni écran virtuel (`PB_GUI=1` pour les interfaces). Mesuré : gros
  morceau chargé en 2,6 s (6,2 s avant) ; mémoire utilisée 1,2 Go (2,3 Go
  avant). Vérifié en jouant : grille, groove, métronome, boucle SooperLooper
  calée sur le cycle. Après un chargement, Hydrogen reste occupé puis remet le
  premier pattern : le pont attend qu'il soit calme (temps processeur) avant
  de choisir le groove.
- Ancienne note : changer de style sans coupure. Changer de morceau
  (`OPEN_SONG`) recharge tout : la batterie se tait 1 à 1,5 s. Pour l'instant,
  un morceau choisi pendant la lecture (sélecteur Song) est ouvert au prochain
  Stop. Solution prévue : tous les styles de la collection RP Bardet utilisent
  la même batterie (GMRockKit), donc on peut les **réunir dans un seul gros
  morceau** (277 patterns, une plage de patterns par style). Choisir un style =
  sauter à ses patterns, aussi instantané qu'un groove. À prévoir :
  - un script qui fusionne les `.h2song` (mêmes instruments vérifiés, patterns
    renumérotés) et écrit la table style → plage de patterns ;
  - le sélecteur Song garde un fichier par style (repère), le pont le traduit
    en plage ; Groove, Previous et Next restent dans la plage du style ;
  - la liste Groove (1–16) et Playing restent relatives au style ;
  - vérifier qu'Hydrogen garde un morceau de près de 300 patterns sans problème.
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
   **Fait avec DLS Test** (2026-11-15) :
   - titre long (`doap:name` « Davis Loop Station … ») dans la fiche, étiquette
     courte (`mod:label` « DLS … ») dans la chaîne, coupée vers 14 caractères
     (« DLS Test (Stér… ») ; `rdfs:comment` des ports affiché en bulle ;
   - bascule, bouton momentané (`epp:trigger`) et menu : OK ;
   - **liaison MIDI** d'un CC du Captain (CC 6) au bouton momentané : +1 à
     chaque appui ;
   - sorties : voyant (toggled) et voyant clignotant OK ; une sortie avec
     `lv2:enumeration` affiche son **texte** (« vide », « enregistre »…) : à
     utiliser pour l'état du Looper et les compteurs ; une sortie numérique
     simple s'affiche en **barre** (pas de chiffre) ;
   - noms de sortie courts collés les uns aux autres (« VoyantAppuis… ») :
     prendre des noms plus longs ;
   - sélecteur de fichier : voir §5 ;
   - écoute en début de chaîne : son identique, avec ou sans le plugin
     (sortie sur un seul haut-parleur Headrush : les deux côtés du stéréo
     restent à vérifier au casque).
4. **Fichier de contrôle + DLS Looper** (mono et stéréo, audio intégré :
   envoi + retour des boucles sur le lien 1 ; record, overdub, undo, mute,
   boucle, état) ; `pb_timebase` intégré au client.
5. **DLS Drum** (mono et stéréo, retour de la batterie sur le lien 2) :
   tempo, play/stop, début, tap ; tempo sans saut (§8). Branchements JACK :
   SooperLooper → `return1`, Hydrogen → `return2`.
   **Fait** (2026-11-15, §5 « Réalisé ») : groove, précédent/suivant, Playing,
   Play/Stop, Tempo, BPM +/-, Tap, Metronome, mesures de 3 temps, affichage
   par le websocket de PiPedal. Restent : tempo sans saut (§8), fill, choix du
   morceau dans PiPedal (collection, sélecteur de fichier).
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
- PiPedal 2.0.111 : liaison MIDI sur un bouton momentané à maintien
  (`mod:preferMomentaryOffByDefault`, BPM +/-) : le Captain doit envoyer 127 à
  l'appui et 0 au relâchement — à vérifier.
- Hydrogen 1.2.0-beta : faut-il le mettre à jour (version stable 1.2.x
  compilée depuis les sources) ? Commandes vérifiées jusqu'ici : voir §5 et §10.
- SooperLooper : « t. stretch » utilisable au changement de tempo ?
- Changement de preset au relâchement (appui court) : acceptable à l'usage ?
