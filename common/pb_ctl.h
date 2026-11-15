/*
 * pedal-bridge : fichier de contrôle partagé /dev/shm/pedal-bridge-control
 * (plugins DLS Looper / DLS Drum <-> client pedal_bridge).
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

    /* Réservé (DLS Drum...). */
    uint8_t  reserved[PB_CTL_BYTES - 5 * 64];
} pb_ctl;

#ifdef __cplusplus
static_assert(sizeof(pb_ctl) == PB_CTL_BYTES, "pb_ctl");
static_assert(offsetof(pb_ctl, looper_owner) == 64, "ligne looper");
static_assert(offsetof(pb_ctl, looper_btn) == 128, "ligne boutons");
static_assert(offsetof(pb_ctl, client_owner) == 192, "ligne client");
static_assert(offsetof(pb_ctl, sl_ok) == 256, "ligne sooperlooper");
#else
_Static_assert(sizeof(pb_ctl) == PB_CTL_BYTES, "pb_ctl");
_Static_assert(offsetof(pb_ctl, looper_owner) == 64, "ligne looper");
_Static_assert(offsetof(pb_ctl, looper_btn) == 128, "ligne boutons");
_Static_assert(offsetof(pb_ctl, client_owner) == 192, "ligne client");
_Static_assert(offsetof(pb_ctl, sl_ok) == 256, "ligne sooperlooper");
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
