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
  MIDI Captain 10 ─┐                                    ┌─> écran OLED (SPI)
  Axe I/O One  ────┼─ MIDI ─> PiPedal                   │      pb_display
  (double switch)  │           ├─ liaisons MIDI ─> Bridge Batterie (LV2)
                   │           ├─ liaisons MIDI ─> Bridge Looper   (LV2)
                   │           │        │  commandes / état
                   │           │        ▼  /dev/shm/pedal-bridge-control
                   │           │   pedal_bridge (le « cerveau »)
                   │           │     ├─ maître du temps JACK (pb_timebase intégré)
                   │           │     ├─ transport JACK (play/stop) ─> Hydrogen suit
                   │           │     ├─ OSC ─> SooperLooper
                   │           │     ├─ OSC ─> Hydrogen (patterns, fills)
                   │           │     ├─ séquenceur de sections, calé sur la mesure
                   │           │     └─ MIDI ─> Captain (DEL)          [à vérifier]
                   │           └─ audio : Send/Return (existe)
```

Principes :

1. **Les plugins sont des surfaces de contrôle.** Ils ne font que publier les
   valeurs de leurs contrôles et afficher l'état reçu. Aucune logique musicale,
   aucun réseau dans `run()`.
2. **Le client décide et calcule le moment.** Toute commande est exécutée par
   `pedal_bridge` au bon instant de la grille : début de mesure, de cycle ou de
   section.
3. **Une seule source de tempo** : le maître du temps intégré au client.
4. **Les liaisons MIDI restent dans PiPedal** (son interface habituelle). On ne
   lit pas le MIDI nous-mêmes, sauf si l'appui long l'exige (§7).

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
| Play / Stop | bouton | démarre ou arrête le transport JACK ; Hydrogen suit |
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
| Tempo | molette | **le même tempo** que la batterie (une seule valeur, deux endroits) |

Sorties : **État de la boucle** (vide, attente, enregistrement, lecture, muet),
**Temps** et **Sur le 1** (clignotant au tempo, couleur différente sur le 1),
**Section**.

À explorer dans SooperLooper : `multiply`, `insert`, `reverse`, `once` (jouer une
fois), `trigger`, `rate`/`stretch` (« t. stretch » : la boucle suit le tempo ?),
`sync`/`play sync`, la quantification du mute (`mute_quantized`), et les pistes
synchronisées entre elles (« sync to loop »).

## 7. MIDI Captain et modes

**Trois modes** : **PiPedal** (presets, effets), **Batterie**, **Looper**. Les 10
boutons du Captain changent de rôle selon le mode.

Où vit le mode ? Trois possibilités, à choisir après avoir lu la configuration du
firmware officiel du Captain :

| Option | Comment | Pour | Contre |
|---|---|---|---|
| A. Pages du Captain | le Captain change de page (bouton dédié ou appui long sur un de ses boutons) ; chaque page envoie d'autres CC | simple, pas de logique côté Pi | il faut que le firmware officiel gère pages et appui long |
| B. Appui long sur le double switch | le client écoute aussi l'Axe I/O One et mesure la durée d'appui | ton idée de départ | PiPedal change de preset **dès l'appui** (CC 65/67) : le changement de mode déclencherait aussi un changement de preset |
| C. Mode dans le client | le Captain envoie toujours les mêmes CC ; le client les interprète selon le mode | indépendant du firmware | PiPedal reçoit aussi ces CC : risque de doublons avec ses propres liaisons |

**Proposition** : l'option **A** si le firmware la permet. Pour B, il faudrait
que PiPedal change de preset au relâchement, ou déplacer preset +/− sur le
Captain.

Retour sur le Captain : le client peut envoyer du MIDI au Captain (il est visible
sur le Pi comme « Feather RP2040 ») pour colorer les DEL selon l'état (rouge =
enregistrement, vert = lecture, clignotement au tempo). **À vérifier** : le
firmware officiel accepte-t-il du MIDI entrant pour piloter ses DEL ?

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
2. **Lire la configuration du Captain** (firmware officiel) : pages, appui long,
   DEL par MIDI. Choisir l'option de mode (§7).
3. **Vérifier PiPedal** avec un plugin minimal : rendu des boutons momentanés
   (`lv2:trigger` ou équivalent), des voyants de sortie, liaison d'un CC à un
   bouton.
4. **Fichier de contrôle + Bridge Looper** (record, overdub, undo, mute, boucle,
   état) ; `pb_timebase` intégré au client.
5. **Bridge Batterie** : tempo, play/stop, début, tap ; tempo sans saut (§8).
6. **Hydrogen OSC** : grooves et fills (après accord pour `hydrogen.conf`).
7. **Sections** (§9).
8. **Écran** : rendu PNG, puis vrai écran après activation du SPI.
9. **DEL du Captain** (si le firmware le permet).

## 13. Questions ouvertes

- Firmware officiel du Captain : pages, appui long, MIDI entrant pour les DEL ?
- PiPedal 2.0.111 : boutons momentanés, voyants de sortie, liaison MIDI sur un
  bouton momentané (appui ou relâchement) ?
- Hydrogen 1.2.0-beta : quelles commandes OSC ? Faut-il le mettre à jour (version
  stable 1.2.x compilée depuis les sources) ?
- SooperLooper : « t. stretch » utilisable au changement de tempo ?
- Appui long sur le double switch : garde-t-on l'idée (§7, option B) ?
