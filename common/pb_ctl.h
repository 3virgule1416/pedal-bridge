/*
 * pedal-bridge : fichier de contrôle partagé /dev/shm/pedal-bridge-control
 * (plugins DLS Looper / DLS Drum <-> client pedal_bridge).
 * Chaque plugin a sa section (propriétaire, réglages, compteurs de boutons).
 *
 * Même principe que les tampons audio (pb_shm.h) :
 *   - créé en 0666 par les plugins seulement (compte pipedal_d) ; le client
 *     l'ouvre sans O_CREAT (fs.protected_regular=2, RemoveIPC) ;
 *   - un seul écrivain par champ, accès atomiques, aucun verrou.
 *
 * Commandes (écrites par le plugin propriétaire) :
 *   - boutons : un compteur par bouton, +1 à chaque appui. Le client compare au
 *     dernier compteur vu : aucun appui perdu, aucun exécuté deux fois ;
 *   - valeurs (boucle choisie, tempo) : le client agit quand elles changent.
 * Pas de float : les atomiques de GCC ne prennent que des entiers.
 * Plusieurs instances (changement de preset) : la dernière activée prend la
 * place (looper_owner) ; les autres n'écrivent plus rien. Quand le
 * propriétaire change, le client se recale sur ses compteurs sans rien exécuter.
 *
 * État (écrit par le client) : transport et mesure (callback JACK), état des
 * boucles de SooperLooper (thread de contrôle).
 */
#ifndef PB_CTL_H
#define PB_CTL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_CTL_MAGIC    0x54434250u /* "PBCT" en mémoire little-endian */
#define PB_CTL_VERSION  1u
#define PB_CTL_BYTES    4096u
#define PB_CTL_PATH     512u          /* chemin du morceau (DLS Drum) */
#define PB_CTL_LOOPS    8u
#define PB_CTL_BUTTONS  16u

/* Boutons du Looper (indices de looper_btn). */
enum pb_btn {
    PB_BTN_RECORD = 0,
    PB_BTN_OVERDUB,
    PB_BTN_UNDO,
    PB_BTN_REDO,
    PB_BTN_MUTE,
    PB_BTN_CLEAR,
    PB_BTN_PLAYSTOP,
    PB_BTN_COUNT
};

/* Boutons de DLS Drum (indices de drum_btn). */
enum pb_drum_btn {
    PB_DBTN_PLAYSTOP = 0,
    PB_DBTN_PREV,       /* groove précédent */
    PB_DBTN_NEXT,       /* groove suivant */
    PB_DBTN_TAP,        /* tap tempo */
    PB_DBTN_BPM_UP,     /* BPM + : +1 par pas ; tenu, le plugin répète (de plus en plus vite) */
    PB_DBTN_BPM_DOWN,   /* BPM - */
    PB_DBTN_FILL,       /* fill de la partie en cours, à la mesure suivante */
    PB_DBTN_PART_PREV,  /* partie précédente (fill de transition) */
    PB_DBTN_PART_NEXT,  /* partie suivante (fill de transition) */
    PB_DBTN_OUTRO,      /* fin de la chanson, puis arrêt */
    PB_DBTN_COUNT
};

#define PB_CTL_GROOVES  160u    /* grooves du menu et affichables (patterns d'Hydrogen) */

/* État d'une boucle, tel qu'affiché par le plugin (port « État »). */
enum pb_loop_state {
    PB_LS_OFFLINE = 0,  /* client ou SooperLooper absent */
    PB_LS_EMPTY,
    PB_LS_WAIT,         /* attente du début ou de la fin du cycle */
    PB_LS_RECORD,
    PB_LS_OVERDUB,
    PB_LS_PLAY,
    PB_LS_MUTE,
    PB_LS_PAUSE,
    PB_LS_OTHER,        /* multiply, insert, replace... */
    PB_LS_COUNT
};

typedef struct pb_ctl {
    /* Ligne 0 : constantes. magic publié en dernier. */
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;
    uint32_t creator_uid;
    uint8_t  pad0[48];

    /* Ligne 1 : plugin DLS Looper propriétaire. */
    uint64_t looper_owner;      /* instance qui écrit (0 = aucune) */
    uint32_t looper_heartbeat;
    uint32_t looper_loop;       /* boucle choisie, 1..8 */
    uint32_t looper_tempo;      /* réglage Tempo, en centièmes de BPM */
    uint8_t  pad1[44];

    /* Ligne 2 : compteurs des boutons du Looper. */
    uint32_t looper_btn[PB_CTL_BUTTONS];

    /* Ligne 3 : client, callback JACK (transport). */
    uint64_t client_owner;
    uint32_t client_heartbeat;  /* +1 par période JACK */
    uint32_t rolling;           /* transport en marche */
    uint32_t bar;               /* mesure, à partir de 1 (0 = inconnue) */
    uint32_t beat;              /* temps dans la mesure, à partir de 1 */
    uint32_t beats_per_bar;
    uint32_t beat_frac;         /* position dans le temps, 0..65535 */
    uint32_t bpm;               /* tempo, en centièmes de BPM (0 = inconnu) */
    uint8_t  pad3[28];

    /* Ligne 4 : client, thread de contrôle (SooperLooper). */
    uint32_t sl_ok;             /* SooperLooper répond */
    uint32_t loop_count;
    uint8_t  loop_state[PB_CTL_LOOPS];  /* pb_loop_state */
    uint8_t  pad4[48];

    /* Ligne 5 : plugin DLS Drum propriétaire. */
    uint64_t drum_owner;
    uint32_t drum_heartbeat;
    uint32_t drum_groove;       /* menu Groove, 1..16 */
    uint32_t drum_tempo;        /* réglage Tempo, en centièmes de BPM */
    uint32_t drum_metro;        /* interrupteur Metronome : 0 éteint, 1 allumé */
    uint32_t drum_kit_num;      /* liste Kit : 1..N (ordre alphabétique des .kit) ; 0 = pas choisi */
    uint32_t drum_part;         /* liste Part : 1 Verse, 2 Pre-Chorus, 3 Chorus, 4 Bridge ; 0 = pas choisi */
    uint32_t drum_var;          /* liste Variation : 0 None (partie sautée), 1.. A, B, C… */
    uint32_t drum_intro;        /* liste Intro : 1 Off, 2 Metronome, 3.. = A, B… ; 0 = pas choisi */
    uint32_t drum_outro;        /* liste Outro : 1.. = A, B… */
    uint8_t  pad5[20];

    /* Ligne 6 : compteurs des boutons de DLS Drum. */
    uint32_t drum_btn[PB_CTL_BUTTONS];

    /* Ligne 7 : client, état de la batterie. */
    uint32_t h2_ok;             /* l'OSC d'Hydrogen écoute */
    uint32_t groove_cur;        /* groove joué, à partir de 1 (0 = inconnu) */
    uint32_t groove_count;      /* patterns du morceau (0 = inconnu) */
    uint32_t part_cur;          /* partie jouée : 0 -, 1 Intro, 2 Verse, 3 Pre-Chorus, 4 Chorus, 5 Bridge, 6 Groove, 7 Outro, 8 Fill */
    uint32_t var_cur;           /* variation jouée, à partir de 1 (0 = -) */
    uint8_t  pad7[44];

    /* Ligne 8 et suivantes : morceau Hydrogen choisi dans DLS Drum (sélecteur
     * de fichier). Verrou de séquence : le plugin met drum_song_seq impair,
     * copie le chemin, puis le remet pair ; le client relit si le numéro a
     * changé pendant sa copie. Chemin vide = aucun morceau choisi. */
    uint32_t drum_song_seq;
    uint8_t  pad8[60];
    char     drum_song[PB_CTL_PATH];

    /* Kit de batterie choisi dans DLS Drum (fichier .kit), même verrou. */
    uint32_t drum_kit_seq;
    uint8_t  pad9[60];
    char     drum_kit[PB_CTL_PATH];

    /* Réservé. Un fichier créé par une version plus ancienne a des zéros ici :
     * les nouveaux champs ne font qu'ajouter, la version reste 1. */
    uint8_t  reserved[PB_CTL_BYTES - 10 * 64 - 2 * PB_CTL_PATH];
} pb_ctl;

#ifdef __cplusplus
static_assert(sizeof(pb_ctl) == PB_CTL_BYTES, "pb_ctl");
static_assert(offsetof(pb_ctl, looper_owner) == 64, "ligne looper");
static_assert(offsetof(pb_ctl, looper_btn) == 128, "ligne boutons");
static_assert(offsetof(pb_ctl, client_owner) == 192, "ligne client");
static_assert(offsetof(pb_ctl, sl_ok) == 256, "ligne sooperlooper");
static_assert(offsetof(pb_ctl, drum_owner) == 320, "ligne drum");
static_assert(offsetof(pb_ctl, drum_btn) == 384, "ligne boutons drum");
static_assert(offsetof(pb_ctl, h2_ok) == 448, "ligne hydrogen");
static_assert(offsetof(pb_ctl, drum_song_seq) == 512 && offsetof(pb_ctl, drum_song) == 576, "ligne morceau");
static_assert(offsetof(pb_ctl, drum_kit_seq) == 1088 && offsetof(pb_ctl, drum_kit) == 1152, "ligne kit");
#else
_Static_assert(sizeof(pb_ctl) == PB_CTL_BYTES, "pb_ctl");
_Static_assert(offsetof(pb_ctl, looper_owner) == 64, "ligne looper");
_Static_assert(offsetof(pb_ctl, looper_btn) == 128, "ligne boutons");
_Static_assert(offsetof(pb_ctl, client_owner) == 192, "ligne client");
_Static_assert(offsetof(pb_ctl, sl_ok) == 256, "ligne sooperlooper");
_Static_assert(offsetof(pb_ctl, drum_owner) == 320, "ligne drum");
_Static_assert(offsetof(pb_ctl, drum_btn) == 384, "ligne boutons drum");
_Static_assert(offsetof(pb_ctl, h2_ok) == 448, "ligne hydrogen");
_Static_assert(offsetof(pb_ctl, drum_song_seq) == 512 && offsetof(pb_ctl, drum_song) == 576, "ligne morceau");
_Static_assert(offsetof(pb_ctl, drum_kit_seq) == 1088 && offsetof(pb_ctl, drum_kit) == 1152, "ligne kit");
#endif

/* Hors temps réel. Chemin : $PB_SHM_DIR (ou /dev/shm)/pedal-bridge-control.
 * Ouvre (et crée si create != 0), mappe et verrouille en RAM.
 * Retourne 0 si OK, sinon -errno (message dans err si non NULL). */
int  pb_ctl_open(pb_ctl **out, int create, char *err, size_t errlen);
void pb_ctl_close(pb_ctl *c);

#ifdef __cplusplus
}
#endif
#endif /* PB_CTL_H */
