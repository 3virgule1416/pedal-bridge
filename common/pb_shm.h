/*
 * pedal-bridge : tampon circulaire partagé dans /dev/shm.
 *
 * Un fichier par lien et par sens :
 *   /dev/shm/pedal-bridge-L<n>-send    (plugin Send   -> client JACK)
 *   /dev/shm/pedal-bridge-L<n>-return  (client JACK   -> plugin Return)
 *
 * File SPSC sans verrou : un seul écrivain, un seul lecteur.
 * write_idx et read_idx ne font que monter (uint64, jamais remis à zéro).
 * Remplissage = write_idx - read_idx.
 *
 * Seuls les plugins (compte pipedal_d) créent les fichiers. Le client et les
 * outils ouvrent seulement des fichiers existants (fs.protected_regular=2 et
 * RemoveIPC de systemd).
 *
 * Les fonctions pb_link_* marquées "RT" ne font ni allocation, ni verrou, ni
 * appel système : seulement des atomiques et de la copie mémoire.
 *
 * Utilisable en C et en C++ (atomiques __atomic de GCC sur des entiers ordinaires).
 */
#ifndef PB_SHM_H
#define PB_SHM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_MAGIC        0x47524250u /* "PBRG" en mémoire little-endian */
#define PB_VERSION      1u
#define PB_MAX_LINKS    8u
#define PB_CHANNELS     2u
#define PB_CAPACITY     8192u       /* trames, puissance de 2 */
#define PB_HEADER_BYTES 256u

enum pb_dir { PB_DIR_SEND = 0, PB_DIR_RETURN = 1 };

typedef struct pb_header {
    /* Ligne 0 : constantes, écrites une fois à la création. magic en dernier. */
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t channels;
    uint32_t capacity;
    uint32_t link;          /* 1..8 */
    uint32_t direction;     /* pb_dir */
    uint32_t creator_uid;
    uint8_t  pad0[32];

    /* Ligne 1 : appartient à l'écrivain. */
    uint64_t write_idx;
    uint64_t overruns;      /* trames perdues : tampon plein */
    uint64_t writer_owner;  /* identifiant de l'instance qui écrit (0 = libre) */
    uint32_t writer_heartbeat;
    uint32_t writer_rate;   /* fréquence d'échantillonnage de l'écrivain */
    uint32_t writer_block;  /* taille du dernier bloc écrit */
    uint32_t writer_target; /* remplissage visé par l'écrivain (trames, 0 = inconnu) */
    uint8_t  pad1[24];

    /* Ligne 2 : appartient au lecteur. */
    uint64_t read_idx;
    uint64_t underruns;     /* trames manquantes : tampon vide */
    uint64_t reader_owner;
    uint32_t reader_heartbeat;
    uint32_t reader_rate;
    uint32_t reader_block;
    uint8_t  pad2[28];

    /* Ligne 3 : réservée. */
    uint8_t  pad3[64];
} pb_header;

#ifdef __cplusplus
static_assert(sizeof(pb_header) == PB_HEADER_BYTES, "pb_header");
static_assert(offsetof(pb_header, write_idx) == 64, "ligne ecrivain");
static_assert(offsetof(pb_header, read_idx) == 128, "ligne lecteur");
#else
_Static_assert(sizeof(pb_header) == PB_HEADER_BYTES, "pb_header");
_Static_assert(offsetof(pb_header, write_idx) == 64, "ligne ecrivain");
_Static_assert(offsetof(pb_header, read_idx) == 128, "ligne lecteur");
#endif

#define PB_FILE_BYTES (PB_HEADER_BYTES + (size_t)PB_CAPACITY * PB_CHANNELS * sizeof(float))

/* Un lien mappé en mémoire. */
typedef struct pb_link {
    pb_header *hdr;
    float     *data;   /* PB_CAPACITY trames entrelacées G/D */
    uint32_t   mask;
    uint32_t   link;
    uint32_t   direction;
} pb_link;

/* Deux segments contigus (le tampon fait le tour). */
typedef struct pb_span {
    float   *p1; uint32_t n1;
    float   *p2; uint32_t n2;
} pb_span;

/* ---------------------------------------------------------------------------
 * Hors temps réel : ouverture / fermeture.
 * ------------------------------------------------------------------------- */

/* Dossier des fichiers : $PB_SHM_DIR ou /dev/shm. */
const char *pb_shm_dir(void);

/* Chemin du fichier d'un lien. Retourne 0 si OK. */
int pb_link_path(char *buf, size_t len, uint32_t link, int direction);

/* Ouvre (et crée si create != 0) le fichier, le mappe et le précharge en RAM.
 * Retourne 0 si OK, sinon -errno (et le message dans err si non NULL). */
int pb_link_open(pb_link *l, uint32_t link, int direction, int create,
                 char *err, size_t errlen);

void pb_link_close(pb_link *l);

/* Identifiant unique d'une instance (pid + compteur). Jamais 0. */
uint64_t pb_new_owner_id(void);

/* ---------------------------------------------------------------------------
 * Atomiques (RT).
 * ------------------------------------------------------------------------- */
#define PB_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define PB_LOAD_RLX(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define PB_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define PB_STORE_RLX(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)

static inline int pb_link_ok(const pb_link *l) { return l && l->hdr; }

/* ---- Côté écrivain (RT) ---- */

/* Prend la main comme écrivain (à l'activation : la dernière instance gagne). */
static inline void pb_writer_take(pb_link *l, uint64_t me, uint32_t rate)
{
    PB_STORE_RLX(&l->hdr->writer_rate, rate);
    PB_STORE_REL(&l->hdr->writer_owner, me);
}

/* Réclame la place si elle est libre. Retourne 1 si on est l'écrivain. */
static inline int pb_writer_claim(pb_link *l, uint64_t me)
{
    uint64_t cur = PB_LOAD_ACQ(&l->hdr->writer_owner);
    if (cur == me) return 1;
    if (cur != 0) return 0;
    return __atomic_compare_exchange_n(&l->hdr->writer_owner, &cur, me, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline void pb_writer_release(pb_link *l, uint64_t me)
{
    uint64_t cur = me;
    __atomic_compare_exchange_n(&l->hdr->writer_owner, &cur, (uint64_t)0, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* L'écrivain annonce le remplissage qu'il vise : le lecteur s'en sert pour
 * savoir quand commencer à jouer et combien garder quand il se recale. */
static inline void pb_writer_set_target(pb_link *l, uint32_t frames)
{
    PB_STORE_RLX(&l->hdr->writer_target, frames);
}

static inline void pb_writer_beat(pb_link *l, uint32_t block)
{
    PB_STORE_RLX(&l->hdr->writer_block, block);
    PB_STORE_REL(&l->hdr->writer_heartbeat, PB_LOAD_RLX(&l->hdr->writer_heartbeat) + 1u);
}

/* Réserve jusqu'à 'want' trames. Retourne le nombre accordé. */
static inline uint32_t pb_write_reserve(pb_link *l, uint32_t want, pb_span *s)
{
    uint64_t w = PB_LOAD_RLX(&l->hdr->write_idx);
    uint64_t r = PB_LOAD_ACQ(&l->hdr->read_idx);
    uint64_t fill = w - r;
    uint32_t space = fill >= PB_CAPACITY ? 0u : (uint32_t)(PB_CAPACITY - fill);
    uint32_t n = want < space ? want : space;
    uint32_t pos = (uint32_t)w & l->mask;
    uint32_t first = PB_CAPACITY - pos;
    if (first > n) first = n;
    s->p1 = l->data + (size_t)pos * PB_CHANNELS; s->n1 = first;
    s->p2 = l->data;                              s->n2 = n - first;
    return n;
}

static inline void pb_write_commit(pb_link *l, uint32_t n)
{
    PB_STORE_REL(&l->hdr->write_idx, PB_LOAD_RLX(&l->hdr->write_idx) + n);
}

static inline void pb_count_overrun(pb_link *l, uint32_t n)
{
    PB_STORE_RLX(&l->hdr->overruns, PB_LOAD_RLX(&l->hdr->overruns) + n);
}

/* ---- Côté lecteur (RT) ---- */

static inline void pb_reader_take(pb_link *l, uint64_t me, uint32_t rate)
{
    PB_STORE_RLX(&l->hdr->reader_rate, rate);
    PB_STORE_REL(&l->hdr->reader_owner, me);
}

static inline int pb_reader_claim(pb_link *l, uint64_t me)
{
    uint64_t cur = PB_LOAD_ACQ(&l->hdr->reader_owner);
    if (cur == me) return 1;
    if (cur != 0) return 0;
    return __atomic_compare_exchange_n(&l->hdr->reader_owner, &cur, me, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline void pb_reader_release(pb_link *l, uint64_t me)
{
    uint64_t cur = me;
    __atomic_compare_exchange_n(&l->hdr->reader_owner, &cur, (uint64_t)0, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static inline void pb_reader_beat(pb_link *l, uint32_t block)
{
    PB_STORE_RLX(&l->hdr->reader_block, block);
    PB_STORE_REL(&l->hdr->reader_heartbeat, PB_LOAD_RLX(&l->hdr->reader_heartbeat) + 1u);
}

/* Trames disponibles à lire. */
static inline uint32_t pb_read_available(const pb_link *l)
{
    uint64_t w = PB_LOAD_ACQ(&l->hdr->write_idx);
    uint64_t r = PB_LOAD_RLX(&l->hdr->read_idx);
    uint64_t fill = w - r;
    return fill > PB_CAPACITY ? PB_CAPACITY : (uint32_t)fill;
}

static inline uint32_t pb_read_reserve(pb_link *l, uint32_t want, pb_span *s)
{
    uint32_t avail = pb_read_available(l);
    uint32_t n = want < avail ? want : avail;
    uint32_t pos = (uint32_t)PB_LOAD_RLX(&l->hdr->read_idx) & l->mask;
    uint32_t first = PB_CAPACITY - pos;
    if (first > n) first = n;
    s->p1 = l->data + (size_t)pos * PB_CHANNELS; s->n1 = first;
    s->p2 = l->data;                              s->n2 = n - first;
    return n;
}

static inline void pb_read_commit(pb_link *l, uint32_t n)
{
    PB_STORE_REL(&l->hdr->read_idx, PB_LOAD_RLX(&l->hdr->read_idx) + n);
}

/* Le lecteur se recale : on jette le surplus pour garder au plus 'keep' trames. */
static inline void pb_reader_resync(pb_link *l, uint32_t keep)
{
    uint64_t w = PB_LOAD_ACQ(&l->hdr->write_idx);
    uint64_t r = PB_LOAD_RLX(&l->hdr->read_idx);
    if (w - r > keep) PB_STORE_REL(&l->hdr->read_idx, w - keep);
}

static inline void pb_count_underrun(pb_link *l, uint32_t n)
{
    PB_STORE_RLX(&l->hdr->underruns, PB_LOAD_RLX(&l->hdr->underruns) + n);
}

/* ---------------------------------------------------------------------------
 * Détection du correspondant (RT) : son heartbeat doit bouger.
 * À appeler une fois par bloc avec le heartbeat de l'autre côté.
 * ------------------------------------------------------------------------- */
typedef struct pb_watch {
    uint32_t last;
    uint32_t stale_frames;  /* trames écoulées depuis le dernier changement */
    uint32_t timeout;       /* trames avant de déclarer absent */
    int      alive;
    int      primed;        /* 0 : première lecture, on note seulement la valeur */
} pb_watch;

static inline void pb_watch_init(pb_watch *w, uint32_t timeout_frames)
{
    w->last = 0; w->stale_frames = timeout_frames; w->timeout = timeout_frames;
    w->alive = 0; w->primed = 0;
}

/* Retourne 1 si le correspondant vient d'apparaître (transition absent -> présent).
 * Un heartbeat figé laissé par un processus mort ne compte pas : il faut qu'il bouge. */
static inline int pb_watch_update(pb_watch *w, uint32_t beat, uint32_t frames)
{
    int was = w->alive;
    if (!w->primed) {
        w->primed = 1;
        w->last = beat;
    } else if (beat != w->last) {
        w->last = beat;
        w->stale_frames = 0;
    } else if (w->stale_frames < w->timeout) {
        w->stale_frames += frames;
    }
    w->alive = w->stale_frames < w->timeout;
    return w->alive && !was;
}

#ifdef __cplusplus
}
#endif
#endif /* PB_SHM_H */
