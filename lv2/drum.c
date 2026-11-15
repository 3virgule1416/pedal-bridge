/*
 * Plugin LV2 « DLS Drum » (mono et stéréo).
 *
 * Audio (lien 2, fixe) : la batterie d'Hydrogen est AJOUTÉE au signal (retour,
 * mode Add). À placer en fin de chaîne : la batterie ne passe dans aucun effet.
 * Mono : le retour stéréo est ramené à (G + D) / 2.
 *
 * Contrôle (fichier partagé pb_ctl.h, section « drum ») : groove choisi,
 * tempo et compteurs de boutons (Play/Stop, groove précédent/suivant, tap
 * tempo), interrupteur du métronome. Le client pedal_bridge pilote Hydrogen et le transport, et renvoie
 * le groove joué et la mesure.
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

/* Ports de contrôle après les ports audio, rangés par section (PiPedal ouvre
 * une nouvelle boîte dès que la section change d'un port au suivant). */
enum {
    /* Drums */
    C_GROOVE = 0, C_PREV, C_NEXT, O_GROOVE,
    /* Transport */
    C_PLAYSTOP, C_TEMPO, C_TAP, C_METRO, O_BEAT, O_PULSE, O_ONE, O_ROLLING, O_TEMPO,
    /* Bridge */
    C_GAIN, O_LINK,
    C_NUM
};

/* Port de chaque bouton, dans l'ordre de enum pb_drum_btn. */
static const int BTN_PORT[PB_DBTN_COUNT] = { C_PLAYSTOP, C_PREV, C_NEXT, C_TAP };

enum { LINK_NONE = 0, LINK_NO_AUDIO = 1, LINK_OK = 2 };

typedef struct {
    int stereo;
    const float *in[2];
    float *out[2];
    float *c[C_NUM];

    pb_return_io ret;
    pb_ctl      *ctl;
    uint64_t     me;
    uint32_t     rate;
    int          need_take;
    pb_watch     client;
    float        last_btn[PB_DBTN_COUNT];
} Drum;

static LV2_Handle instantiate_common(double rate, int stereo)
{
    Drum *p = (Drum *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->stereo = stereo;
    p->rate = (uint32_t)(rate + 0.5);
    p->me = pb_new_owner_id();
    pb_watch_init(&p->client, p->rate / 2u);
    pb_return_io_open(&p->ret, 2, p->rate);      /* échec : pas de batterie */
    char err[256];
    if (pb_ctl_open(&p->ctl, 1, err, sizeof err) != 0) {
        pb_log_error("DLS Drum: %s\n", err);
        p->ctl = NULL;
    }
    return p;
}

static LV2_Handle inst_mono(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; (void)f; return instantiate_common(r, 0); }
static LV2_Handle inst_stereo(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; (void)f; return instantiate_common(r, 1); }

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Drum *p = (Drum *)h;
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
    Drum *p = (Drum *)h;
    pb_return_io_activate(&p->ret);
    p->need_take = 1;
    pb_watch_init(&p->client, p->rate / 2u);
    for (int b = 0; b < PB_DBTN_COUNT; b++)
        p->last_btn[b] = p->c[BTN_PORT[b]] ? *p->c[BTN_PORT[b]] : 0.0f;
}

static inline float port(const Drum *p, int k, float def) { return p->c[k] ? *p->c[k] : def; }
static inline void set_out(Drum *p, int k, float v) { if (p->c[k]) *p->c[k] = v; }

static void run(LV2_Handle h, uint32_t n)
{
    Drum *p = (Drum *)h;
    const int nch = p->stereo ? 2 : 1;

    /* --- Audio : entrée + batterie --- */
    for (int c = 0; c < nch; c++)
        if (p->out[c] != p->in[c]) memmove(p->out[c], p->in[c], n * sizeof(float));
    float *const outs[2] = { p->out[0], p->stereo ? p->out[1] : NULL };
    const int back = pb_return_io_run(&p->ret, outs, nch, n, port(p, C_GAIN, 0.0f));

    /* --- Commandes vers le client --- */
    int pressed[PB_DBTN_COUNT];
    for (int b = 0; b < PB_DBTN_COUNT; b++) {
        const float v = port(p, BTN_PORT[b], 0.0f);
        pressed[b] = v > 0.5f && p->last_btn[b] <= 0.5f;   /* front montant */
        p->last_btn[b] = v;
    }
    int groove = (int)lrintf(port(p, C_GROOVE, 1.0f));
    if (groove < 1) groove = 1;
    if (groove > 16) groove = 16;

    pb_ctl *ctl = p->ctl;
    if (!ctl) {
        set_out(p, O_GROOVE, 0); set_out(p, O_BEAT, 0); set_out(p, O_PULSE, 0);
        set_out(p, O_ONE, 0); set_out(p, O_ROLLING, 0); set_out(p, O_TEMPO, 0);
        set_out(p, O_LINK, back ? LINK_OK : LINK_NONE);
        return;
    }
    if (p->need_take) {                          /* la dernière instance activée commande */
        p->need_take = 0;
        PB_STORE_REL(&ctl->drum_owner, p->me);
    }
    if (PB_LOAD_ACQ(&ctl->drum_owner) == p->me) {
        PB_STORE_RLX(&ctl->drum_groove, (uint32_t)groove);
        PB_STORE_RLX(&ctl->drum_tempo, (uint32_t)lrintf(port(p, C_TEMPO, 120.0f) * 100.0f));
        PB_STORE_RLX(&ctl->drum_metro, port(p, C_METRO, 0.0f) > 0.5f ? 1u : 0u);
        for (int b = 0; b < PB_DBTN_COUNT; b++)
            if (pressed[b])
                PB_STORE_REL(&ctl->drum_btn[b], PB_LOAD_RLX(&ctl->drum_btn[b]) + 1u);
        PB_STORE_REL(&ctl->drum_heartbeat, PB_LOAD_RLX(&ctl->drum_heartbeat) + 1u);
    }

    /* --- État reçu du client -> sorties --- */
    pb_watch_update(&p->client, PB_LOAD_ACQ(&ctl->client_heartbeat), n);
    const int alive = p->client.alive;
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
    uint32_t cur = alive && PB_LOAD_RLX(&ctl->h2_ok) ? PB_LOAD_RLX(&ctl->groove_cur) : 0u;
    if (cur > PB_CTL_GROOVES) cur = 0;

    set_out(p, O_GROOVE, (float)cur);
    set_out(p, O_BEAT, rolling ? (float)beat : 0.0f);
    set_out(p, O_PULSE, pulse ? 1.0f : 0.0f);
    set_out(p, O_ONE, pulse && beat == 1 ? 1.0f : 0.0f);
    set_out(p, O_ROLLING, rolling ? 1.0f : 0.0f);
    set_out(p, O_TEMPO, tempo);
    set_out(p, O_LINK, back ? LINK_OK : alive ? LINK_NO_AUDIO : LINK_NONE);
}

static void deactivate(LV2_Handle h)
{
    Drum *p = (Drum *)h;
    pb_return_io_deactivate(&p->ret);
    if (p->ctl) {
        uint64_t cur = p->me;
        __atomic_compare_exchange_n(&p->ctl->drum_owner, &cur, (uint64_t)0, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
}

static void cleanup(LV2_Handle h)
{
    Drum *p = (Drum *)h;
    deactivate(h);
    pb_return_io_close(&p->ret);
    pb_ctl_close(p->ctl);
    free(p);
}

static const void *extension_data(const char *uri) { (void)uri; return NULL; }

const LV2_Descriptor pb_drum_mono_descriptor = {
    PB_URI_DRUM_MONO, inst_mono, connect_port, activate, run, deactivate, cleanup, extension_data };
const LV2_Descriptor pb_drum_stereo_descriptor = {
    PB_URI_DRUM_STEREO, inst_stereo, connect_port, activate, run, deactivate, cleanup, extension_data };
