/*
 * test_looper : charge pedal_bridge.so (dlopen) et vérifie DLS Looper (mono
 * et stéréo). Le test joue le rôle du client pedal_bridge : lecteur du tampon
 * L1-send, écrivain du tampon L1-return, et état dans le fichier de contrôle.
 *
 *   test_looper <chemin/pedal_bridge.so> <dossier du bundle> [--rt-loop]
 */
#define _GNU_SOURCE
#include "pb_ctl.h"
#include "pb_shm.h"
#include "test_util.h"
#include "test_alloc.h"

#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK  128
#define RATE   48000.0
#define TARGET 256u

/* Ports de contrôle (voir lv2/looper.c), après les ports audio. */
enum {
    C_LOOP = 0, C_RECORD, C_OVERDUB, C_UNDO, C_REDO, C_MUTE, C_CLEAR, C_PLAYSTOP,
    C_TEMPO, C_GAIN,
    O_STATE, O_BEAT, O_PULSE, O_ONE, O_ROLLING, O_TEMPO, O_BRIDGE, O_AUDIO,
    C_NUM
};

typedef struct {
    const LV2_Descriptor *d;
    LV2_Handle h;
    int stereo, inplace;
    float in[2][BLOCK], out[2][BLOCK];
    float c[C_NUM];
} Host;

static float IN(uint64_t i, int c) { return 0.3f * sinf((float)(i % 48000) * (c ? 0.031f : 0.017f)); }
static float X(uint64_t f, int c)  { return 0.5f * sinf((float)(f % 100000) * (c ? 0.013f : 0.0091f)); }

static void host_connect(Host *t, int inplace)
{
    const uint32_t na = t->stereo ? 4 : 2, half = na / 2;
    t->inplace = inplace;
    for (uint32_t c = 0; c < half; c++) {
        t->d->connect_port(t->h, c, t->in[c]);
        t->d->connect_port(t->h, half + c, inplace ? t->in[c] : t->out[c]);
    }
    for (uint32_t k = 0; k < C_NUM; k++) t->d->connect_port(t->h, na + k, &t->c[k]);
}

static int host_init(Host *t, const LV2_Descriptor *d, const char *bundle, int stereo)
{
    memset(t, 0, sizeof *t);
    t->d = d;
    t->stereo = stereo;
    t->c[C_LOOP] = 1;
    t->c[C_TEMPO] = 120;
    static const LV2_Feature *none[] = { NULL };
    t->h = d->instantiate(d, RATE, bundle, none);
    if (!t->h) return -1;
    host_connect(t, 0);
    return 0;
}

static uint64_t g_in_frame;   /* compteur du signal d'entrée */
static float g_sent_in[2][BLOCK];

static const float *O(const Host *t, int c) { return t->inplace ? t->in[c] : t->out[c]; }

static void run1(Host *t)
{
    const int nch = t->stereo ? 2 : 1;
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < nch; c++) t->in[c][i] = g_sent_in[c][i] = IN(g_in_frame + (uint64_t)i, c);
    g_in_frame += BLOCK;
    g_in_rt = 1;
    t->d->run(t->h, BLOCK);
    g_in_rt = 0;
}

/* --- Faux client --- */
typedef struct {
    pb_link snd, ret;
    pb_ctl *ctl;
    uint64_t me;
} Client;

static void cl_beat(Client *k)
{
    pb_reader_beat(&k->snd, BLOCK);
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

/* Lit le dernier bloc envoyé par le plugin et le compare à l'entrée. */
static int cl_check_sent(Client *k, const Host *t)
{
    if (pb_read_available(&k->snd) < BLOCK) return 0;
    pb_reader_resync(&k->snd, BLOCK);
    pb_span s;
    uint32_t got = pb_read_reserve(&k->snd, BLOCK, &s);
    int ok = got == BLOCK;
    for (uint32_t i = 0; i < got && ok; i++) {
        const float *p = i < s.n1 ? s.p1 + 2 * i : s.p2 + 2 * (i - s.n1);
        const float l = g_sent_in[0][i], r = t->stereo ? g_sent_in[1][i] : g_sent_in[0][i];
        if (p[0] != l || p[1] != r) ok = 0;
    }
    pb_read_commit(&k->snd, got);
    return ok;
}

/* Sortie == entrée + retour × g (mono : moyenne G/D du retour). */
static int check_mix(const Host *t, uint64_t f0, float g)
{
    for (int i = 0; i < BLOCK; i++) {
        if (t->stereo) {
            for (int c = 0; c < 2; c++) {
                float want = g_sent_in[c][i] + X(f0 + (uint64_t)i, c) * g;
                if (fabsf(O(t, c)[i] - want) > 2e-6f) return 0;
            }
        } else {
            float want = g_sent_in[0][i] + 0.5f * (X(f0 + (uint64_t)i, 0) + X(f0 + (uint64_t)i, 1)) * g;
            if (fabsf(O(t, 0)[i] - want) > 2e-6f) { fprintf(stderr, "  i=%d : %g au lieu de %g\n", i, O(t, 0)[i], want); return 0; }
        }
    }
    return 1;
}

static int check_dry(const Host *t)
{
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < (t->stereo ? 2 : 1); c++)
            if (O(t, c)[i] != g_sent_in[c][i]) return 0;
    return 1;
}

static void press(Host *t, int btn)
{
    t->c[btn] = 1; run1(t);
    t->c[btn] = 0; run1(t);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage : test_looper pedal_bridge.so bundle/ [--rt-loop]\n"); return 2; }
    const int rt_loop = argc > 3 && !strcmp(argv[3], "--rt-loop");

    char dir[] = "/tmp/pb-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("PB_SHM_DIR", dir, 1);
    umask(022);

    void *so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
    const LV2_Descriptor *dm = fn ? fn(2) : NULL, *ds = fn ? fn(3) : NULL;
    T_CHECK(dm && !strcmp(dm->URI, "http://pedal-bridge.local/plugins/looper-mono"), "descripteur 2 = Looper mono");
    T_CHECK(ds && !strcmp(ds->URI, "http://pedal-bridge.local/plugins/looper-stereo"), "descripteur 3 = Looper stéréo");
    if (!dm || !ds) return 1;

    Host A;
    T_CHECK(host_init(&A, dm, argv[2], 0) == 0, "instanciation (mono)");
    char path[256], err[256];
    struct stat st;
    snprintf(path, sizeof path, "%s/pedal-bridge-control", dir);
    T_CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0666 && st.st_size == PB_CTL_BYTES,
            "fichier de contrôle créé en 0666, 4096 octets");

    Client K;
    K.me = pb_new_owner_id();
    T_CHECK(pb_link_open(&K.snd, 1, PB_DIR_SEND, 0, err, sizeof err) == 0, "client : L1-send ouvert");
    T_CHECK(pb_link_open(&K.ret, 1, PB_DIR_RETURN, 0, err, sizeof err) == 0, "client : L1-return ouvert");
    T_CHECK(pb_ctl_open(&K.ctl, 0, err, sizeof err) == 0, "client : contrôle ouvert sans création");

    /* --- Mode RT pour strace --- */
    if (rt_loop) {
        dm->activate(A.h);
        pb_reader_take(&K.snd, K.me, 48000);
        pb_writer_take(&K.ret, K.me, 48000); pb_writer_set_target(&K.ret, TARGET);
        K.ctl->sl_ok = 1;
        run1(&A);
        g_rt_allocs = 0;
        (void)getppid();                       /* marqueur début */
        for (int k = 0; k < 3000; k++) {
            if (k % 500 < 450) cl_write(&K, BLOCK);   /* trous => sous-débits */
            pb_reader_resync(&K.snd, 0);
            cl_beat(&K);
            K.ctl->rolling = (k / 100) & 1; K.ctl->beat = 1 + (k / 25) % 4;
            K.ctl->beat_frac = (uint32_t)(k * 997) & 0xffff; K.ctl->bpm = 10000u + (uint32_t)(k % 7) * 100u;
            A.c[C_RECORD + (k / 10) % 7] = (k % 10) < 5;
            if (k == 1000) A.c[C_GAIN] = -6.0f;
            if (k == 1500) A.c[C_LOOP] = 4;
            if (k == 2000) A.c[C_TEMPO] = 90;
            run1(&A);
        }
        (void)getppid();                       /* marqueur fin */
        printf("allocations pendant run() : %ld\n", g_rt_allocs);
        dm->deactivate(A.h);
        dm->cleanup(A.h);
        pb_ctl_close(K.ctl);
        snprintf(path, sizeof path, "%s/pedal-bridge-control", dir); unlink(path);
        t_cleanup_dir(dir);
        return g_rt_allocs ? 1 : 0;
    }

    dm->activate(A.h);

    /* --- 1. Sans client --- */
    int ok = 1;
    for (int k = 0; k < 5; k++) { run1(&A); ok &= check_dry(&A); }
    T_CHECK(ok, "sans client : sortie identique à l'entrée");
    T_CHECK(A.c[O_BRIDGE] == 0 && A.c[O_STATE] == PB_LS_OFFLINE && A.c[O_AUDIO] == 0,
            "sans client : Pont 0, État « hors ligne », Audio 0");
    T_CHECK(K.ctl->looper_owner != 0 && K.ctl->looper_loop == 1 && K.ctl->looper_tempo == 12000,
            "le plugin publie : propriétaire, boucle 1, tempo 120");

    /* --- 2. Client présent : audio --- */
    pb_reader_take(&K.snd, K.me, 48000);
    pb_writer_take(&K.ret, K.me, 48000);
    pb_writer_set_target(&K.ret, TARGET);
    K.ctl->sl_ok = 1;
    K.ctl->loop_state[0] = PB_LS_PLAY;
    cl_write(&K, 512); cl_beat(&K);
    run1(&A);                                     /* 1er bloc : le plugin voit le client */
    pb_reader_resync(&K.snd, 0);
    ok = 1;
    int sent_ok = 1;
    for (int k = 0; k < 10; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&A);
        if (k > 0) ok &= check_mix(&A, f0, 1.0f);
        sent_ok &= cl_check_sent(&K, &A);
    }
    T_CHECK(sent_ok, "mono : l'entrée part vers le looper, la même à gauche et à droite");
    T_CHECK(ok, "mono : sortie = entrée + (G + D) / 2 du retour");
    T_CHECK(A.c[O_BRIDGE] == 1 && A.c[O_AUDIO] == 1 && A.c[O_STATE] == PB_LS_PLAY,
            "client présent : Pont 1, Audio 1, État « lecture »");

    /* En place : la copie envoyée est bien l'entrée SANS les boucles. */
    host_connect(&A, 1);
    ok = 1; sent_ok = 1;
    for (int k = 0; k < 3; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&A);
        ok &= check_mix(&A, f0, 1.0f);
        sent_ok &= cl_check_sent(&K, &A);
    }
    T_CHECK(ok && sent_ok, "en place : envoi = entrée seule, puis entrée + boucles");
    host_connect(&A, 0);

    /* Volume des boucles -6 dB */
    A.c[C_GAIN] = -6.0206f;
    cl_write(&K, BLOCK); cl_beat(&K); run1(&A); cl_check_sent(&K, &A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&A); ok &= check_mix(&A, f0, 0.5f); cl_check_sent(&K, &A);
    }
    T_CHECK(ok, "Volume boucles -6 dB : seulement sur les boucles");
    A.c[C_GAIN] = 0;

    /* --- 3. Boutons --- */
    uint32_t b0[PB_BTN_COUNT];
    for (int b = 0; b < PB_BTN_COUNT; b++) b0[b] = K.ctl->looper_btn[b];
    press(&A, C_RECORD);
    T_CHECK(K.ctl->looper_btn[PB_BTN_RECORD] == b0[PB_BTN_RECORD] + 1, "Record : compteur +1");
    A.c[C_UNDO] = 1; for (int k = 0; k < 5; k++) run1(&A); A.c[C_UNDO] = 0; run1(&A);
    T_CHECK(K.ctl->looper_btn[PB_BTN_UNDO] == b0[PB_BTN_UNDO] + 1, "Undo tenu 5 blocs : compté une seule fois");
    press(&A, C_PLAYSTOP); press(&A, C_PLAYSTOP);
    T_CHECK(K.ctl->looper_btn[PB_BTN_PLAYSTOP] == b0[PB_BTN_PLAYSTOP] + 2, "Play/Stop 2 appuis : +2");
    ok = 1;
    for (int b = 0; b < PB_BTN_COUNT; b++)
        if (b != PB_BTN_RECORD && b != PB_BTN_UNDO && b != PB_BTN_PLAYSTOP && K.ctl->looper_btn[b] != b0[b]) ok = 0;
    T_CHECK(ok, "les autres compteurs ne bougent pas");

    /* --- 4. Boucle choisie, tempo, état --- */
    A.c[C_LOOP] = 3; A.c[C_TEMPO] = 95;
    K.ctl->loop_state[2] = PB_LS_RECORD;
    cl_beat(&K); run1(&A);
    T_CHECK(K.ctl->looper_loop == 3 && K.ctl->looper_tempo == 9500, "boucle 3 et tempo 95 publiés");
    T_CHECK(A.c[O_STATE] == PB_LS_RECORD, "État = celui de la boucle 3 (« enregistre »)");
    K.ctl->sl_ok = 0; cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_STATE] == PB_LS_OFFLINE, "SooperLooper absent : « hors ligne »");
    K.ctl->sl_ok = 1;

    /* --- 5. Transport --- */
    K.ctl->rolling = 0; K.ctl->beat = 3; K.ctl->bpm = 9960;
    cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_ROLLING] == 0 && A.c[O_BEAT] == 0 && A.c[O_PULSE] == 0, "arrêté : temps « - », pas de pulsation");
    T_CHECK(A.c[O_TEMPO] == 100, "Tempo actuel arrondi : 99,6 -> 100");
    K.ctl->rolling = 1; K.ctl->beat = 1; K.ctl->beat_frac = 1000;
    cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_ROLLING] == 1 && A.c[O_BEAT] == 1 && A.c[O_PULSE] == 1 && A.c[O_ONE] == 1, "temps 1 : pulsation et « Sur le 1 »");
    K.ctl->beat = 2; cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_BEAT] == 2 && A.c[O_PULSE] == 1 && A.c[O_ONE] == 0, "temps 2 : pulsation seulement");
    K.ctl->beat_frac = 40000; cl_beat(&K); run1(&A);
    T_CHECK(A.c[O_PULSE] == 0, "milieu du temps : pulsation éteinte");

    /* --- 6. Client disparu --- */
    int k_off = 0;
    for (; k_off < 400 && A.c[O_BRIDGE] == 1; k_off++) run1(&A);
    T_CHECK(A.c[O_BRIDGE] == 0 && A.c[O_STATE] == PB_LS_OFFLINE && A.c[O_ROLLING] == 0,
            "client muet : Pont 0 après ~0,5 s, plus d'état");
    run1(&A);
    T_CHECK(check_dry(&A), "client muet : signal intact");

    /* --- 7. Deux instances : la dernière activée commande --- */
    Host B;
    T_CHECK(host_init(&B, ds, argv[2], 1) == 0, "2e instance (stéréo)");
    ds->activate(B.h);
    run1(&B);
    T_CHECK(K.ctl->looper_owner != 0, "propriétaire présent");
    uint32_t r0 = K.ctl->looper_btn[PB_BTN_RECORD];
    press(&A, C_RECORD);
    T_CHECK(K.ctl->looper_btn[PB_BTN_RECORD] == r0, "l'ancienne instance ne commande plus");
    press(&B, C_RECORD);
    T_CHECK(K.ctl->looper_btn[PB_BTN_RECORD] == r0 + 1, "la nouvelle instance commande");

    /* Stéréo : audio */
    cl_write(&K, 512); cl_beat(&K); run1(&B); run1(&A);
    pb_reader_resync(&K.snd, 0);
    ok = 1; sent_ok = 1;
    for (int k = 0; k < 6; k++) {
        cl_write(&K, BLOCK); cl_beat(&K);
        uint64_t f0 = PB_LOAD_ACQ(&K.ret.hdr->read_idx);
        run1(&B);
        if (k > 0) ok &= check_mix(&B, f0, 1.0f);
        sent_ok &= cl_check_sent(&K, &B);
        run1(&A);                                 /* l'ancienne ne lit ni n'écrit plus */
    }
    T_CHECK(sent_ok, "stéréo : G et D envoyés tels quels");
    T_CHECK(ok, "stéréo : sortie = entrée + retour, canal par canal");
    T_CHECK(check_dry(&A), "l'ancienne instance laisse passer son signal sans boucles");

    ds->deactivate(B.h);
    T_CHECK(K.ctl->looper_owner == 0, "deactivate libère la commande");
    ds->cleanup(B.h);

    T_CHECK(g_rt_allocs == 0, "aucune allocation pendant run()");
    dm->deactivate(A.h);
    dm->cleanup(A.h);

    /* --- 8. Dossier inaccessible --- */
    setenv("PB_SHM_DIR", "/nonexistent-pedal-bridge", 1);
    Host C;
    T_CHECK(host_init(&C, dm, argv[2], 0) == 0, "instanciation sans dossier partagé");
    dm->activate(C.h);
    press(&C, C_RECORD);
    run1(&C);
    T_CHECK(check_dry(&C) && C.c[O_BRIDGE] == 0, "sans dossier : signal intact, Pont 0");
    dm->deactivate(C.h);
    dm->cleanup(C.h);

    pb_link_close(&K.snd);
    pb_link_close(&K.ret);
    pb_ctl_close(K.ctl);
    snprintf(path, sizeof path, "%s/pedal-bridge-control", dir); unlink(path);
    t_cleanup_dir(dir);
    return t_summary("test_looper");
}
