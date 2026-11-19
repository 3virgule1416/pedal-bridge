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
 * Kit : liste fixe (les .kit de scripts/kits, en ordre alphabétique, écrits
 * dans la description à la compilation). Morceau : sélecteur de fichier de
 * PiPedal (propriété patch « song »), les .h2song du dossier
 * /var/pipedal/audio_uploads/DLS Drums, que le plugin crée ouvert en écriture
 * à tous (2777) pour qu'on puisse y copier des morceaux sans sudo. Le chemin
 * est gardé dans le preset (état LV2) et passé au client par le fichier de
 * contrôle (verrou de séquence).
 *
 * Temps réel : tout est ouvert à l'instanciation ; run() ne fait que des
 * atomiques, du calcul et des copies.
 */
#include "io.h"
#include "pb_ctl.h"
#include "plugin.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SONG_DIR "/var/pipedal/audio_uploads/DLS Drums"

/* Ports de contrôle après les ports audio, rangés par section (PiPedal ouvre
 * une nouvelle boîte dès que la section change d'un port au suivant). */
enum {
    /* Song : chaque bouton à côté de sa liste */
    C_KIT = 0, C_INTRO, C_INTRO_BTN, C_OUTRO, C_OUTRO_BTN,
    /* Parts */
    C_PART, C_GROOVE /* Variation */, C_AUTOFILL, O_PART, O_GROOVE /* variation jouée */,
    /* Pedals */
    C_PLAYSTOP, C_PART_PREV, C_PART_NEXT, C_PREV, C_NEXT, C_FILL,
    /* Tempo */
    C_TEMPO, C_BPM_DOWN, C_BPM_UP, C_TAP, C_METRO, O_BEAT, O_PULSE, O_ONE, O_ROLLING, O_TEMPO,
    /* Bridge */
    C_GAIN, O_LINK,
    C_NUM,
    /* Ports atom (messages patch du sélecteur de fichier), après les autres. */
    A_CONTROL = C_NUM, A_NOTIFY
};

/* Port de chaque bouton, dans l'ordre de enum pb_drum_btn. */
static const int BTN_PORT[PB_DBTN_COUNT] = { C_PLAYSTOP, C_PREV, C_NEXT, C_TAP, C_BPM_UP, C_BPM_DOWN, C_FILL,
                                             C_PART_PREV, C_PART_NEXT, C_OUTRO_BTN, C_INTRO_BTN };

enum { LINK_NONE = 0, LINK_NO_AUDIO = 1, LINK_OK = 2 };

/* BPM + / BPM - tenus : un pas à l'appui, puis répétition après 0,4 s (un
 * pas toutes les 120 ms), plus vite après 2 s (toutes les 40 ms). */
#define HOLD_FIRST_S  0.4
#define HOLD_SLOW_S   0.12
#define HOLD_FAST_S   0.04
#define HOLD_FAST_AFTER_S 2.0

typedef struct { uint32_t held, next; } Hold;      /* en trames */

/* Propriété « fichier » (sélecteur de PiPedal). */
enum { F_SONG = 0, F_NUM };
typedef struct {
    LV2_URID urid;
    char     path[PB_CTL_PATH];
    int      send;                              /* renvoyer le chemin à PiPedal */
    int      publish;                           /* l'écrire dans le fichier de contrôle */
} FileProp;

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
    Hold         hold[2];                       /* BPM +, BPM - */

    /* Kit et morceau (sélecteurs de fichier). Sans urid:map : rien. */
    const LV2_Atom_Sequence *control;
    LV2_Atom_Sequence       *notify;
    LV2_URID_Map  *map;
    LV2_Atom_Forge forge;
    LV2_URID u_path, u_urid, u_object, u_set, u_get, u_property, u_value;
    FileProp f[F_NUM];
} Drum;

/* Hors temps réel (instanciation) : dossier ouvert en écriture à tous pour
 * y copier des fichiers sans sudo. Échec sans gravité. */
static void make_dir(const char *dir)
{
    if (mkdir(dir, 02777) != 0 && errno != EEXIST) return;
    chmod(dir, 02777);                          /* mkdir est limité par l'umask */
}

static LV2_Handle instantiate_common(double rate, int stereo, const LV2_Feature *const *features)
{
    Drum *p = (Drum *)calloc(1, sizeof *p);
    if (!p) return NULL;
    for (int i = 0; features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map)) p->map = (LV2_URID_Map *)features[i]->data;
    if (p->map) {
        LV2_URID_Map *m = p->map;
        p->u_path = m->map(m->handle, LV2_ATOM__Path);
        p->u_urid = m->map(m->handle, LV2_ATOM__URID);
        p->u_object = m->map(m->handle, LV2_ATOM__Object);
        p->u_set = m->map(m->handle, LV2_PATCH__Set);
        p->u_get = m->map(m->handle, LV2_PATCH__Get);
        p->u_property = m->map(m->handle, LV2_PATCH__property);
        p->u_value = m->map(m->handle, LV2_PATCH__value);
        p->f[F_SONG].urid = m->map(m->handle, PB_URI_DRUM_SONG);
        lv2_atom_forge_init(&p->forge, m);
        make_dir(SONG_DIR);
    }
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
{ (void)d; (void)b; return instantiate_common(r, 0, f); }
static LV2_Handle inst_stereo(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; return instantiate_common(r, 1, f); }

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
    if (k == A_CONTROL) p->control = (const LV2_Atom_Sequence *)data;
    else if (k == A_NOTIFY) p->notify = (LV2_Atom_Sequence *)data;
    else if (k < C_NUM) p->c[k] = (float *)data;
}

static void activate(LV2_Handle h)
{
    Drum *p = (Drum *)h;
    pb_return_io_activate(&p->ret);
    p->need_take = 1;
    pb_watch_init(&p->client, p->rate / 2u);
    for (int b = 0; b < PB_DBTN_COUNT; b++)
        p->last_btn[b] = p->c[BTN_PORT[b]] ? *p->c[BTN_PORT[b]] : 0.0f;
    p->hold[0].held = p->hold[1].held = 0;
    for (int i = 0; i < F_NUM; i++) p->f[i].send = p->f[i].publish = 1;
}

/* Message de PiPedal : patch:Set (nouveau fichier) ou patch:Get. Copie sans
 * allocation. */
static void handle_object(Drum *p, const LV2_Atom_Object *obj)
{
    if (obj->body.otype == p->u_get) {
        for (int i = 0; i < F_NUM; i++) p->f[i].send = 1;
        return;
    }
    if (obj->body.otype != p->u_set) return;
    const LV2_Atom *prop = NULL, *val = NULL;
    lv2_atom_object_get(obj, p->u_property, &prop, p->u_value, &val, 0);
    if (!prop || prop->type != p->u_urid || !val || val->type != p->u_path) return;
    const LV2_URID which = ((const LV2_Atom_URID *)prop)->body;
    for (int i = 0; i < F_NUM; i++) {
        FileProp *F = &p->f[i];
        if (which != F->urid) continue;
        size_t n = val->size;
        if (n >= sizeof F->path) n = sizeof F->path - 1;
        memcpy(F->path, LV2_ATOM_BODY_CONST(val), n);
        F->path[n] = 0;
        F->send = F->publish = 1;
    }
}

/* Chemin -> fichier de contrôle (verrou de séquence). */
static void publish_path(uint32_t *seq, char *dst, const char *src)
{
    const uint32_t s = PB_LOAD_RLX(seq);
    PB_STORE_REL(seq, (s | 1u) + 0u);                          /* impair : écriture en cours */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(dst, src, PB_CTL_PATH);
    PB_STORE_REL(seq, (s | 1u) + 1u);                          /* pair : prêt */
}

/* Réponse à PiPedal : chemins actuels (patch:Set), pour son affichage. */
static void send_paths(Drum *p)
{
    if (!p->notify || !p->map) return;
    const uint32_t cap = p->notify->atom.size;
    lv2_atom_forge_set_buffer(&p->forge, (uint8_t *)p->notify, cap);
    LV2_Atom_Forge_Frame seq;
    lv2_atom_forge_sequence_head(&p->forge, &seq, 0);
    for (int i = 0; i < F_NUM; i++) {
        FileProp *F = &p->f[i];
        if (!F->send) continue;
        LV2_Atom_Forge_Frame obj;
        lv2_atom_forge_frame_time(&p->forge, 0);
        lv2_atom_forge_object(&p->forge, &obj, 0, p->u_set);
        lv2_atom_forge_key(&p->forge, p->u_property);
        lv2_atom_forge_urid(&p->forge, F->urid);
        lv2_atom_forge_key(&p->forge, p->u_value);
        lv2_atom_forge_path(&p->forge, F->path, (uint32_t)strlen(F->path));
        lv2_atom_forge_pop(&p->forge, &obj);
        F->send = 0;
    }
    lv2_atom_forge_pop(&p->forge, &seq);
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

    /* --- Messages du sélecteur de fichier --- */
    if (p->control && p->map)
        LV2_ATOM_SEQUENCE_FOREACH(p->control, ev)
            if (ev->body.type == p->u_object)
                handle_object(p, (const LV2_Atom_Object *)&ev->body);
    send_paths(p);

    /* --- Commandes vers le client --- */
    int pressed[PB_DBTN_COUNT];
    for (int b = 0; b < PB_DBTN_COUNT; b++) {
        const float v = port(p, BTN_PORT[b], 0.0f);
        pressed[b] = v > 0.5f && p->last_btn[b] <= 0.5f;   /* front montant */
        p->last_btn[b] = v;
    }
    /* Répétition de BPM + / BPM - tenus (pas supplémentaires). */
    uint32_t steps[PB_DBTN_COUNT] = {0};
    for (int b = 0; b < PB_DBTN_COUNT; b++) steps[b] = (uint32_t)pressed[b];
    for (int h = 0; h < 2; h++) {
        const int b = PB_DBTN_BPM_UP + h;
        Hold *H = &p->hold[h];
        if (port(p, BTN_PORT[b], 0.0f) <= 0.5f) { H->held = 0; continue; }
        if (pressed[b]) { H->held = 0; H->next = (uint32_t)(p->rate * HOLD_FIRST_S); continue; }
        H->held += n;
        for (int guard = 0; H->held >= H->next && guard < 16; guard++) {
            steps[b]++;
            const double gap = H->held > p->rate * HOLD_FAST_AFTER_S ? HOLD_FAST_S : HOLD_SLOW_S;
            H->next += (uint32_t)(p->rate * gap);
        }
    }
    int groove = (int)lrintf(port(p, C_GROOVE, 1.0f));
    int var = groove < 0 ? 0 : groove > 128 ? 128 : groove;      /* 0 : None (partie sautée) */
    if (groove < 1) groove = 1;
    if (groove > (int)PB_CTL_GROOVES) groove = (int)PB_CTL_GROOVES;

    pb_ctl *ctl = p->ctl;
    if (!ctl) {
        set_out(p, O_GROOVE, 0); set_out(p, O_PART, 0); set_out(p, O_BEAT, 0); set_out(p, O_PULSE, 0);
        set_out(p, O_ONE, 0); set_out(p, O_ROLLING, 0); set_out(p, O_TEMPO, 0);
        set_out(p, O_LINK, back ? LINK_OK : LINK_NONE);
        return;
    }
    if (p->need_take) {                          /* la dernière instance activée commande */
        p->need_take = 0;
        PB_STORE_REL(&ctl->drum_owner, p->me);
        for (int i = 0; i < F_NUM; i++) p->f[i].publish = 1;
    }
    if (PB_LOAD_ACQ(&ctl->drum_owner) == p->me) {
        if (p->map) {
            if (p->f[F_SONG].publish) { publish_path(&ctl->drum_song_seq, ctl->drum_song, p->f[F_SONG].path); p->f[F_SONG].publish = 0; }
        }
        PB_STORE_RLX(&ctl->drum_groove, (uint32_t)groove);
        PB_STORE_RLX(&ctl->drum_var, (uint32_t)var);
        PB_STORE_RLX(&ctl->drum_part, (uint32_t)lrintf(port(p, C_PART, 0.0f)) + 1u);
        PB_STORE_RLX(&ctl->drum_intro, (uint32_t)lrintf(port(p, C_INTRO, 0.0f)) + 1u);
        PB_STORE_RLX(&ctl->drum_outro, (uint32_t)lrintf(port(p, C_OUTRO, 1.0f)));
        PB_STORE_RLX(&ctl->drum_nofill, port(p, C_AUTOFILL, 1.0f) > 0.5f ? 0u : 1u);
        PB_STORE_RLX(&ctl->drum_tempo, (uint32_t)lrintf(port(p, C_TEMPO, 120.0f) * 100.0f));
        PB_STORE_RLX(&ctl->drum_metro, port(p, C_METRO, 0.0f) > 0.5f ? 1u : 0u);
        {
            const long kit = lrintf(port(p, C_KIT, 0.0f));
            PB_STORE_RLX(&ctl->drum_kit_num, kit >= 0 && kit < 64 ? (uint32_t)kit + 1u : 0u);
        }
        for (int b = 0; b < PB_DBTN_COUNT; b++)
            if (steps[b])
                PB_STORE_REL(&ctl->drum_btn[b], PB_LOAD_RLX(&ctl->drum_btn[b]) + steps[b]);
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
    uint32_t part = alive ? PB_LOAD_RLX(&ctl->part_cur) : 0u;
    set_out(p, O_PART, (float)(part <= 8 ? part : 0));
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

/* --- État (preset) : les chemins du kit et du morceau --- */
static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature *const *f)
{
    (void)flags; (void)f;
    Drum *p = (Drum *)h;
    if (!p->map) return LV2_STATE_SUCCESS;
    for (int i = 0; i < F_NUM; i++) {
        const LV2_State_Status st = store(sh, p->f[i].urid, p->f[i].path, strlen(p->f[i].path) + 1, p->u_path,
                                          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        if (st != LV2_STATE_SUCCESS) return st;
    }
    return LV2_STATE_SUCCESS;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature *const *f)
{
    (void)flags; (void)f;
    Drum *p = (Drum *)h;
    if (!p->map) return LV2_STATE_SUCCESS;
    for (int i = 0; i < F_NUM; i++) {
        FileProp *F = &p->f[i];
        size_t size;
        uint32_t type, fl;
        const char *v = (const char *)retrieve(sh, F->urid, &size, &type, &fl);
        if (v && type == p->u_path) {
            size_t n = size < sizeof F->path ? size : sizeof F->path - 1;
            memcpy(F->path, v, n);
            F->path[n] = 0;
            F->send = F->publish = 1;
        }
    }
    return LV2_STATE_SUCCESS;
}

static const void *extension_data(const char *uri)
{
    static const LV2_State_Interface state = { save, restore };
    return !strcmp(uri, LV2_STATE__interface) ? &state : NULL;
}

const LV2_Descriptor pb_drum_mono_descriptor = {
    PB_URI_DRUM_MONO, inst_mono, connect_port, activate, run, deactivate, cleanup, extension_data };
const LV2_Descriptor pb_drum_stereo_descriptor = {
    PB_URI_DRUM_STEREO, inst_stereo, connect_port, activate, run, deactivate, cleanup, extension_data };
