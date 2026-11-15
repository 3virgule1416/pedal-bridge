/*
 * Plugin LV2 « DLS Looper » (mono et stéréo).
 *
 * Audio (lien 1, fixe) : l'entrée est copiée vers SooperLooper (envoi), PUIS
 * les boucles sont ajoutées au signal (retour, mode Add). La copie part avant
 * l'ajout : les boucles ne se réenregistrent pas dans elles-mêmes.
 *   - mono : la même entrée à gauche et à droite vers le looper ; le retour
 *     stéréo est ramené en mono ((G + D) / 2) ;
 *   - stéréo : tel quel.
 *
 * Contrôle (fichier partagé pb_ctl.h) : le plugin publie la boucle choisie,
 * le tempo et un compteur par bouton ; le client pedal_bridge exécute au bon
 * moment (SooperLooper, transport) et renvoie l'état, que le plugin recopie
 * dans ses ports de sortie (voyants et textes de PiPedal).
 *
 * Temps réel : tout est ouvert à l'instanciation ; run() ne fait que des
 * atomiques, du calcul et des copies.
 */
#include "io.h"
#include "pb_ctl.h"
#include "plugin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Ports de contrôle, après les ports audio (1+1 en mono, 2+2 en stéréo).
 * Rangés par section : PiPedal ouvre une nouvelle boîte dès que la section
 * change d'un port au suivant. */
enum {
    /* Boucle */
    C_LOOP = 0, C_RECORD, C_OVERDUB, C_UNDO, C_REDO, C_MUTE, C_CLEAR, O_STATE,
    /* Transport */
    C_PLAYSTOP, C_TEMPO, O_BEAT, O_PULSE, O_ONE, O_ROLLING, O_TEMPO,
    /* Pont */
    C_GAIN, O_LINK,
    C_NUM
};

/* Port de chaque bouton, dans l'ordre de enum pb_btn. */
static const int BTN_PORT[PB_BTN_COUNT] = {
    C_RECORD, C_OVERDUB, C_UNDO, C_REDO, C_MUTE, C_CLEAR, C_PLAYSTOP
};

/* Sortie « Liaison ». */
enum { LINK_NONE = 0, LINK_NO_AUDIO = 1, LINK_OK = 2 };

typedef struct {
    int stereo;
    const float *in[2];
    float *out[2];
    float *c[C_NUM];

    pb_send_io   send;
    pb_return_io ret;
    pb_ctl      *ctl;
    uint64_t     me;
    uint32_t     rate;
    int          need_take;
    pb_watch     client;
    float        last_btn[PB_BTN_COUNT];
} Looper;

static LV2_Handle instantiate_common(double rate, int stereo)
{
    Looper *p = (Looper *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->stereo = stereo;
    p->rate = (uint32_t)(rate + 0.5);
    p->me = pb_new_owner_id();
    pb_watch_init(&p->client, p->rate / 2u);
    pb_send_io_open(&p->send, 1, p->rate);       /* échec : pas d'audio du pont */
    pb_return_io_open(&p->ret, 1, p->rate);
    char err[256];
    if (pb_ctl_open(&p->ctl, 1, err, sizeof err) != 0) {
        pb_log_error("DLS Looper: %s\n", err);
        p->ctl = NULL;                           /* audio seulement */
    }
    return p;
}

static LV2_Handle inst_mono(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; (void)f; return instantiate_common(r, 0); }
static LV2_Handle inst_stereo(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; (void)f; return instantiate_common(r, 1); }

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Looper *p = (Looper *)h;
    const uint32_t na = p->stereo ? 4 : 2;
    if (port < na) {
        const uint32_t half = na / 2;
        if (port < half) p->in[port] = (const float *)data;
        else p->out[port - half] = (float *)data;
        return;
    }
    const uint32_t k = port - na;
    if (k < C_NUM) p->c[k] = (float *)data;
}

static void activate(LV2_Handle h)
{
    Looper *p = (Looper *)h;
    pb_send_io_activate(&p->send);
    pb_return_io_activate(&p->ret);
    p->need_take = 1;
    pb_watch_init(&p->client, p->rate / 2u);
    for (int b = 0; b < PB_BTN_COUNT; b++)
        p->last_btn[b] = p->c[BTN_PORT[b]] ? *p->c[BTN_PORT[b]] : 0.0f;
}

static inline float port(const Looper *p, int k, float def) { return p->c[k] ? *p->c[k] : def; }
static inline void set_out(Looper *p, int k, float v) { if (p->c[k]) *p->c[k] = v; }

static void run(LV2_Handle h, uint32_t n)
{
    Looper *p = (Looper *)h;
    const int nch = p->stereo ? 2 : 1;

    /* --- Audio : envoi de l'entrée, puis entrée + boucles en sortie --- */
    const float *inl = p->in[0], *inr = p->stereo ? p->in[1] : p->in[0];
    const int sent = pb_send_io_run(&p->send, inl, inr, n);
    for (int c = 0; c < nch; c++)
        if (p->out[c] != p->in[c]) memmove(p->out[c], p->in[c], n * sizeof(float));
    float *const outs[2] = { p->out[0], p->stereo ? p->out[1] : NULL };
    const int back = pb_return_io_run(&p->ret, outs, nch, n, port(p, C_GAIN, 0.0f));

    /* --- Commandes vers le client --- */
    pb_ctl *ctl = p->ctl;
    int pressed[PB_BTN_COUNT];
    for (int b = 0; b < PB_BTN_COUNT; b++) {
        const float v = port(p, BTN_PORT[b], 0.0f);
        pressed[b] = v > 0.5f && p->last_btn[b] <= 0.5f;   /* front montant */
        p->last_btn[b] = v;
    }
    int loop = (int)lrintf(port(p, C_LOOP, 1.0f));
    if (loop < 1) loop = 1;
    if (loop > (int)PB_CTL_LOOPS) loop = (int)PB_CTL_LOOPS;

    if (!ctl) {
        set_out(p, O_STATE, PB_LS_OFFLINE); set_out(p, O_BEAT, 0); set_out(p, O_PULSE, 0);
        set_out(p, O_ONE, 0); set_out(p, O_ROLLING, 0); set_out(p, O_TEMPO, 0);
        set_out(p, O_LINK, sent && back ? LINK_OK : LINK_NONE);
        return;
    }
    if (p->need_take) {                          /* la dernière instance activée commande */
        p->need_take = 0;
        PB_STORE_REL(&ctl->looper_owner, p->me);
    }
    if (PB_LOAD_ACQ(&ctl->looper_owner) == p->me) {
        PB_STORE_RLX(&ctl->looper_loop, (uint32_t)loop);
        PB_STORE_RLX(&ctl->looper_tempo, (uint32_t)lrintf(port(p, C_TEMPO, 120.0f) * 100.0f));
        for (int b = 0; b < PB_BTN_COUNT; b++)
            if (pressed[b])
                PB_STORE_REL(&ctl->looper_btn[b], PB_LOAD_RLX(&ctl->looper_btn[b]) + 1u);
        PB_STORE_REL(&ctl->looper_heartbeat, PB_LOAD_RLX(&ctl->looper_heartbeat) + 1u);
    }

    /* --- État reçu du client -> sorties --- */
    pb_watch_update(&p->client, PB_LOAD_ACQ(&ctl->client_heartbeat), n);
    const int alive = p->client.alive;
    const int sl_ok = alive && PB_LOAD_RLX(&ctl->sl_ok);
    const int rolling = alive && PB_LOAD_RLX(&ctl->rolling);
    uint32_t beat = PB_LOAD_RLX(&ctl->beat);
    if (beat > 8) beat = 8;
    const float frac = (float)PB_LOAD_RLX(&ctl->beat_frac) / 65536.0f;
    const int pulse = rolling && beat >= 1 && frac < 0.25f;
    const float bpm = (float)PB_LOAD_RLX(&ctl->bpm) * 0.01f;
    float tempo = 0.0f;
    if (alive && bpm > 0.0f) {
        tempo = roundf(bpm);
        if (tempo < 40.0f) tempo = 40.0f;
        if (tempo > 240.0f) tempo = 240.0f;
    }
    uint32_t st = sl_ok ? PB_LOAD_RLX(&ctl->loop_state[loop - 1]) : PB_LS_OFFLINE;
    if (st >= PB_LS_COUNT) st = PB_LS_OTHER;

    set_out(p, O_STATE, (float)st);
    set_out(p, O_BEAT, rolling ? (float)beat : 0.0f);
    set_out(p, O_PULSE, pulse ? 1.0f : 0.0f);
    set_out(p, O_ONE, pulse && beat == 1 ? 1.0f : 0.0f);
    set_out(p, O_ROLLING, rolling ? 1.0f : 0.0f);
    set_out(p, O_TEMPO, tempo);
    set_out(p, O_LINK, sent && back ? LINK_OK : alive ? LINK_NO_AUDIO : LINK_NONE);
}

static void deactivate(LV2_Handle h)
{
    Looper *p = (Looper *)h;
    pb_send_io_deactivate(&p->send);
    pb_return_io_deactivate(&p->ret);
    if (p->ctl) {
        uint64_t cur = p->me;
        __atomic_compare_exchange_n(&p->ctl->looper_owner, &cur, (uint64_t)0, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
}

static void cleanup(LV2_Handle h)
{
    Looper *p = (Looper *)h;
    deactivate(h);
    pb_send_io_close(&p->send);
    pb_return_io_close(&p->ret);
    pb_ctl_close(p->ctl);
    free(p);
}

static const void *extension_data(const char *uri) { (void)uri; return NULL; }

const LV2_Descriptor pb_looper_mono_descriptor = {
    PB_URI_LOOPER_MONO, inst_mono, connect_port, activate, run, deactivate, cleanup, extension_data };
const LV2_Descriptor pb_looper_stereo_descriptor = {
    PB_URI_LOOPER_STEREO, inst_stereo, connect_port, activate, run, deactivate, cleanup, extension_data };
