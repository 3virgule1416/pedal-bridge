/*
 * Plugin LV2 « Bridge Return » (stéréo).
 *
 * Lit le tampon partagé du lien choisi (écrit par le client JACK) et
 *   - mode Add     : l'ajoute au signal qui traverse le plugin ;
 *   - mode Replace : le met à la place du signal.
 * Sans écrivain : Add laisse passer le signal tel quel, Replace sort du silence.
 *
 * Machine à états :
 *   ATTENTE : on ne joue rien tant que le tampon n'a pas assez de réserve
 *             (cible annoncée par l'écrivain). Le surplus (vieux son) est jeté.
 *   LECTURE : on lit n trames par bloc. Fondu d'entrée au premier bloc.
 *             Tampon vide : fondu de sortie sur ce qui reste, retour en ATTENTE.
 *             Si rien ne reste, la dernière valeur jouée descend en ligne droite
 *             jusqu'à zéro sur le reste du bloc (pas de marche = pas de clic).
 *
 * Temps réel : les 8 liens sont mappés à l'instanciation ; run() ne fait que
 * des atomiques, du calcul et des copies.
 */
#include "plugin.h"
#include "pb_shm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
    P_LINK, P_MODE, P_GAIN, P_CONNECTED,
    P_COUNT
};

enum { MODE_ADD = 0, MODE_REPLACE = 1 };
enum { ST_WAIT = 0, ST_PLAY = 1 };

#define DEFAULT_TARGET 256u   /* réserve si l'écrivain n'annonce rien */

typedef struct {
    const float *in[2];
    float       *out[2];
    const float *p_link;
    const float *p_mode;
    const float *p_gain;
    float       *p_connected;

    pb_link  links[PB_MAX_LINKS];
    uint64_t me;
    uint32_t rate;

    int      cur;
    int      need_take;
    int      state;
    pb_watch writer;

    float    dry_cur;      /* 1 = signal d'entrée gardé (Add), 0 = remplacé */
    float    gain_db;
    float    gain_target;
    float    gain_cur;
    float    env;          /* enveloppe du retour : 0..1 */
    float    last[2];      /* dernière valeur du retour envoyée en sortie */
} Return;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f)
{
    (void)d; (void)bundle; (void)f;
    Return *r = (Return *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->rate = (uint32_t)(rate + 0.5);
    r->me = pb_new_owner_id();
    r->cur = -1;
    r->dry_cur = 1.0f;
    r->gain_target = r->gain_cur = 1.0f;
    pb_watch_init(&r->writer, r->rate / 4u);

    for (uint32_t i = 0; i < PB_MAX_LINKS; i++) {
        char err[256];
        if (pb_link_open(&r->links[i], i + 1u, PB_DIR_RETURN, 1, err, sizeof err) != 0)
            pb_log_error("pedal-bridge return: %s\n", err);
    }
    return r;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Return *r = (Return *)h;
    switch (port) {
    case P_IN_L:      r->in[0] = (const float *)data; break;
    case P_IN_R:      r->in[1] = (const float *)data; break;
    case P_OUT_L:     r->out[0] = (float *)data; break;
    case P_OUT_R:     r->out[1] = (float *)data; break;
    case P_LINK:      r->p_link = (const float *)data; break;
    case P_MODE:      r->p_mode = (const float *)data; break;
    case P_GAIN:      r->p_gain = (const float *)data; break;
    case P_CONNECTED: r->p_connected = (float *)data; break;
    default: break;
    }
}

static void activate(LV2_Handle h)
{
    Return *r = (Return *)h;
    r->need_take = 1;
    r->state = ST_WAIT;
    r->env = 0.0f;
    r->gain_cur = r->gain_target;
    int mode = r->p_mode ? (int)lrintf(*r->p_mode) : MODE_ADD;
    r->dry_cur = mode == MODE_REPLACE ? 0.0f : 1.0f;
    pb_watch_init(&r->writer, r->rate / 4u);
}

static inline float db_to_gain(float db)
{
    return db <= -60.0f ? 0.0f : powf(10.0f, db * 0.05f);
}

/* Mixe un segment du tampon partagé dans la sortie.
 * Les rampes (dry, wet = gain*env) avancent d'un pas par trame. */
static inline void mix_span(Return *r, const float *src, uint32_t m, uint32_t *i,
                            float *dry, float ddry, float *wet, float dwet)
{
    const float *inl = r->in[0], *inr = r->in[1];
    float *outl = r->out[0], *outr = r->out[1];
    for (uint32_t k = 0; k < m; k++, (*i)++) {
        uint32_t j = *i;
        float a = inl[j], b = inr[j];          /* lire avant d'écrire (en place) */
        float wl = src[2 * k] * *wet, wr = src[2 * k + 1] * *wet;
        outl[j] = a * *dry + wl;
        outr[j] = b * *dry + wr;
        r->last[0] = wl; r->last[1] = wr;
        *dry += ddry; *wet += dwet;
    }
}

static void run(LV2_Handle h, uint32_t n)
{
    Return *r = (Return *)h;
    if (n == 0) return;

    /* --- Lien choisi --- */
    int want = r->p_link ? (int)lrintf(*r->p_link) : 1;
    if (want < 1) want = 1;
    if (want > (int)PB_MAX_LINKS) want = (int)PB_MAX_LINKS;
    want -= 1;
    int tail = 0;   /* le retour s'arrête dans ce bloc : adoucir la fin */
    if (want != r->cur || r->need_take) {
        if (r->state == ST_PLAY) tail = 1;
        if (r->cur >= 0 && want != r->cur && pb_link_ok(&r->links[r->cur]))
            pb_reader_release(&r->links[r->cur], r->me);
        r->cur = want;
        r->need_take = 0;
        if (pb_link_ok(&r->links[want]))
            pb_reader_take(&r->links[want], r->me, r->rate);
        pb_watch_init(&r->writer, r->rate / 4u);
        r->state = ST_WAIT;
        r->env = 0.0f;
    }

    /* --- Contrôles --- */
    int mode = r->p_mode ? (int)lrintf(*r->p_mode) : MODE_ADD;
    const float dry_target = mode == MODE_REPLACE ? 0.0f : 1.0f;
    float db = r->p_gain ? *r->p_gain : 0.0f;
    if (db != r->gain_db) { r->gain_db = db; r->gain_target = db_to_gain(db); }

    const float fn = (float)n;
    float dry = r->dry_cur;
    const float ddry = (dry_target - r->dry_cur) / fn;

    /* --- Lecture du tampon --- */
    pb_link *l = &r->links[r->cur];
    uint32_t got = 0;
    pb_span sp = { 0, 0, 0, 0 };
    float env_end = 0.0f;
    int connected = 0;

    if (pb_link_ok(l) && pb_reader_claim(l, r->me)) {
        pb_watch_update(&r->writer, PB_LOAD_ACQ(&l->hdr->writer_heartbeat), n);
        connected = r->writer.alive;

        uint32_t target = PB_LOAD_RLX(&l->hdr->writer_target);
        if (target == 0) target = DEFAULT_TARGET;
        if (target > PB_CAPACITY / 2) target = PB_CAPACITY / 2;
        uint32_t start_at = target > n ? target : n;

        if (r->state == ST_WAIT && connected && pb_read_available(l) >= start_at) {
            pb_reader_resync(l, start_at);   /* jeter le vieux son en trop */
            r->state = ST_PLAY;
            r->env = 0.0f;                   /* fondu d'entrée */
        }
        if (r->state == ST_PLAY) {
            got = pb_read_reserve(l, n, &sp);
            if (got == n) {
                env_end = 1.0f;
            } else {
                /* Tampon vide : fondu de sortie sur ce qui reste, puis ATTENTE. */
                pb_count_underrun(l, n - got);
                env_end = 0.0f;
                r->state = ST_WAIT;
                tail = 1;
            }
        }
        pb_reader_beat(l, n);
    } else {
        if (r->state == ST_PLAY) tail = 1;
        r->state = ST_WAIT;
    }

    /* --- Mixage --- */
    const float wet0 = r->gain_cur * r->env;
    const float wet1 = r->gain_target * env_end;
    const float span_len = got ? (float)got : 1.0f;
    float wet = wet0;
    const float dwet = (wet1 - wet0) / span_len;
    uint32_t i = 0;
    if (got) {
        mix_span(r, sp.p1, sp.n1, &i, &dry, ddry, &wet, dwet);
        mix_span(r, sp.p2, sp.n2, &i, &dry, ddry, &wet, dwet);
        pb_read_commit(l, got);
    }
    /* Reste du bloc (ou bloc entier) : le signal d'entrée, plus la queue
     * qui ramène la dernière valeur du retour à zéro si on vient d'arrêter. */
    const uint32_t rest = n - i;
    for (int c = 0; c < 2; c++) {
        const float *in = r->in[c];
        float *out = r->out[c];
        float d = dry;
        if (tail && rest) {
            float t = r->last[c];
            const float dt = t / (float)rest;
            for (uint32_t j = i; j < n; j++, d += ddry) { t -= dt; out[j] = in[j] * d + t; }
        } else {
            for (uint32_t j = i; j < n; j++, d += ddry) out[j] = in[j] * d;
        }
    }
    if (r->state != ST_PLAY) { r->last[0] = r->last[1] = 0.0f; }

    r->dry_cur = dry_target;
    r->gain_cur = r->gain_target;
    r->env = env_end;
    if (r->p_connected) *r->p_connected = connected ? 1.0f : 0.0f;
}

static void deactivate(LV2_Handle h)
{
    Return *r = (Return *)h;
    if (r->cur >= 0 && pb_link_ok(&r->links[r->cur]))
        pb_reader_release(&r->links[r->cur], r->me);
    if (r->p_connected) *r->p_connected = 0.0f;
}

static void cleanup(LV2_Handle h)
{
    Return *r = (Return *)h;
    for (uint32_t i = 0; i < PB_MAX_LINKS; i++) {
        if (pb_link_ok(&r->links[i])) pb_reader_release(&r->links[i], r->me);
        pb_link_close(&r->links[i]);
    }
    free(r);
}

static const void *extension_data(const char *uri) { (void)uri; return NULL; }

const LV2_Descriptor pb_return_descriptor = {
    PB_URI_RETURN, instantiate, connect_port, activate, run, deactivate,
    cleanup, extension_data
};
