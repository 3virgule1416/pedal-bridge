# Contrôle au pied : batterie, looper, écran

Document de conception **et de référence** : il décrit ce qui marche aujourd'hui
(à jour au 2026-11-19), comment, et ce qui reste à faire. Les choix sont
expliqués ; les mesures qui les justifient sont gardées.

## 1. But

Jouer de la guitare dans PiPedal et, **sans toucher au PC ni à VNC** :

- lancer et arrêter la batterie (Hydrogen), choisir le style et le beat, passer
  d'une partie à l'autre (couplet, refrain…), déclencher un fill, finir la
  chanson ;
- enregistrer, superposer, annuler des boucles (SooperLooper), et plus tard
  enchaîner des sections ;
- régler le tempo ;
- tout ça au pied (MIDI Captain 10 boutons + double switch de l'Axe I/O One),
  avec un retour visuel (DEL du Captain, voyants dans PiPedal, écran OLED plus
  tard).

Tout ce qui change quelque chose tombe **sur la mesure** (au début de la mesure
suivante, ou du cycle), jamais au moment approximatif de l'appui.

## 2. État

| Pièce | Rôle | État |
|---|---|---|
| DLS Send / DLS Return (LV2) | audio PiPedal ↔ JACK, génériques | fait |
| **DLS Looper** (LV2, mono et stéréo) | envoi au looper + retour des boucles ; boutons du looper | fait (§6) |
| **DLS Drum** (LV2, mono et stéréo) | retour de la batterie ; chanson, parties, variations, tempo | fait (§5) |
| `pedal_bridge` (client JACK) | rééchantillonnage, compensation de latence, **cerveau** : séquenceur de batterie, commandes, websocket PiPedal, **routeur MIDI** du Captain | fait |
| `pb_timebase` | maître du temps JACK ; tempo changé à la barre, sans saut (§8) | fait |
| Hydrogen **1.2.7** (`h2cli`, sans fenêtre) | batterie, pilotée par OSC | fait (compilé, §10) |
| SooperLooper 1.7.8 | boucles, pilotées par OSC | fait |
| Pages du **MIDI Captain** DRUM, BROWSE, PARTS | jouer et régler la batterie au pied | fait (§7) |
| Double switch A/B par le pont | presets (mode PiPedal) ; Play/Stop, End, Part ▶ (mode Drum) ; changer de mode | fait (§7.3) |
| Page LOOPER du Captain, sections | boucles au pied, enchaîner | à faire (§9) |
| Écran OLED | état lisible | à faire (§11) |

**Démarrage du Pi** (services systemd, `scripts/systemd/`, sous le compte
`pedalboard`) :

| Service | Rôle |
|---|---|
| `pb-leds` | DEL de la page PEDAL du Captain (suit PiPedal), toujours, relancé s'il plante |
| `pb-garde` | gardien : pont absent → double switch rendu à PiPedal, Captain sur la page 0 |
| `dls-session` | **fenêtre de choix** (`pb_demarrage`) : la **rangée du bas** du Captain (boutons 6 à 10) clignote en orange 8 s ; un appui sur l'un d'eux = **mode spectacle** (PiPedal seul : ni JACK, ni Hydrogen, ni SooperLooper, ni le pont ; rangée verte 2 s) ; sinon la session complète (`session.sh start`) |

En mode spectacle, le double switch et les DEL de la page PEDAL marchent
comme avec PiPedal seul. À la main : `bash scripts/session.sh start` /
`stop` / `status`, ou `sudo systemctl stop dls-session`. Inventaire de ce qui
est installé : `docs/installations.md`.

## 3. Architecture

```
  MIDI Captain ──── MIDI (page PEDAL, canal 1) ────> PiPedal ──> effets, presets
     │  ▲                                              │
     │  │ SysEx (DEL), CC 20 canal 16 (page)          ├─ DLS Drum   ─┐ /dev/shm/
     │  │                                              └─ DLS Looper ─┤ pedal-bridge-
     │  │                                                             │ control
     └──┼── MIDI (pages DRUM, BROWSE, PARTS, canal 2) ──> pedal_bridge ┘
        │                                                 ├─ séquenceur de batterie (à la mesure)
  Axe I/O One ── MIDI (double switch) ──> pedal_bridge    ├─ OSC -> Hydrogen (patterns), SooperLooper
                                                          ├─ websocket -> PiPedal (affichage)
  pb_timebase : maître du temps JACK ─────────────────────┤ transport JACK
                                                          └─ audio : liens 1 (looper), 2 (batterie)
```

Principes :

1. **Les plugins sont des surfaces de contrôle.** Ils publient les valeurs de
   leurs contrôles et affichent l'état reçu. Aucune logique musicale, aucune
   allocation, aucun verrou, aucun appel système dans `run()`.
2. **Le pont décide et calcule le moment.** Toute commande est exécutée par
   `pedal_bridge` au bon instant de la grille.
3. **Une seule source de tempo** : `pb_timebase`.
4. **Le Captain a une page par mode**, chacune avec ses propres CC : PiPedal
   écoute directement la page PEDAL ; le pont écoute les autres en parallèle
   (§7). Si le pont s'arrête, la page PEDAL marche toujours.

## 4. Communication plugins ↔ pont

Fichier partagé `/dev/shm/pedal-bridge-control` (`common/pb_ctl.h`), créé par
les plugins en 0666, ouvert sans `O_CREAT` par le pont ; un seul écrivain par
champ, atomiques.

- **Commandes (plugins)** : valeurs continues (tempo, listes) ; boutons : **un
  compteur par bouton**, incrémenté à chaque appui (aucun appui perdu ni
  exécuté deux fois). Boutons de DLS Drum 0 à 15 sur une ligne, 16 et plus sur
  une autre (`pb_drum_btn`). Chemins (chanson, kit) protégés par un numéro de
  séquence.
- **État (pont)** : transport, mesure, temps, tempo, état des boucles, partie et
  variation jouées, style et type, groove.
- La **dernière instance activée** d'un plugin commande (changement de preset) ;
  le pont se recale sur ses compteurs sans rien rejouer.
- Les plugins recopient l'état vers leurs **ports de sortie** (voyants, textes).

**Affichage dans PiPedal par son websocket** (`client/pipedal_ws.cc`) : un
plugin LV2 ne peut pas changer ses propres réglages. Le pont remet donc à jour
les listes de DLS Drum (Part, Variation, Intro, Outro, Kit, Auto Fill, Tempo)
et le sélecteur Song (`setPatchProperty`, chemin) comme le fait la page web :
`ws://127.0.0.1:80/pipedal`, `currentPedalboard` (instances par URI), puis
`setControl`. Chaque valeur envoyée est **attendue et ignorée une fois**
(écho du plugin, oublié après 2 s). PiPedal marque alors le preset modifié.
`PB_PIPEDAL_PORT=0` désactive tout ça (banc de test).

## 4 bis. Audio intégré aux plugins de contrôle

| Plugin | Audio | Lien | Où le placer |
|---|---|---|---|
| **DLS Looper** | copie l'entrée vers SooperLooper **puis** ajoute les boucles | lien 1 | **début** de chaîne : boucles sèches qui passent dans les effets (réamping) ; **fin** : boucles enregistrées avec leurs effets |
| **DLS Drum** | ajoute la batterie | lien 2 | **fin** de chaîne : la batterie ne passe dans aucun effet |

- La copie vers le looper se fait **avant** l'ajout des boucles : elles ne
  peuvent pas se réenregistrer.
- Versions **mono et stéréo** (le nombre de canaux d'un LV2 est fixé dans sa
  description). En mono : même son à gauche et à droite vers le looper, retour
  ramené à (G + D) / 2.
- Branchements JACK (`session.sh`) : `pedal_bridge:send1` → SooperLooper →
  `return1` ; Hydrogen → `return2`. La compensation de SooperLooper est
  calculée sur le lien 1.
- Ne pas mettre DLS Send / DLS Return sur le lien 1 en même temps que DLS
  Looper : la dernière instance activée prend le lien.

## 5. DLS Drum

URI `…/drum-mono`, `…/drum-stereo` (« DLS Drum », « DLS Drum St. »). TTL
généré par `scripts/gen_dls_ttl.py` ; code `lv2/drum.c` (plugin) et
`client/controle.cc` (pont).

### 5.1 Contrôles, par section de PiPedal

Les réglages (listes) sont séparés des boutons à assigner aux pédales. PiPedal
ouvre une nouvelle boîte dès que la section change d'un port au suivant : les
ports sont rangés par section.

| Section | Contrôles |
|---|---|
| **Song** | Kit (liste fixe : `scripts/kits/*.kit`), **Song** (sélecteur de fichier), Intro (liste) + **Intro** + **Intro ▶**, Outro (liste) + **End** + **Outro ▶** |
| **Browse** | Style ◀ ▶, Type ◀ ▶, Beat ◀ ▶ ; sorties **Style** et **Type** |
| **Parts** | Part (Verse, Pre-Chorus, Chorus, Bridge), Variation (None, A, B…), Auto Fill ; sorties **Playing**, **Var** |
| **Pedals** | Play / Stop, Part ◀ ▶, Var ◀ ▶, Fill |
| **Tempo** | Tempo (40–240), BPM −, BPM +, Tap, Metronome ; sorties Beat, voyants ♩ 1 ▶, BPM |
| **Bridge** | Volume, Link |

### 5.2 La liste des chansons (Song)

- Les chansons sont dans `/var/pipedal/audio_uploads/DLS Drums/`, rangées en
  **Style / Type / Beat** (`scripts/ranger.py` ; fichiers déplacés, rien
  d'effacé ; `.alias.txt` : le pont retrouve la chanson d'un ancien preset).
  Noms en anglais, sans tempo ; les packs JJ sont coupés par famille, nommés
  par leur allure (slow, medium, fast, very fast, extreme). Ex. :
  `Metal/Straight/Lamb Chops very fast`, `Rock/Straight/Classic S03`.
- Les métadonnées de conversion (`.X.parts.json`) sont **cachées** (point au
  début) : le sélecteur de PiPedal ne les montre pas.
- Le **sélecteur Song** est un sélecteur de fichier de PiPedal : il liste le
  dossier, affiche le nom sans extension, et le choix est gardé dans le preset.
- **Browse** (boutons) : Beat ◀ ▶ dans le type, Type ◀ ▶ dans le style, Style
  ◀ ▶ ; chaque type retient son dernier beat, chaque style son dernier type
  (mémoire, §5.6). En jouant : à la mesure suivante, sans coupure. Le pont
  renvoie le chemin au sélecteur Song (qui montre le Beat) ; les sorties
  **Style** et **Type** montrent le reste.
- Les noms de Style et Type sont **figés** dans la description du plugin
  (PiPedal n'affiche pas de texte libre venant d'un plugin) :
  `scripts/browse.txt` (« Style/Type », ordre du pont), installé avec le plugin.
  Le pont lit la liste **installée** (jamais un mauvais nom ; un dossier pas
  encore installé s'affiche « - »). Au premier choix de chanson après son
  démarrage, il compare aux dossiers ; s'ils ont changé, il refait
  `browse.txt` et `drum.ttl` dans le paquet compilé et écrit dans son journal
  la commande sudo pour les installer.

### 5.3 Le gros morceau (un par kit)

`scripts/gros_morceau.py` réunit toutes les chansons, rendues pour un kit, dans
`~/dls-batterie/gros/<kit>.h2song` (+ `.index.txt` : premier pattern et nombre
par chanson ; `.parts.txt` : structure). Changer de **chanson** = sauter à sa
fenêtre de patterns, **même en jouant**, sans coupure. Changer de **kit** =
ouvrir un autre gros morceau (~2,5 s, à l'arrêt). Refait automatiquement s'il
manque ou s'il est plus vieux qu'une chanson, un `.kit` ou le script.

- XML allégé (réglages de note égaux aux valeurs par défaut retirés) : 3 136
  patterns, ~12 Mo, ouvert en **2,3 s** avec Hydrogen 1.2.7.
- Deux patterns ajoutés : **Silence** (1 mesure vide : décompte, mesure vide de
  la démo) et **Final hit** (grosse caisse + crash du kit sur le 1).
- Après un `OPEN_SONG`, Hydrogen reste occupé puis remet le premier pattern :
  le pont attend qu'il soit calme (temps processeur) avant de choisir.
- Kits : `scripts/kits/*.kit` donne l'instrument de chaque rôle (K, S, H, C,
  C2, CH…) avec des remplaçants ; modèles dans `~/dls-modeles`.

### 5.4 Structure des chansons

`.parts.txt` : pour chaque chanson, `part:<Partie>` (variations), `fill:<Partie>`,
`fill:*`, `intro`, `outro`, `transition`. Une variation est une **phrase** : les
moitiés « Verse (1) », « Verse (2) » d'une phrase de 8 mesures coupée à 4
(limite d'Hydrogen) sont enchaînées (`4+5,6+7`).

Sur 102 chansons : 78 **packs de grooves** (JJ… : intros, grooves, fills), 24 à
**vraies parties**, souvent incomplètes.

- **Pack de grooves** : les 4 parties existent et puisent dans les mêmes
  grooves ; au départ Verse = A, Pre-Chorus = B, Chorus = C, Bridge = D.
- **Parties absentes** d'une chanson à parties (ex. Pop Punk/Modern : Verse
  seulement) : elles existent aussi, à **None au départ**, et puisent dans tous
  les grooves de la chanson ; leur donner une variation en fait une vraie partie.
- La liste Part montre la partie affichée ; une chanson n'a jamais moins
  d'une partie active.

### 5.5 Le séquenceur (dans le pont)

Hydrogen redémarre un pattern choisi à son début : le pont choisit chaque
pattern **~10 ms avant son départ**, d'après la position du transport JACK, et
le note dans son journal (« pattern 12 (mesure 3 temps 4.99, une fois) »).

| Fonction | Effet |
|---|---|
| **Play** | intro (ou décompte), puis la partie affichée en boucle |
| **Intro** (liste) | Off, Metronome (1 mesure de clic), A, B… ; **la liste montre toujours ce qui jouera** : chanson jamais réglée → A si elle a des intros, sinon Off ; lettre sans intro correspondante → corrigée |
| **Part ◀ ▶** | partie précédente / suivante (parties à None sautées, retour au début après la dernière), à la mesure suivante ; **transition** de la chanson, sinon un fill de la partie quittée, si **Auto Fill** est allumé |
| **Var ◀ ▶** | variation précédente / suivante de la partie, à la mesure suivante ; Var ◀ s'arrête à A (None seulement par la liste ou la page PARTS) |
| **Variation = None** | partie sautée par Part ◀ ▶ ; au Play, une partie affichée à None laisse la place à la suivante |
| **Fill** | un fill de la partie (sinon commun), à la mesure suivante, puis la phrase reprend ; les fills tournent par liste |
| **End** | l'outro choisie, puis tout s'arrête ; **sans outro** (86 chansons) : un fill de la partie + **coup final** ; la liste Outro choisit alors le fill (A = 1er, B = 2e…) |
| **Intro** (bouton) | l'intro à la mesure suivante, puis la partie reprend |
| **Intro ▶ / Outro ▶** | choix suivant de la liste, montré dans PiPedal, puis joué |
| Intro, End, Intro ▶, Outro ▶ **à l'arrêt** | jouent seuls (batterie seule, boucles en pause), pour choisir |
| **Playing / Var** | ce qui joue vraiment : Intro, partie, Outro, Fill, « - » (mesure vide) |

- **Changer de chanson en jouant** : la nouvelle part à la mesure suivante.
- **Cycle de SooperLooper** = longueur de la phrase.
- **Limite** : un fill au milieu d'un pattern de 4 mesures le fait repartir à
  sa mesure 1 ; la phrase est alors décalée jusqu'à la fin de ce pattern.

**Tempo et métronome** : Tempo appliqué quand le bouton ne bouge plus depuis
0,3 s ; BPM ± : ±1, tenus : répétition (0,4 s, plus vite après 2 s), regroupés
(10 tempos par seconde au plus) ; Tap : au moins 3 coups, tempo changé une fois
à la fin, moyenne des 8 derniers. Métronome : l'OSC d'Hydrogen n'a qu'une
bascule ; le pont suit le retour d'état et ne bascule que si l'état diffère.

### 5.6 Mémoire par chanson

`~/dls-batterie/choix.txt`, écrite à chaque changement, **prioritaire sur le
preset** :

- la variation de chaque partie (`chanson<TAB>Verse<TAB>A`, `-` = None) ;
- l'Intro et l'Outro (`…Intro<TAB>Metronome`, `…Outro<TAB>B`), remontrés dans
  PiPedal au changement de chanson ; pendant 1,5 s après, une valeur venue du
  preset ne les remplace pas ;
- Browse : dernier beat de chaque type, dernier type de chaque style.

Dans le preset seulement : Kit, Song, Tempo, Metronome, Auto Fill, Volume.
Copie sur le PC : `bash scripts/sauvegarde.sh` (voir `docs/installations.md`).

### 5.7 Comparaison

BeatBuddy : appui = fill, appui long = transition puis partie suivante, double
appui = outro, retour à la 1re partie après la dernière ; la chanson n'a que les
parties créées. RC-600 : variations A à D sans parties, fill automatique au
changement de variation, changement à la fin de la mesure ou de la boucle
(VAR.CHANGE). Ici : fill sur demande et transition, toutes les variations des
packs, un bouton par fonction, changement à la mesure.

## 6. DLS Looper

URI `…/looper-mono`, `…/looper-stereo` (« DLS Looper », « DLS Looper St. »).

| Section | Contrôles |
|---|---|
| Loop | Boucle (1–8), Record, Overdub, Undo, Redo, Mute, Effacer ; sortie State (offline, empty, waiting, recording, overdub, playing, muted, paused) |
| Transport | Play / Stop (le même que la batterie), Tempo ; sorties Beat, ♩, 1, ▶, BPM |
| Bridge | Volume des boucles, Link |

- Exécution dans le pont : `/sl/N/hit` record, overdub, undo, redo, mute,
  undo_all sur la boucle choisie ; Play / Stop global (§8 bis).
- **Record ne part que si le transport roule** (SooperLooper suit le
  transport). Avec la batterie arrêtée, un deuxième appui sur Record ne désarme
  pas une boucle en attente (à corriger avec la page LOOPER).
- Mesuré à travers le plugin (`pb_lag`, période 128) : boucle vs batterie
  −10,6 ms = latence propre de PiPedal (entrée + sortie de l'Axe), identique
  après Stop/Play : le trajet du plugin et du pont est exact. Test au
  médiator : écart moyen −6,4 ms, sans dérive.
- Bancs : `scripts/test-looper.sh` (JACK séparé), `test_looper` hors ligne.
- À explorer dans SooperLooper : `multiply`, `insert`, `reverse`, `once`,
  `rate`/`stretch`, quantification du mute, pistes synchronisées.

## 6 bis. Jouer avec ou sans batterie

- Le **tempo et la grille** viennent de `pb_timebase`, pas d'Hydrogen :
  SooperLooper se cale sans aucune batterie. La batterie n'arrive dans PiPedal
  **que par DLS Drum** : preset sans DLS Drum = pas de batterie (Hydrogen reste
  lancé, silencieux). Repère sans batterie : métronome d'Hydrogen.
- **Mode tempo** (principal) : tempo choisi avant d'enregistrer (réglage, tap) ;
  4/4 ou 3/4 (selon le groove) ; tout est quantifié.
- **Mode libre** (option, à faire) : la 1re boucle fixe la longueur ; le pont en
  déduit le tempo (nombre de mesures choisi d'avance, comme MEASURE de la
  RC-600) et place le 1 au début de la boucle.

## 7. MIDI Captain

### 7.1 Ce qu'on sait du Captain (firmware officiel 5.0, « super mode »)

- Port MIDI « Feather RP2040 » sur le Pi. Configuration par fichiers
  `supersetup/pageN.txt` sur sa mémoire USB (copie : `E:/DWL/Pedalboard/supersetup/` ;
  mode d'emploi : `E:/DWL/MIDICaptainSTD_FW5.zip`).
- Par touche : `short_dw` (appui), `short_up` (relâchement), `long` / `long_up`
  (appui long, `long_press_timing`), `ledcolor0/1` (3 DEL), `keytimes`
  (bascule) ; `page_name` : 6 caractères au plus.
- **Commande externe, canal 16** : CC 20 = aller à la page N ; CC 1 à 10 = appui
  virtuel ; CC 21 à 30 = état d'une touche à bascule. **DEL** par SysEx
  `F0 00 04 10 <touche 1-10> <R G B> ×3 F7` (RVB 8 bits divisés par 2) ; les 3 DEL
  d'une touche se règlent séparément (vérifié).
- L'écran montre le nom de la page, le nom du dernier bouton appuyé, la
  dernière commande ; **le Pi ne peut pas y écrire de texte libre**.
- Le Captain remet la DEL d'une touche appuyée à la couleur de sa page : le
  pont **repeint** la page après chaque appui et relâchement.

### 7.2 Architecture : une page par mode

| Page | Nom | Messages | Qui écoute |
|---|---|---|---|
| 0 | `PEDAL` | CC1 à CC10, canal 1, appui seulement | **PiPedal** (liaisons du musicien) |
| 1 | `DRUM` | CC21 à CC30, canal 2, 127 / 0 | pont |
| 2 | `BROWSE` | CC31 à CC40, canal 2 | pont |
| 3 | `PARTS` | CC41 à CC50, canal 2 | pont |
| 4 | `LOOP` (à faire) | CC51 à CC60, canal 2 | pont |

- Pages générées par `scripts/gen_captain.py` (`captain/pageN.txt`, fins de
  ligne Windows), à partir de l'en-tête de la page 0 du musicien ; à copier sur
  le Captain. Rangée du haut : key1 à key5 ; du bas : keyA à keyE.
- **Aucune couleur dans les pages** : le pont peint les DEL ; la table des
  couleurs ne vit que dans le pont.
- Le pont (`client/midi.cc`, ALSA séquenceur, client « pedal_bridge ») s'abonne
  au Captain et à l'Axe **en parallèle de PiPedal** et les reprend s'ils sont
  rebranchés. Il mesure la durée des appuis : une touche sans action longue agit
  **dès l'appui** ; une touche avec action longue fait l'action courte **au
  relâchement** et la longue **dès 0,5 s** de maintien.
- Commande du pont `captain page N` (aussi utilisée par les touches → Browse…).

### 7.3 Double switch (A, B)

Axe I/O One : A = CC 65, B = CC 67, canal 1, 127 à l'appui et 0 au
relâchement. Le pont le traite **selon le mode** :

| Mode | A court | A long | B court | B long |
|---|---|---|---|---|
| **PiPedal** (page PEDAL) | preset précédent | — | preset suivant | changer de mode |
| **Drum** (DRUM, BROWSE, PARTS) | Play / Stop | End | Part ▶ | changer de mode |

- En mode PiPedal, le pont **renvoie à PiPedal les mêmes CC** que l'Axe (65,
  67 : liaisons « Previous / Next Preset », « Trigger on rising edge » de
  PiPedal, inchangées), au relâchement (~0,2 s plus tard qu'en direct, pour
  reconnaître l'appui long de B).
- A en mode Drum : **à l'arrêt, Play part dès l'appui** ; en jouant, court =
  Stop au relâchement, long = End.
- Changer de mode : PiPedal ↔ Drum (le Captain passe sur PEDAL ou DRUM).
  Le Captain ne dit jamais sur quelle page il est : au démarrage, le pont le
  met sur PEDAL ; ensuite, la page se déduit aussi des boutons appuyés (CC 1 à
  10 canal 1 = PEDAL ; canal 2 = DRUM, BROWSE ou PARTS).
- **Sans toucher aux réglages de PiPedal** : la liaison Axe → PiPedal est une
  connexion MIDI ALSA que PiPedal fait lui-même ; le pont la **débranche** en
  démarrant (et de nouveau si PiPedal la refait) et la **rebranche** en
  s'arrêtant, après avoir remis le Captain sur la **page 0**. `PB_AXE=0` :
  le pont laisse le double switch à PiPedal.
- **Gardien `pb_garde`** (lancé par `session.sh`) : si le pont plante ou est
  tué, il rebranche l'Axe à PiPedal et remet le Captain sur la page 0 dans la
  seconde. Pont qui ne démarre pas : rien n'est débranché. Donc, **sans pont,
  on retombe toujours en mode PiPedal normal**. Vérifié : arrêt normal et
  `kill -9`.

En plus (provisoire, inutile avec B) : sur DRUM, → Browse **tenu** ramène à
PEDAL ; sur BROWSE et PARTS, la dernière touche **tenue** ramène à DRUM.

### 7.4 Page DRUM (jouer)

Rangée du **bas** : ce qu'on touche en jouant ; du **haut** : entre deux
morceaux.

| | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| **Haut** | Tap *(long : Metronome)* | BPM − *(tenu : continu)* | BPM + *(tenu : continu)* | → Browse | → Parts |
| **Bas** | Fill *(long : Crash)* | Var ◀ *(long : Batterie légère)* | Var ▶ *(long : Fill long)* | Part ◀ *(long : Intro)* | Batterie on/off *(long : Break)* |

- Mêmes actions que les boutons de DLS Drum (fonction `drum_button` commune).
- Tap compte le coup dès l'appui ; tenu, ce coup est annulé et le métronome
  bascule (Tap vert quand il est allumé).
- Pas de démo sur cette page : Part ◀ ▶ ne parcourent que les parties, End va
  à la fin, Play démarre par l'intro.
- **Pas encore faits** : Crash, Batterie légère, Fill long, Batterie on/off,
  Break (§7.7).
- Couleurs : Tap blanc (vert : métronome), BPM jaune, → pages gris, Fill
  orange, Var cyan, Part vert, Batterie vert foncé.

### 7.5 Page BROWSE (choisir, écouter la chanson)

| | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| **Haut** | Style ◀ *(long : Kit ◀)* | Style ▶ *(long : Kit ▶)* | Type ◀ | Type ▶ | Metronome |
| **Bas** | Beat ◀ | Beat ▶ | Part ◀ | Part ▶ | → Parts *(long : → DRUM)* |

- **Kit ◀ ▶** (Style tenus) : change la liste Kit dans PiPedal. En jouant :
  Stop, chargement du gros morceau du kit (~2,5 s), puis reprise sur la même
  partie et variation, sans intro, **à la mesure 1** (pour rester calé avec les
  boucles). À l'arrêt : chargement tout de suite.
- **Metronome** : DEL verte allumé, rouge éteint.
- **Mode démo** (en jouant) : la chanson se déroule en boucle comme une vraie :
  Intro, puis **deux fois** [Verse ×2, Pre-Chorus, Chorus ×2, Bridge], puis
  Outro (sinon fill + coup final), **une mesure sans batterie**, et on
  recommence. Parties à None sautées ; fill entre deux parties si Auto Fill ;
  l'intro suit la liste Intro (Metronome = une mesure de clic à chaque tour).
  Repart à l'intro : Play depuis BROWSE, arrivée sur BROWSE en jouant, nouveau
  beat. **Part ◀ ▶** (et ceux de DLS Drum pendant la démo) : étape précédente /
  suivante de **toute la chanson**, Intro et Outro compris. En quittant BROWSE,
  la démo s'arrête, la partie en cours continue.
- Couleurs par paire : Style jaune, Type orange, Beat cyan, Part vert.

### 7.6 Page PARTS (régler intro, parties, outro : audition)

| | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| **Haut** | Intro | Verse | Pre-Chorus | Chorus | Bridge |
| **Bas** | Back | Next | Outro | AutoFill | → Browse *(long : → DRUM)* |

- Appuyer sur Intro, une partie ou Outro la **sélectionne** et la fait
  **boucler** (l'Intro et l'Outro aussi). **À l'arrêt**, choisir ne fait que
  sélectionner ; **Play** la lance, batterie seule. **En jouant** : à la mesure
  suivante.
- **Back / Next** changent la valeur de la sélection (retenue par chanson) :
  Intro (Off, Metronome, A…), partie (None, A…), Outro (A…, ou fins fill + coup
  final).
- **None / Off en jouant** : mesures vides (le transport continue) ; le
  prochain choix repart à la mesure.
- En arrivant sur PARTS en jouant, la partie qui joue est sélectionnée ; en
  quittant pendant que l'intro, l'outro ou le décompte boucle, retour à la
  partie à la mesure suivante.
- **DEL** : une couleur par partie (Intro bleu, Verse vert, Pre-Chorus
  turquoise, Chorus orange, Bridge violet, Outro magenta), vive si
  sélectionnée, **5 %** sinon ; None / Off : deux DEL de la couleur de la
  partie et **celle du milieu rouge** ; Back / Next : couleur de la
  sélection ; AutoFill vert / rouge.

### 7.7 Fonctions encore à faire

| Fonction | Effet prévu |
|---|---|
| **Batterie on/off** | coupe la batterie (mute d'Hydrogen) ; boucles et tempo continuent ; revient pile sur le 1 |
| **Batterie légère** | coupe grosse caisse et caisse claire, garde les cymbales (montées, passages calmes) ; bascule |
| **Break** | une mesure de silence (ou un coup sur le 1), puis le groove repart |
| **Crash** | un coup de crash sur le prochain 1 |
| **Fill long** | fill de 2 mesures ou plus ; à défaut, deux fills courts |
| **Page LOOPER** | boutons du looper au pied, état des boucles en couleur |

### 7.8 DEL de la page PEDAL : `pb_leds`

Programme à part (`client/pb_leds.cc`, service `pb-leds`), pour que les DEL de
PiPedal marchent **même sans le pont**. Il suit PiPedal par son websocket
(`hello`, `currentPedalboard`, `getPresets`, puis `onPedalboardChanged`,
`onItemEnabledChanged`, `onControlChanged`, `onSelectedSnapshotChanged`,
`onPresetsChanged`) ; règles reprises de `captain-manager/pipedal_leds.py` :

- touche N = CC N (1 à 10) lié dans PiPedal : **bypass d'un effet** (allumé si
  l'effet est actif) ou **interrupteur d'un effet** (allumé s'il est à 1) ;
- **couleur de l'effet** = sa couleur d'icône dans PiPedal (clic sur le nom de
  l'effet), sinon vert ; allumé : 3 DEL vives, éteint : 1 DEL à 5 % ;
- snapshots 1 à 5 sur les touches 6 à 10 (devant), dans leur couleur,
  vifs si sélectionnés ;
- **éclair blanc** de toutes les touches au chargement d'un preset ;
- reconnexion à PiPedal (3 s) et repeinture si le Captain est rebranché ; il
  écoute aussi les boutons de la page PEDAL (le Captain remet en noir la DEL
  appuyée) et repeint.

**Un seul responsable des DEL** : le pont publie la page du Captain
(`cap_page` dans `/dev/shm/pedal-bridge-control`) ; `pb_leds` ne peint que sur
la page PEDAL, ou si le pont ne donne plus signe de vie (`client_heartbeat`
figé). Pendant la fenêtre de démarrage, il laisse la rangée du bas à
`pb_demarrage`. `pipedal_leds.py` reste dans `captain-manager` comme référence.

## 8. Tempo sans saut

SooperLooper place sa grille depuis la trame 0 avec le tempo actuel. Un tempo
changé en cours de lecture est donc appliqué **au début de la mesure suivante**,
et à cet instant `pb_timebase` **repositionne le transport** à la trame de cette
mesure au nouveau tempo (`trame = mesure × temps × trames par temps`). La mesure
ne saute pas, la grille de SooperLooper reste alignée. Vérifié : « mesure 5
temps 4 » à 160 BPM → « mesure 6 temps 1 » à 200 BPM ; `sl-sync-test.sh`
(100 → 120 → 100 puis record) : départ pile au 1, décalage −2,8 ms comme sans
changement.

Hydrogen 1.2.0-beta accumulait des notes après un changement de tempo en
lecture (son doublé, CPU ×4, jusqu'au Stop : bug Hydrogen #1953, corrigé en
1.2.4). **Réglé avec Hydrogen 1.2.7** (§10).

Les boucles déjà enregistrées ne s'étirent pas (« t. stretch » de SooperLooper
à tester).

## 8 bis. Stop, Play et chansons sauvegardées

| Situation (`pb_lag`) | Boucle vs batterie |
|---|---|
| Lecture continue après l'enregistrement | −22,67 ms (compensation appliquée) |
| Relance par `trigger` | 0 ms : **compensation perdue** |
| Pause / reprise | garde la compensation, mais +3 à 4 ms à chaque pause |
| **Audio de la boucle décalé de D trames, puis `trigger`** | **−22,67 ms, identique** |

SooperLooper compense en décalant la lecture, et `trigger` l'oublie. Le pont
décale donc **l'audio de la boucle elle-même** (normalisation) :

- **stop** : transport arrêté, boucles en pause, puis normalisation des
  boucles neuves (`save_loop`, rotation de D = latence envoi + retour au moment
  de l'enregistrement, `load_loop`). Refusé pendant un enregistrement.
- **play** : stop, transport à la trame 0, `trigger` de toutes les boucles,
  départ. Toujours depuis la mesure 1.
- **save NOM / load NOM** : `~/pedal-bridge/chansons/NOM/` (boucles
  normalisées, tempo, `NOM.h2song`).
- Seul un enregistrement neuf rend une boucle « à normaliser ». Une boucle
  normalisée est définitive (vérifié même après changement de période JACK).
  État gardé dans `~/pedal-bridge/travail/etat.txt`.
- **Boucles muettes** : gardées muettes après Stop puis Play ; un Mute fait à
  l'arrêt est retenu.
- **Mesures de 3 temps** : groove de 144 ticks = 3 temps ; le pont envoie
  `beats N` à `pb_timebase`. Les 4/4 swing « tassés » dans 144 ticks sont
  réécrits par `scripts/h2_swing44.py`.

Limites : ne pas changer la période JACK pendant que SooperLooper tourne (il
plante ; `session.sh` refuse) ; un undo qui annule une normalisation n'est pas
détecté.

## 9. Sections (à faire)

SooperLooper n'a pas de mode chanson ; le pont en fera un, calé sur la mesure :
une **section** = quelles boucles jouent + une longueur en cycles ; une
**chanson** = une liste de sections (`A×4, B×4, A×4`). Au pied : section
suivante au prochain cycle, ou enchaînement automatique. Idée : lier les
sections aux parties de la batterie (une boucle pour le couplet, une pour le
refrain).

## 10. Hydrogen

- **Version 1.2.7**, compilée dans `~/hydrogen-1.2.7` (Debian bookworm n'a que
  la 1.2.0-beta). `session.sh` l'utilise s'il existe (`PB_H2=/usr` : celle de
  Debian). Lancé sans fenêtre (`h2cli`) ; `PB_GUI=1` pour les interfaces.
- En écoute du transport JACK (J.Trans), jamais maître : il suit le tempo de
  `pb_timebase`.
- **OSC** (`oscEnabled` dans `~/.hydrogen/hydrogen.conf`), port 9000 : on
  utilise `OPEN_SONG`, `SELECT_ONLY_NEXT_PATTERN` (choisit sans démarrer ;
  sélectionner le numéro déjà choisi est ignoré : forcer 1 puis 0 après un
  chargement), `SONG_MODE_ACTIVATION 0` (mode Pattern), `TOGGLE_METRONOME`,
  `SAVE_SONG_AS`. Retour d'état : table de mixage, métronome ; **pas** le
  pattern ni le tempo (le pont les retient).
- **Bibliothèque DLS** (`scripts/gen_rythmes.py`) : rythmes écrits pour ce
  projet, toujours la même structure (Intro, Verse, Verse fill, Chorus, Chorus
  fill, Transition, Ending). Conversion des packs MIDI : `scripts/midi2h2.py`.
- Kits dans `~/.hydrogen/data/drumkits` : AVL Black Pearl, AVL Red Zeppelin
  (CC BY-SA, Glen MacArthur), DeathMetal (CC-BY, Archman ; ancien format :
  `scripts/modele_kit.py`, `--gain 5`), plus GMRockKit d'Hydrogen.

## 11. Écran OLED (à faire)

Waveshare 2,42 po, SSD1309, 128 × 64, en SPI.

| Module | Rôle | Pi (broche / GPIO) |
|---|---|---|
| VCC | 3,3 V | 1 |
| GND | masse | 6 |
| DIN | MOSI | 19 / GPIO 10 |
| CLK | horloge | 23 / GPIO 11 |
| CS | sélection | 24 / GPIO 8 (CE0) |
| DC | données/commande | 22 / GPIO 25 |
| RST | reset | 13 / GPIO 27 |

- À activer : `dtparam=spi=on` dans `/boot/firmware/config.txt` puis
  redémarrage (commande sudo, avec accord). `pedalboard` est déjà dans les
  groupes `spi` et `gpio`.
- Programme `pb_display` (C, `spidev` + `libgpiod`) : il lit l'état du pont et
  affiche ce que PiPedal et le Captain ne peuvent pas : **noms du Style, du
  Type et du Beat**, partie et variation, tempo, mesure et temps, état des
  boucles. Mise en page d'abord en images PNG sur le PC.

## 11 bis. Fenêtre de démarrage, hotspot et carte verrouillée (décidé 2026-11-20, à faire)

Trois options **indépendantes**, choisies au Captain dans la fenêtre de 8 s au
démarrage du Pi (`pb_demarrage`, service `dls-session`) :

| Option | Effet | Dure |
|---|---|---|
| **PiPedal seul** | sans batterie ni looper (ni JACK, Hydrogen, SooperLooper, pont) | ce démarrage |
| **Hotspot Wi-Fi** | active le hotspot (désactivé par défaut) | ce démarrage |
| **Carte verrouillée** (read-only) | rien n'est écrit sur la carte SD : **on peut couper le courant** sans risque | jusqu'à ce qu'on la rebascule (demande un redémarrage) |

Combinaisons utiles : session complète + carte normale (répétition) ; session
complète + verrouillée (spectacle avec batterie et looper) ; PiPedal seul +
verrouillée (spectacle le plus sûr).

**Page START du Captain** (page 5, canal 2, rien n'y est lié dans PiPedal) :
pendant la fenêtre, le Captain y est : les appuis ne déclenchent rien dans
PiPedal, et l'écran affiche START. À la fin, retour sur la page PEDAL (le pont
ou le gardien la remettent de toute façon).

| Bouton | Pendant la fenêtre | Appui |
|---|---|---|
| Rangée du bas (6 à 10) | clignote en orange | **PiPedal seul** : clignotement arrêté, vert fixe |
| Bouton 5 (haut droite) | clignote en bleu | **hotspot** : clignotement arrêté, bleu fixe |
| Bouton 1 (haut gauche) | fixe : **rouge** = carte verrouillée, **blanc** = normale | bascule rouge ↔ blanc |

- Chaque option est une **bascule** : un deuxième appui annule (le bouton
  reprend son état de départ : clignotement, ou sa couleur d'avant).
- À la fin des 8 s, on applique ce qui est choisi à ce moment-là ; si l'état
  de la carte a changé, le Pi bascule le verrouillage et **redémarre**.
- **Carte verrouillée** : système de fichiers en surcouche de Raspberry Pi OS
  (`raspi-config`, overlay) ; toutes les écritures vont en mémoire et
  disparaissent à l'extinction. Rien de ce qui change pendant la soirée n'est
  gardé (presets modifiés, `choix.txt`, boucles sauvegardées, chansons
  ajoutées). Basculer demande root : petit service root dédié, avec accord.
- Avant de verrouiller : commande **« préparer »** qui fabrique ou met à jour
  les gros morceaux de **tous les kits** (sinon un kit périmé serait refait en
  mémoire à chaque démarrage : quelques secondes et du processeur au mauvais
  moment) ; lancée aussi automatiquement avant le redémarrage qui verrouille.
- Hotspot : voir d'abord si PiPedal le gère déjà (réglages Wi-Fi de son
  interface) et passer par lui. Avec la carte verrouillée, le « désactivé par
  défaut » vient tout seul.
- À vérifier : PiPedal démarre bien en read-only ; journal du pont et fichiers
  temporaires déjà en mémoire (`/tmp`, `/dev/shm`).

## 12. Suite

1. Vérifier au prochain redémarrage du Pi : services, fenêtre de la rangée du bas.
2. **Fenêtre de démarrage complète** (§11 bis) : page START, hotspot, carte
   verrouillée, commande « préparer ».
3. **Fonctions de batterie restantes** (§7.7) : Batterie on/off, Break,
   Crash, Batterie légère, Fill long.
4. **Page LOOPER** du Captain.
5. **Sections** (§9).
6. **Écran OLED** (§11).
7. Mode libre (§6 bis).

## 13. Questions ouvertes

- Changement de preset au relâchement (appui court de la page PEDAL ou du
  double switch) : acceptable à l'usage ?
- SooperLooper : « t. stretch » utilisable au changement de tempo ?
