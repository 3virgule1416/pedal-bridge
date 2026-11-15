/* Audio du pont pour DLS Looper / DLS Drum : voir io.h. */
#include "io.h"
#include "plugin.h"

#include <math.h>
#include <string.h>

#define DEFAULT_TARGET 256u   /* réserve si l'écrivain n'annonce rien */

enum { ST_WAIT = 0, ST_PLAY = 1 };

/* ------------------------------------------------------------- envoi --- */

int pb_send_io_open(pb_send_io *s, uint32_t link_no, uint32_t rate)
{
    memset(s, 0, sizeof *s);
    s->rate = rate;
    s->me = pb_new_owner_id();
    pb_watch_init(&s->peer, rate / 4u);
    char err[256];
    if (pb_link_open(&s->link, link_no, PB_DIR_SEND, 1, err, sizeof err) != 0) {
        pb_log_error("pedal-bridge: %s\n", err);
        return -1;
    }
    return 0;
}

void pb_send_io_activate(pb_send_io *s)
{
    s->need_take = 1;
    pb_watch_init(&s->peer, s->rate / 4u);
}

void pb_send_io_deactivate(pb_send_io *s)
{
    if (pb_link_ok(&s->link)) pb_writer_release(&s->link, s->me);
}

void pb_send_io_close(pb_send_io *s)
{
    pb_send_io_deactivate(s);
    pb_link_close(&s->link);
}

int pb_send_io_run(pb_send_io *s, const float *inl, const float *inr, uint32_t n)
{
    pb_link *l = &s->link;
    if (n == 0 || !pb_link_ok(l)) return 0;
    if (s->need_take) {
        s->need_take = 0;
        pb_writer_take(l, s->me, s->rate);
        pb_watch_init(&s->peer, s->rate / 4u);
    }
    if (!pb_writer_claim(l, s->me)) return 0;
    pb_watch_update(&s->peer, PB_LOAD_ACQ(&l->hdr->reader_heartbeat), n);
    const int connected = s->peer.alive;
    if (connected) {
        pb_span sp;
        const uint32_t got = pb_write_reserve(l, n, &sp);
        uint32_t i = 0;
        for (uint32_t k = 0; k < sp.n1; k++, i++) { sp.p1[2 * k] = inl[i]; sp.p1[2 * k + 1] = inr[i]; }
        for (uint32_t k = 0; k < sp.n2; k++, i++) { sp.p2[2 * k] = inl[i]; sp.p2[2 * k + 1] = inr[i]; }
        pb_write_commit(l, got);
        if (got < n) pb_count_overrun(l, n - got);
    }
    pb_writer_beat(l, n);
    return connected;
}

/* ------------------------------------------------------------ retour --- */

int pb_return_io_open(pb_return_io *r, uint32_t link_no, uint32_t rate)
{
    memset(r, 0, sizeof *r);
    r->rate = rate;
    r->me = pb_new_owner_id();
    r->gain_target = r->gain_cur = 1.0f;
    pb_watch_init(&r->peer, rate / 4u);
    char err[256];
    if (pb_link_open(&r->link, link_no, PB_DIR_RETURN, 1, err, sizeof err) != 0) {
        pb_log_error("pedal-bridge: %s\n", err);
        return -1;
    }
    return 0;
}

void pb_return_io_activate(pb_return_io *r)
{
    r->need_take = 1;
    r->state = ST_WAIT;
    r->env = 0.0f;
    r->last[0] = r->last[1] = 0.0f;
    r->gain_cur = r->gain_target;
    pb_watch_init(&r->peer, r->rate / 4u);
}

void pb_return_io_deactivate(pb_return_io *r)
{
    if (pb_link_ok(&r->link)) pb_reader_release(&r->link, r->me);
}

void pb_return_io_close(pb_return_io *r)
{
    pb_return_io_deactivate(r);
    pb_link_close(&r->link);
}

static inline float db_to_gain(float db)
{
    return db <= -60.0f ? 0.0f : powf(10.0f, db * 0.05f);
}

/* Ajoute un segment du tampon partagé ; wet avance d'un pas par trame. */
static inline void add_span(pb_return_io *r, const float *src, uint32_t m, uint32_t *i,
                            float *const out[2], int nch, float *wet, float dwet)
{
    for (uint32_t k = 0; k < m; k++, (*i)++) {
        const float wl = src[2 * k] * *wet, wr = src[2 * k + 1] * *wet;
        if (nch == 2) { out[0][*i] += wl; out[1][*i] += wr; }
        else          { out[0][*i] += 0.5f * (wl + wr); }
        r->last[0] = wl; r->last[1] = wr;
        *wet += dwet;
    }
}

int pb_return_io_run(pb_return_io *r, float *const out[2], int nch, uint32_t n, float gain_db)
{
    pb_link *l = &r->link;
    if (n == 0) return 0;
    int tail = 0;   /* le retour s'arrête dans ce bloc : adoucir la fin */

    if (gain_db != r->gain_db) { r->gain_db = gain_db; r->gain_target = db_to_gain(gain_db); }

    uint32_t got = 0;
    pb_span sp = { 0, 0, 0, 0 };
    float env_end = 0.0f;
    int connected = 0;

    if (pb_link_ok(l) && r->need_take) {
        if (r->state == ST_PLAY) tail = 1;
        r->need_take = 0;
        pb_reader_take(l, r->me, r->rate);
        pb_watch_init(&r->peer, r->rate / 4u);
        r->state = ST_WAIT;
        r->env = 0.0f;
    }

    if (pb_link_ok(l) && pb_reader_claim(l, r->me)) {
        pb_watch_update(&r->peer, PB_LOAD_ACQ(&l->hdr->writer_heartbeat), n);
        connected = r->peer.alive;

        uint32_t target = PB_LOAD_RLX(&l->hdr->writer_target);
        if (target == 0) target = DEFAULT_TARGET;
        if (target > PB_CAPACITY / 2) target = PB_CAPACITY / 2;
        const uint32_t start_at = target > n ? target : n;

        if (r->state == ST_WAIT && connected && pb_read_available(l) >= start_at) {
            pb_reader_resync(l, start_at);   /* jeter le vieux son en trop */
            r->state = ST_PLAY;
            r->env = 0.0f;                   /* fondu d'entrée */
        }
        if (r->state == ST_PLAY) {
            got = pb_read_reserve(l, n, &sp);
            if (got == n) {
                env_end = 1.0f;
            } else {                         /* tampon vide : fondu de sortie, ATTENTE */
                pb_count_underrun(l, n - got);
                r->state = ST_WAIT;
                tail = 1;
            }
        }
        pb_reader_beat(l, n);
    } else {
        if (r->state == ST_PLAY) tail = 1;
        r->state = ST_WAIT;
    }

    const float wet0 = r->gain_cur * r->env;
    const float wet1 = r->gain_target * env_end;
    float wet = wet0;
    const float dwet = (wet1 - wet0) / (got ? (float)got : 1.0f);
    uint32_t i = 0;
    if (got) {
        add_span(r, sp.p1, sp.n1, &i, out, nch, &wet, dwet);
        add_span(r, sp.p2, sp.n2, &i, out, nch, &wet, dwet);
        pb_read_commit(l, got);
    }
    /* Arrêt dans ce bloc : la dernière valeur descend en ligne droite à zéro. */
    const uint32_t rest = n - i;
    if (tail && rest) {
        float t0 = r->last[0], t1 = r->last[1];
        const float d0 = t0 / (float)rest, d1 = t1 / (float)rest;
        for (uint32_t j = i; j < n; j++) {
            t0 -= d0; t1 -= d1;
            if (nch == 2) { out[0][j] += t0; out[1][j] += t1; }
            else          { out[0][j] += 0.5f * (t0 + t1); }
        }
    }
    if (r->state != ST_PLAY) r->last[0] = r->last[1] = 0.0f;

    r->gain_cur = r->gain_target;
    r->env = env_end;
    return connected;
}
