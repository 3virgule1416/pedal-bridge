/*
 * test_drum : charge pedal_bridge.so (dlopen) et vérifie DLS Drum (mono et
 * stéréo). Le test joue le rôle du client : écrivain du tampon L2-return et
 * état dans le fichier de contrôle.
 *
 *   test_drum <chemin/pedal_bridge.so> <dossier du bundle> [--rt-loop]
 */
#define _GNU_SOURCE
#include "pb_ctl.h"
#include "pb_shm.h"
#include "test_util.h"
#include "test_alloc.h"

#include <dlfcn.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK  128
#define RATE   48000.0
#define TARGET 256u

/* Ports de contrôle (voir lv2/drum.c), après les ports audio. */
enum {
    /* Song : chaque bouton à côté de sa liste */
    C_KIT = 0, C_INTRO, C_INTRO_BTN, C_INTRO_NEXT, C_OUTRO, C_OUTRO_BTN, C_OUTRO_NEXT,
    /* Browse */
    C_STYLE_PREV, C_STYLE_NEXT, C_TYPE_PREV, C_TYPE_NEXT, C_BEAT_PREV, C_BEAT_NEXT, O_STYLE, O_TYPE,
    /* Parts */
    C_PART, C_GROOVE /* Variation */, C_AUTOFILL, O_PART, O_GROOVE /* variation jouée */,
    /* Pedals */
    C_PLAYSTOP, C_PART_PREV, C_PART_NEXT, C_PREV, C_NEXT, C_FILL,
    /* Tempo */
    C_TEMPO, C_BPM_DOWN, C_BPM_UP, C_TAP, C_METRO, O_BEAT, O_PULSE, O_ONE, O_ROLLING, O_TEMPO,
    C_GAIN, O_LINK,
    C_NUM,
    A_CONTROL = C_NUM, A_NOTIFY            /* ports atom du sélecteur de fichier */
};
static const int BTN_PORT[PB_DBTN_COUNT] = { C_PLAYSTOP, C_PREV, C_NEXT, C_TAP, C_BPM_UP, C_BPM_DOWN, C_FILL,
                                             C_PART_PREV, C_PART_NEXT, C_OUTRO_BTN, C_INTRO_BTN,
                                             C_INTRO_NEXT, C_OUTRO_NEXT, C_STYLE_PREV, C_STYLE_NEXT,
                                             C_TYPE_PREV, C_TYPE_NEXT, C_BEAT_PREV, C_BEAT_NEXT };

typedef struct {
    const LV2_Descriptor *d;
    LV2_Handle h;
    int stereo;
    float in[2][BLOCK], out[2][BLOCK];
    float c[C_NUM];
} Host;

static float IN(uint64_t i, int c) { return 0.3f * sinf((float)(i % 48000) * (c ? 0.031f : 0.017f)); }
static float X(uint64_t f, int c)  { return 0.5f * sinf((float)(f % 100000) * (c ? 0.013f : 0.0091f)); }
static uint64_t g_in_frame;
static float g_last_in[2][BLOCK];

static int host_init(Host *t, const LV2_Descriptor *d, const char *bundle, int stereo)
{
    memset(t, 0, sizeof *t);
    t->d = d;
    t->stereo = stereo;
    t->c[C_GROOVE] = 1;
    t->c[C_TEMPO] = 120;
    static const LV2_Feature *none[] = { NULL };
    t->h = d->instantiate(d, RATE, bundle, none);
    if (!t->h) return -1;
    const uint32_t na = stereo ? 4 : 2, half = na / 2;
    for (uint32_t c = 0; c < half; c++) {
        d->connect_port(t->h, c, t->in[c]);
        d->connect_port(t->h, half + c, t->out[c]);
    }
    for (uint32_t k = 0; k < C_NUM; k++) d->connect_port(t->h, na + k, &t->c[k]);
    return 0;
}

/* --- Carte d'URID minimale (pour le sélecteur de fichier) --- */
static char g_uris[64][128];
static uint32_t g_nuris;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    for (uint32_t i = 0; i < g_nuris; i++) if (!strcmp(g_uris[i], uri)) return i + 1;
    if (g_nuris >= 64) return 0;
    snprintf(g_uris[g_nuris], sizeof g_uris[0], "%s", uri);
    return ++g_nuris;
}
static LV2_URID_Map g_map = { NULL, map_uri };

static void run1(Host *t)
{
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < 2; c++) t->in[c][i] = g_last_in[c][i] = IN(g_in_frame + (uint64_t)i, c);
    g_in_frame += BLOCK;
    g_in_rt = 1;
    t->d->run(t->h, BLOCK);
    g_in_rt = 0;
}

static int check_dry(const Host *t)
{
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < (t->stereo ? 2 : 1); c++)
            if (t->out[c][i] != g_last_in[c][i]) return 0;
    return 1;
}

static int check_mix(const Host *t, uint64_t f0, float g)
{
    for (int i = 0; i < BLOCK; i++) {
        const uint64_t f = f0 + (uint64_t)i;
        if (t->stereo) {
            for (int c = 0; c < 2; c++)
                if (fabsf(t->out[c][i] - (g_last_in[c][i] + X(f, c) * g)) > 2e-6f) return 0;
        } else if (fabsf(t->out[0][i] - (g_last_in[0][i] + 0.5f * (X(f, 0) + X(f, 1)) * g)) > 2e-6f) {
            return 0;
        }
    }
    return 1;
}

typedef struct { pb_link ret; pb_ctl *ctl; uint64_t me; } Client;

static void cl_beat(Client *k)
{
    pb_writer_beat(&k->ret, BLOCK);
    PB_STORE_REL(&k->ctl->client_heartbeat, k->ctl->client_heartbeat + 1u);
}

static void cl_write(Client *k, uint32_t n)
{
    pb_span s;
    uint64_t f = PB_LOAD_RLX(&k->ret.hdr->write_idx);
    uint32_t got = pb_write_reserve(&k->ret, n, &s);
    for (uint32_t i = 0; i < got; i++, f++) {
        float *p = i < s.n1 ? s.p1 + 2 * i : s.p2 + 2 * (i - s.n1);
        p[0] = X(f, 0); p[1] = X(f, 1);
    }
    pb_write_commit(&k->ret, got);
}

static void press(Host *t, int port)
{
    t->c[port] = 1; run1(t);
    t->c[port] = 0; run1(t);
}

static void rm_ctl(const char *dir)
{
    char path[256];
    snprintf(path, sizeof path, "%s/pedal-bridge-control", dir);
    unlink(path);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage : test_drum pedal_bridge.so bundle/ [--rt-loop]\n"); return 2; }
    const int rt_loop = argc > 3 && !strcmp(argv[3], "--rt-loop");

    char dir[] = "/tmp/pb-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("PB_SHM_DIR", dir, 1);
    umask(022);

    void *so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
    const LV2_Descriptor *dm = fn ? fn(4) : NULL, *ds = fn ? fn(5) : NULL;
    T_CHECK(dm && !strcmp(dm->URI, "http://pedal-bridge.local/plugins/drum-mono"), "descripteur 4 = Drum mono");
    T_CHECK(ds && !strcmp(ds->URI, "http://pedal-bridge.local/plugins/drum-stereo"), "descripteur 5 = Drum stéréo");
    if (!dm || !ds) return 1;

    Host A;
    T_CHECK(host_init(&A, dm, argv[2], 0) == 0, "instanciation (mono)");
    Client K;
    char err[256];
    K.me = pb_new_owner_id();
    T_CHECK(pb_link_open(&K.ret, 2, PB_DIR_RETURN, 0, err, sizeof err) == 0, "client : L2-return ouvert");
    T_CHECK(pb_ctl_open(&K.ctl, 0, err, sizeof err) == 0, "client : contrôle ouvert sans création");

    if (rt_loop) {
        dm->activate(A.h);
        pb_writer_take(&K.ret, K.me, 48000); pb_writer_set_target(&K.ret, TARGET);
        K.ctl->h2_ok = 1;
        run1(&A);
        g_rt_allocs = 0;
        (void)getppid();                       /* marqueur début */
        for (int k = 0; k < 3000; k++) {
            if (k % 500 < 450) cl_write(&K, BLOCK);
            cl_beat(&K);
            K.ctl->rolling = (k / 100) & 1; K.ctl->beat = 1 + (k / 25) % 4;
            K.ctl->beat_frac = (uint32_t)(k * 997) & 0xffff; K.ctl->groove_cur = (uint32_t)(k / 300);
            A.c[BTN_PORT[(k / 10) % PB_DBTN_COUNT]] = (k % 10) < 5;
            if (k == 1000) A.c[C_GAIN] = -6.0f;
            if (k == 1500) A.c[C_GROOVE] = 4;
            if (k == 2000) A.c[C_TEMPO] = 90;
            run1(&A);
        }
        (void)getppid();                       /* marqueur fin */
        printf("allocations pendant run() : %ld\n", g_rt_allocs);
        dm->deactivate(A.h);
        dm->cleanup(A.h);
        pb_ctl_close(K.ctl);
        rm_ctl(dir);
        t_cleanup_dir(dir);
        return g_rt_allocs ? 1 : 0;
    }

    dm->activate(A.h);

    /* --- 1. Sans client --- */
    int ok = 1;
    for (int k = 0; k < 5; k++) { run1(&A); ok &= check_dry(&A); }
    T_CHECK(ok, "sans client : sortie identique à l'entrée");
    T_CHECK(A.c[O_LINK] == 0 && A.c[O_GROOVE] == 0, "sans client : Link « no bridge », groove « - »");
    T_CHECK(K.ctl->drum_owner != 0 && K.ctl->drum_groove == 1 && K.ctl->drum_tempo == 12000,
            "le plugin publie : propriétaire, groove 1, tempo 120");
    T_CHECK(K.ctl->looper_owner == 0, "la section du Looper n'est pas touchée");

    /* --- 2. Client présent : batterie ajoutée --- */
    pb_writer_take(&K.ret, K.me, 48000);
    pb_writer_set_target(&K.ret, TARGET);
    K.ctl->h2_ok = 1; K.ctl->groove_cur = 3;
    cl_write(&K, 512); cl_beat(&K); run1(&A);
    ok = 1;
    for (int k = 0; k < 10; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&A);
        if (k > 0) ok &= check_mix(&A, f0, 1.0f);
    }
    T_CHECK(ok, "mono : sortie = entrée + (G + D) / 2 de la batterie");
    T_CHECK(A.c[O_LINK] == 2 && A.c[O_GROOVE] == 3, "client présent : Link « OK », groove joué 3");
    K.ctl->h2_ok = 0; cl_write(&K, BLOCK); cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_GROOVE] == 0, "Hydrogen absent : groove « - »");
    K.ctl->h2_ok = 1;

    A.c[C_GAIN] = -6.0206f;
    cl_write(&K, BLOCK); cl_beat(&K); run1(&A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&A); ok &= check_mix(&A, f0, 0.5f);
    }
    T_CHECK(ok, "Volume -6 dB : seulement sur la batterie");
    A.c[C_GAIN] = 0;

    /* --- 3. Boutons, groove, tempo --- */
    uint32_t b0[PB_DBTN_COUNT];
    for (int b = 0; b < PB_DBTN_COUNT; b++) b0[b] = *pb_drum_btn(K.ctl, b);
    press(&A, C_NEXT); press(&A, C_NEXT); press(&A, C_PREV);
    A.c[C_TAP] = 1; for (int k = 0; k < 5; k++) run1(&A); A.c[C_TAP] = 0; run1(&A);
    press(&A, C_PLAYSTOP);
    T_CHECK(K.ctl->drum_btn[PB_DBTN_NEXT] == b0[PB_DBTN_NEXT] + 2 && K.ctl->drum_btn[PB_DBTN_PREV] == b0[PB_DBTN_PREV] + 1,
            "Next ×2, Previous ×1 : compteurs");
    T_CHECK(K.ctl->drum_btn[PB_DBTN_TAP] == b0[PB_DBTN_TAP] + 1, "Tap tenu 5 blocs : compté une fois");
    T_CHECK(K.ctl->drum_btn[PB_DBTN_PLAYSTOP] == b0[PB_DBTN_PLAYSTOP] + 1, "Play/Stop : +1");
    T_CHECK(K.ctl->looper_btn[PB_BTN_PLAYSTOP] == 0, "Play/Stop de Drum : compteur du Looper intact");
    A.c[C_GROOVE] = 7; A.c[C_TEMPO] = 96; run1(&A);
    T_CHECK(K.ctl->drum_groove == 7 && K.ctl->drum_tempo == 9600, "groove 7 et tempo 96 publiés");
    T_CHECK(K.ctl->drum_metro == 0, "Metronome éteint par défaut");
    A.c[C_METRO] = 1; run1(&A);
    T_CHECK(K.ctl->drum_metro == 1, "Metronome allumé publié");
    A.c[C_METRO] = 0; run1(&A);
    T_CHECK(K.ctl->drum_metro == 0, "Metronome éteint publié");

    /* BPM + / BPM - : un pas à l'appui, répétition quand on tient. */
    uint32_t u0 = K.ctl->drum_btn[PB_DBTN_BPM_UP], d0 = K.ctl->drum_btn[PB_DBTN_BPM_DOWN];
    press(&A, C_BPM_UP);
    T_CHECK(K.ctl->drum_btn[PB_DBTN_BPM_UP] == u0 + 1, "BPM + appuyé : 1 pas");
    A.c[C_BPM_DOWN] = 1; for (int k = 0; k < 112; k++) run1(&A); A.c[C_BPM_DOWN] = 0; run1(&A);  /* 0,3 s */
    T_CHECK(K.ctl->drum_btn[PB_DBTN_BPM_DOWN] == d0 + 1, "BPM - tenu 0,3 s : 1 pas (pas encore de répétition)");
    u0 = K.ctl->drum_btn[PB_DBTN_BPM_UP];
    A.c[C_BPM_UP] = 1; for (int k = 0; k < 375; k++) run1(&A); A.c[C_BPM_UP] = 0; run1(&A);    /* 1 s */
    uint32_t du = K.ctl->drum_btn[PB_DBTN_BPM_UP] - u0;
    printf("BPM + tenu 1 s : %u pas\n", du);
    T_CHECK(du >= 6 && du <= 8, "BPM + tenu 1 s : 6 à 8 pas (répétition toutes les 120 ms)");
    u0 = K.ctl->drum_btn[PB_DBTN_BPM_UP];
    A.c[C_BPM_UP] = 1; for (int k = 0; k < 1125; k++) run1(&A); A.c[C_BPM_UP] = 0; run1(&A);   /* 3 s */
    du = K.ctl->drum_btn[PB_DBTN_BPM_UP] - u0;
    printf("BPM + tenu 3 s : %u pas\n", du);
    T_CHECK(du >= 35 && du <= 45, "BPM + tenu 3 s : plus vite après 2 s (35 à 45 pas)");

    /* --- 4. Transport --- */
    K.ctl->rolling = 1; K.ctl->beat = 1; K.ctl->beat_frac = 100; K.ctl->bpm = 9600;
    cl_write(&K, BLOCK); cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_ROLLING] == 1 && A.c[O_BEAT] == 1 && A.c[O_PULSE] == 1 && A.c[O_ONE] == 1 && A.c[O_TEMPO] == 96,
            "temps 1 : voyants ♩ et 1, BPM 96");
    K.ctl->beat = 3; cl_write(&K, BLOCK); cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_BEAT] == 3 && A.c[O_ONE] == 0, "temps 3 : pas de voyant 1");

    /* --- 5. Deux instances --- */
    Host B;
    T_CHECK(host_init(&B, ds, argv[2], 1) == 0, "2e instance (stéréo)");
    ds->activate(B.h);
    run1(&B);
    uint32_t n0 = K.ctl->drum_btn[PB_DBTN_NEXT];
    press(&A, C_NEXT);
    T_CHECK(K.ctl->drum_btn[PB_DBTN_NEXT] == n0, "l'ancienne instance ne commande plus");
    press(&B, C_NEXT);
    T_CHECK(K.ctl->drum_btn[PB_DBTN_NEXT] == n0 + 1, "la nouvelle instance commande");
    cl_write(&K, 512); cl_beat(&K); run1(&B);
    ok = 1;
    for (int k = 0; k < 6; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&B);
        if (k > 0) ok &= check_mix(&B, f0, 1.0f);
    }
    T_CHECK(ok, "stéréo : sortie = entrée + batterie, canal par canal");
    ds->deactivate(B.h);
    T_CHECK(K.ctl->drum_owner == 0, "deactivate libère la commande");
    ds->cleanup(B.h);

    T_CHECK(g_rt_allocs == 0, "aucune allocation pendant run()");
    dm->deactivate(A.h);
    dm->cleanup(A.h);
    /* --- 6. Morceau (sélecteur de fichier, patch:Set) --- */
    {
        const LV2_Feature fmap = { LV2_URID__map, &g_map };
        const LV2_Feature *feats[] = { &fmap, NULL };
        Host S;
        memset(&S, 0, sizeof S);
        S.d = dm; S.c[C_GROOVE] = 1; S.c[C_TEMPO] = 120;
        S.h = dm->instantiate(dm, RATE, argv[2], feats);
        T_CHECK(S.h != NULL, "instanciation avec urid:map");
        dm->connect_port(S.h, 0, S.in[0]);
        dm->connect_port(S.h, 1, S.out[0]);
        for (uint32_t k = 0; k < C_NUM; k++) dm->connect_port(S.h, 2 + k, &S.c[k]);
        static uint64_t ctl_buf[512], notify_buf[512];   /* alignés sur 8 octets */
        dm->connect_port(S.h, 2 + A_CONTROL, ctl_buf);
        dm->connect_port(S.h, 2 + A_NOTIFY, notify_buf);
        dm->activate(S.h);

        const char *song = "/var/pipedal/audio_uploads/DLS Drums/Rock.h2song";
        LV2_Atom_Forge forge;
        lv2_atom_forge_init(&forge, &g_map);
        lv2_atom_forge_set_buffer(&forge, (uint8_t *)ctl_buf, sizeof ctl_buf);
        LV2_Atom_Forge_Frame seq, obj;
        lv2_atom_forge_sequence_head(&forge, &seq, 0);
        lv2_atom_forge_frame_time(&forge, 0);
        lv2_atom_forge_object(&forge, &obj, 0, map_uri(NULL, LV2_PATCH__Set));
        lv2_atom_forge_key(&forge, map_uri(NULL, LV2_PATCH__property));
        lv2_atom_forge_urid(&forge, map_uri(NULL, "http://pedal-bridge.local/plugins/drum#song"));
        lv2_atom_forge_key(&forge, map_uri(NULL, LV2_PATCH__value));
        lv2_atom_forge_path(&forge, song, (uint32_t)strlen(song));
        lv2_atom_forge_pop(&forge, &obj);
        lv2_atom_forge_pop(&forge, &seq);

        { const uint32_t cap = sizeof notify_buf - sizeof(LV2_Atom); memcpy(notify_buf, &cap, sizeof cap); }   /* atom.size */
        const uint32_t s0 = K.ctl->drum_song_seq;
        run1(&S);
        T_CHECK(!strcmp(K.ctl->drum_song, song), "morceau choisi -> fichier de contrôle");
        T_CHECK(K.ctl->drum_song_seq != s0 && (K.ctl->drum_song_seq & 1u) == 0, "numéro de séquence pair et changé");
        uint32_t nsize;
        memcpy(&nsize, notify_buf, sizeof nsize);
        T_CHECK(nsize > sizeof(LV2_Atom_Sequence_Body), "chemin renvoyé à PiPedal (notify)");

        /* Séquence vide au bloc suivant : rien ne change. */
        lv2_atom_forge_set_buffer(&forge, (uint8_t *)ctl_buf, sizeof ctl_buf);
        lv2_atom_forge_sequence_head(&forge, &seq, 0);
        lv2_atom_forge_pop(&forge, &seq);
        { const uint32_t cap = sizeof notify_buf - sizeof(LV2_Atom); memcpy(notify_buf, &cap, sizeof cap); }   /* atom.size */
        const uint32_t s1 = K.ctl->drum_song_seq;
        run1(&S);
        T_CHECK(K.ctl->drum_song_seq == s1, "pas de nouveau morceau : fichier de contrôle inchangé");

        /* Liste Kit : numéro publié (1..N), le morceau ne bouge pas. */
        S.c[C_KIT] = 2; run1(&S);
        T_CHECK(K.ctl->drum_kit_num == 3, "liste Kit 2 -> kit n° 3 publié");
        T_CHECK(!strcmp(K.ctl->drum_song, song), "le morceau reste celui choisi");
        dm->deactivate(S.h);
        dm->cleanup(S.h);
    }

    pb_link_close(&K.ret);
    pb_ctl_close(K.ctl);
    rm_ctl(dir);
    t_cleanup_dir(dir);
    return t_summary("test_drum");
}
