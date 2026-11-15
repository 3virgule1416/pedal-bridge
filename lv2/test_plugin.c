/*
 * Plugin LV2 « DLS Test » (mono et stéréo) : vérifie dans PiPedal
 *   - le rendu des contrôles : bascule, bouton momentané (trigger), menu ;
 *   - les sorties : voyant, compteur, voyant clignotant, état « texte »
 *     (port de sortie avec liste de valeurs) ;
 *   - la liaison MIDI d'un CC à ces contrôles ;
 *   - un plugin mono ou stéréo en début de chaîne mono ;
 *   - le sélecteur de fichier (pipedal_ui:fileProperty) : dossier accepté,
 *     droits d'écriture, liste, ce qui est gardé dans le preset.
 *
 * L'audio passe tel quel. À l'instanciation, le plugin crée (s'il le peut)
 * /var/pipedal/audio_uploads/PedalBridgeTest/ avec trois faux fichiers de
 * groove ; la sortie « Dossier créé » dit si ça a marché.
 */
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define URI_BASE   "http://pedal-bridge.local/plugins/test"
#define URI_MONO   URI_BASE "-mono"
#define URI_STEREO URI_BASE "-stereo"
#define URI_FILE   URI_BASE "#fichier"
#define TEST_DIR   "/var/pipedal/audio_uploads/PedalBridgeTest"

/* Ports de contrôle, après les ports audio (1+1 en mono, 2+2 en stéréo). */
enum {
    C_TOGGLE = 0, C_TRIGGER, C_LIST,
    C_LED, C_COUNT, C_BLINK, C_STATE, C_NAMELEN, C_DIROK,
    C_CONTROL, C_NOTIFY,
    C_NUM
};

typedef struct {
    LV2_URID atom_Path, atom_URID, atom_Object, atom_Sequence;
    LV2_URID patch_Set, patch_Get, patch_property, patch_value;
    LV2_URID file;
} Urids;

typedef struct {
    int stereo;
    const float *in[2];
    float *out[2];
    float *c[C_NUM];                   /* ports de contrôle (float) */
    const LV2_Atom_Sequence *control;
    LV2_Atom_Sequence *notify;

    LV2_URID_Map *map;
    Urids u;
    LV2_Atom_Forge forge;

    double rate;
    uint64_t frames;
    float last_trigger;
    float count;
    float dir_ok;
    char path[1024];
    int send_path;                     /* renvoyer le chemin (patch:Set) */
} Test;

static void make_test_files(Test *t)
{
    /* Hors temps réel (instanciation). Échec sans gravité : voyant à 0. */
    if (mkdir(TEST_DIR, 0775) != 0 && errno != EEXIST) { t->dir_ok = 0; return; }
    static const char *names[] = {
        "01 - main 1-1 (1 mesure).groove",
        "03 - main 2-1 (1 mesure).groove",
        "12 - refrain (2 mesures).groove",
    };
    t->dir_ok = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", TEST_DIR, names[i]);
        FILE *f = fopen(p, "a");
        if (f) fclose(f); else t->dir_ok = 0;
    }
}

static LV2_Handle instantiate_common(double rate, const LV2_Feature *const *features, int stereo)
{
    LV2_URID_Map *map = NULL;
    for (int i = 0; features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map)) map = (LV2_URID_Map *)features[i]->data;
    if (!map) return NULL;                         /* urid:map obligatoire */
    Test *t = (Test *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->stereo = stereo;
    t->rate = rate;
    t->map = map;
    t->u.atom_Path = map->map(map->handle, LV2_ATOM__Path);
    t->u.atom_URID = map->map(map->handle, LV2_ATOM__URID);
    t->u.atom_Object = map->map(map->handle, LV2_ATOM__Object);
    t->u.atom_Sequence = map->map(map->handle, LV2_ATOM__Sequence);
    t->u.patch_Set = map->map(map->handle, LV2_PATCH__Set);
    t->u.patch_Get = map->map(map->handle, LV2_PATCH__Get);
    t->u.patch_property = map->map(map->handle, LV2_PATCH__property);
    t->u.patch_value = map->map(map->handle, LV2_PATCH__value);
    t->u.file = map->map(map->handle, URI_FILE);
    lv2_atom_forge_init(&t->forge, map);
    make_test_files(t);
    return t;
}

static LV2_Handle inst_mono(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; return instantiate_common(r, f, 0); }
static LV2_Handle inst_stereo(const LV2_Descriptor *d, double r, const char *b, const LV2_Feature *const *f)
{ (void)d; (void)b; return instantiate_common(r, f, 1); }

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Test *t = (Test *)h;
    const uint32_t na = t->stereo ? 4 : 2;
    if (port < na) {
        const uint32_t half = na / 2;
        if (port < half) t->in[port] = (const float *)data;
        else t->out[port - half] = (float *)data;
        return;
    }
    const uint32_t k = port - na;
    if (k == C_CONTROL) t->control = (const LV2_Atom_Sequence *)data;
    else if (k == C_NOTIFY) t->notify = (LV2_Atom_Sequence *)data;
    else if (k < C_NUM) t->c[k] = (float *)data;
}

static void activate(LV2_Handle h) { ((Test *)h)->send_path = 1; }

/* Message reçu de PiPedal : patch:Set (nouveau fichier) ou patch:Get. */
static void handle_object(Test *t, const LV2_Atom_Object *obj)
{
    if (obj->body.otype == t->u.patch_Get) { t->send_path = 1; return; }
    if (obj->body.otype != t->u.patch_Set) return;
    const LV2_Atom *prop = NULL, *val = NULL;
    lv2_atom_object_get(obj, t->u.patch_property, &prop, t->u.patch_value, &val, 0);
    if (!prop || prop->type != t->u.atom_URID || ((const LV2_Atom_URID *)prop)->body != t->u.file) return;
    if (!val || val->type != t->u.atom_Path) return;
    size_t n = val->size;                      /* copie sans allocation */
    if (n >= sizeof t->path) n = sizeof t->path - 1;
    memcpy(t->path, LV2_ATOM_BODY_CONST(val), n);
    t->path[n] = 0;
    t->send_path = 1;
}

static void run(LV2_Handle h, uint32_t n)
{
    Test *t = (Test *)h;
    /* Audio : tel quel. */
    const int nc = t->stereo ? 2 : 1;
    for (int ch = 0; ch < nc; ch++)
        if (t->out[ch] != t->in[ch]) memmove(t->out[ch], t->in[ch], n * sizeof(float));

    /* Messages entrants. */
    if (t->control)
        LV2_ATOM_SEQUENCE_FOREACH(t->control, ev)
            if (ev->body.type == t->u.atom_Object)
                handle_object(t, (const LV2_Atom_Object *)&ev->body);

    /* Contrôles -> sorties. */
    const float trig = t->c[C_TRIGGER] ? *t->c[C_TRIGGER] : 0;
    if (trig > 0.5f && t->last_trigger <= 0.5f)              /* front montant */
        t->count = t->count >= 9 ? 0 : t->count + 1;
    t->last_trigger = trig;
    t->frames += n;
    const double secs = t->frames / t->rate;
    if (t->c[C_LED]) *t->c[C_LED] = (t->c[C_TOGGLE] && *t->c[C_TOGGLE] > 0.5f) ? 1.0f : 0.0f;
    if (t->c[C_COUNT]) *t->c[C_COUNT] = t->count;
    if (t->c[C_BLINK]) *t->c[C_BLINK] = ((uint64_t)(secs * 2.0) & 1) ? 1.0f : 0.0f;
    if (t->c[C_STATE]) *t->c[C_STATE] = (float)(((uint64_t)(secs / 2.0)) % 3);
    if (t->c[C_NAMELEN]) *t->c[C_NAMELEN] = (float)strlen(t->path);
    if (t->c[C_DIROK]) *t->c[C_DIROK] = t->dir_ok;

    /* Réponse : chemin actuel (patch:Set), pour l'affichage de PiPedal. */
    if (t->notify) {
        const uint32_t cap = t->notify->atom.size;
        lv2_atom_forge_set_buffer(&t->forge, (uint8_t *)t->notify, cap);
        LV2_Atom_Forge_Frame seq;
        lv2_atom_forge_sequence_head(&t->forge, &seq, 0);
        if (t->send_path) {
            LV2_Atom_Forge_Frame obj;
            lv2_atom_forge_frame_time(&t->forge, 0);
            lv2_atom_forge_object(&t->forge, &obj, 0, t->u.patch_Set);
            lv2_atom_forge_key(&t->forge, t->u.patch_property);
            lv2_atom_forge_urid(&t->forge, t->u.file);
            lv2_atom_forge_key(&t->forge, t->u.patch_value);
            lv2_atom_forge_path(&t->forge, t->path, (uint32_t)strlen(t->path));
            lv2_atom_forge_pop(&t->forge, &obj);
            t->send_path = 0;
        }
        lv2_atom_forge_pop(&t->forge, &seq);
    }
}

static void cleanup(LV2_Handle h) { free(h); }

/* --- État (preset) : le chemin du fichier --- */
static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature *const *f)
{
    (void)flags; (void)f;
    Test *t = (Test *)h;
    return store(sh, t->u.file, t->path, strlen(t->path) + 1, t->u.atom_Path,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature *const *f)
{
    (void)flags; (void)f;
    Test *t = (Test *)h;
    size_t size;
    uint32_t type, fl;
    const char *v = (const char *)retrieve(sh, t->u.file, &size, &type, &fl);
    if (v && type == t->u.atom_Path) {
        size_t n = size < sizeof t->path ? size : sizeof t->path - 1;
        memcpy(t->path, v, n);
        t->path[n] = 0;
        t->send_path = 1;
    }
    return LV2_STATE_SUCCESS;
}

static const void *extension_data(const char *uri)
{
    static const LV2_State_Interface state = { save, restore };
    return !strcmp(uri, LV2_STATE__interface) ? &state : NULL;
}

static const LV2_Descriptor d_mono = {
    URI_MONO, inst_mono, connect_port, activate, run, NULL, cleanup, extension_data };
static const LV2_Descriptor d_stereo = {
    URI_STEREO, inst_stereo, connect_port, activate, run, NULL, cleanup, extension_data };

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t i)
{
    return i == 0 ? &d_mono : i == 1 ? &d_stereo : NULL;
}
