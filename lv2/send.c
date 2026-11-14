/*
 * Plugin LV2 « Bridge Send » (stéréo).
 *
 * L'audio passe tel quel (pass-through) ET est copié dans le tampon partagé
 * du lien choisi, quand un lecteur (client JACK) est présent.
 *
 * Temps réel : les 8 liens sont mappés à l'instanciation. run() ne fait que
 * des atomiques, du calcul et des copies mémoire.
 */
#include "plugin.h"
#include "pb_shm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
    P_LINK, P_GAIN, P_CONNECTED,
    P_COUNT
};

typedef struct {
    const float *in[2];
    float       *out[2];
    const float *p_link;
    const float *p_gain;
    float       *p_connected;

    pb_link  links[PB_MAX_LINKS];
    uint64_t me;
    uint32_t rate;

    int      cur;        /* index du lien courant 0..7, -1 aucun */
    int      need_take;  /* prendre la place d'écrivain au prochain run() */
    pb_watch reader;     /* présence du lecteur */

    float    gain_db;    /* dernière valeur lue du port */
    float    gain_target;
    float    gain_cur;
} Send;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f)
{
    (void)d; (void)bundle; (void)f;
    Send *s = (Send *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rate = (uint32_t)(rate + 0.5);
    s->me = pb_new_owner_id();
    s->cur = -1;
    s->gain_db = 0.0f;
    s->gain_target = s->gain_cur = 1.0f;
    pb_watch_init(&s->reader, s->rate / 4u);

    /* Créer/ouvrir les 8 liens maintenant (hors temps réel). Un lien qui
     * échoue reste vide : le plugin fera seulement du pass-through dessus. */
    for (uint32_t i = 0; i < PB_MAX_LINKS; i++) {
        char err[256];
        if (pb_link_open(&s->links[i], i + 1u, PB_DIR_SEND, 1, err, sizeof err) != 0)
            pb_log_error("pedal-bridge send: %s\n", err);
    }
    return s;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Send *s = (Send *)h;
    switch (port) {
    case P_IN_L:      s->in[0] = (const float *)data; break;
    case P_IN_R:      s->in[1] = (const float *)data; break;
    case P_OUT_L:     s->out[0] = (float *)data; break;
    case P_OUT_R:     s->out[1] = (float *)data; break;
    case P_LINK:      s->p_link = (const float *)data; break;
    case P_GAIN:      s->p_gain = (const float *)data; break;
    case P_CONNECTED: s->p_connected = (float *)data; break;
    default: break;
    }
}

static void activate(LV2_Handle h)
{
    Send *s = (Send *)h;
    s->need_take = 1;
    s->gain_cur = s->gain_target;
    pb_watch_init(&s->reader, s->rate / 4u);
}

static inline float db_to_gain(float db)
{
    return db <= -60.0f ? 0.0f : powf(10.0f, db * 0.05f);
}

static void run(LV2_Handle h, uint32_t n)
{
    Send *s = (Send *)h;

    /* --- Lien choisi --- */
    int want = s->p_link ? (int)lrintf(*s->p_link) : 1;
    if (want < 1) want = 1;
    if (want > (int)PB_MAX_LINKS) want = (int)PB_MAX_LINKS;
    want -= 1;
    if (want != s->cur || s->need_take) {
        if (s->cur >= 0 && want != s->cur && pb_link_ok(&s->links[s->cur]))
            pb_writer_release(&s->links[s->cur], s->me);
        s->cur = want;
        s->need_take = 0;
        if (pb_link_ok(&s->links[want]))
            pb_writer_take(&s->links[want], s->me, s->rate);
        pb_watch_init(&s->reader, s->rate / 4u);
    }

    /* --- Gain (lissé sur le bloc) --- */
    float db = s->p_gain ? *s->p_gain : 0.0f;
    if (db != s->gain_db) {
        s->gain_db = db;
        s->gain_target = db_to_gain(db);
    }

    /* --- Copie vers le tampon partagé --- */
    int connected = 0;
    pb_link *l = &s->links[s->cur];
    if (n > 0 && pb_link_ok(l) && pb_writer_claim(l, s->me)) {
        pb_watch_update(&s->reader, PB_LOAD_ACQ(&l->hdr->reader_heartbeat), n);
        connected = s->reader.alive;
        if (connected) {
            pb_span sp;
            uint32_t got = pb_write_reserve(l, n, &sp);
            const float *inl = s->in[0], *inr = s->in[1];
            float g = s->gain_cur;
            const float dg = (s->gain_target - s->gain_cur) / (float)n;
            uint32_t i = 0;
            for (uint32_t k = 0; k < sp.n1; k++, i++, g += dg) {
                sp.p1[2 * k]     = inl[i] * g;
                sp.p1[2 * k + 1] = inr[i] * g;
            }
            for (uint32_t k = 0; k < sp.n2; k++, i++, g += dg) {
                sp.p2[2 * k]     = inl[i] * g;
                sp.p2[2 * k + 1] = inr[i] * g;
            }
            pb_write_commit(l, got);
            if (got < n) pb_count_overrun(l, n - got);
        }
        pb_writer_beat(l, n);
    }
    s->gain_cur = s->gain_target;

    /* --- Pass-through (après la copie : les tampons peuvent être les mêmes) --- */
    for (int c = 0; c < 2; c++)
        if (s->out[c] != s->in[c])
            memmove(s->out[c], s->in[c], n * sizeof(float));

    if (s->p_connected) *s->p_connected = connected ? 1.0f : 0.0f;
}

static void deactivate(LV2_Handle h)
{
    Send *s = (Send *)h;
    if (s->cur >= 0 && pb_link_ok(&s->links[s->cur]))
        pb_writer_release(&s->links[s->cur], s->me);
    if (s->p_connected) *s->p_connected = 0.0f;
}

static void cleanup(LV2_Handle h)
{
    Send *s = (Send *)h;
    for (uint32_t i = 0; i < PB_MAX_LINKS; i++) {
        if (pb_link_ok(&s->links[i])) pb_writer_release(&s->links[i], s->me);
        pb_link_close(&s->links[i]);
    }
    free(s);
}

static const void *extension_data(const char *uri) { (void)uri; return NULL; }

const LV2_Descriptor pb_send_descriptor = {
    PB_URI_SEND, instantiate, connect_port, activate, run, deactivate,
    cleanup, extension_data
};
