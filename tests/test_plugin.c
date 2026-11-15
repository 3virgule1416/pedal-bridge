/*
 * test_plugin : charge pedal_bridge.so comme le ferait un hôte LV2 (dlopen,
 * sans lilv), et vérifie le comportement du Send.
 *
 *   test_plugin <chemin/pedal_bridge.so> <dossier du bundle>
 *   test_plugin <...> <...> --rt-loop   boucle de run() encadrée de deux
 *                                       getppid() pour vérification par strace
 *
 * Compte aussi les allocations (malloc & cie interceptés) pendant run().
 */
#define _GNU_SOURCE
#include "pb_shm.h"
#include "test_util.h"

#include <dlfcn.h>
#include <errno.h>
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_alloc.h"

/* ---- Petit hôte ---- */
#define BLOCK 128
#define RATE  48000.0

enum { P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_LINK, P_GAIN, P_CONNECTED };

typedef struct {
    const LV2_Descriptor *d;
    LV2_Handle h;
    float in[2][BLOCK], out[2][BLOCK];
    float link, gain, connected;
    uint64_t frame;   /* compteur de trames du signal de test */
} Host;

static float sig(uint64_t i, int c) { return 0.5f * sinf((float)(i % 48000) * (c ? 0.0731f : 0.0523f)); }

static void host_connect(Host *t, int inplace)
{
    const LV2_Descriptor *d = t->d;
    d->connect_port(t->h, P_IN_L, t->in[0]);
    d->connect_port(t->h, P_IN_R, t->in[1]);
    d->connect_port(t->h, P_OUT_L, inplace ? t->in[0] : t->out[0]);
    d->connect_port(t->h, P_OUT_R, inplace ? t->in[1] : t->out[1]);
    d->connect_port(t->h, P_LINK, &t->link);
    d->connect_port(t->h, P_GAIN, &t->gain);
    d->connect_port(t->h, P_CONNECTED, &t->connected);
}

static int host_init(Host *t, const LV2_Descriptor *d, const char *bundle)
{
    memset(t, 0, sizeof *t);
    t->d = d;
    t->link = 1; t->gain = 0;
    static const LV2_Feature *none[] = { NULL };
    t->h = d->instantiate(d, RATE, bundle, none);
    if (!t->h) return -1;
    host_connect(t, 0);
    return 0;
}

/* Remplit l'entrée, lance run(), retourne 1 si la sortie == l'entrée d'origine. */
static int host_run(Host *t, int inplace)
{
    float ref[2][BLOCK];
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < 2; c++)
            ref[c][i] = t->in[c][i] = sig(t->frame + (uint64_t)i, c);
    t->frame += BLOCK;
    g_in_rt = 1;
    t->d->run(t->h, BLOCK);
    g_in_rt = 0;
    const float *o0 = inplace ? t->in[0] : t->out[0], *o1 = inplace ? t->in[1] : t->out[1];
    return !memcmp(o0, ref[0], sizeof ref[0]) && !memcmp(o1, ref[1], sizeof ref[1]);
}

/* Lit toutes les trames disponibles ; vérifie qu'elles valent signal*gain et se
 * terminent à la trame t->frame. Retourne le nombre lu, -1 si contenu faux. */
static long drain_check(pb_link *l, const Host *t, float gain)
{
    uint32_t n = pb_read_available(l);
    pb_span s;
    pb_read_reserve(l, n, &s);
    uint64_t f = t->frame - n;
    for (int seg = 0; seg < 2; seg++) {
        const float *p = seg ? s.p2 : s.p1;
        uint32_t m = seg ? s.n2 : s.n1;
        for (uint32_t k = 0; k < m; k++, f++)
            for (int c = 0; c < 2; c++)
                if (fabsf(p[2*k+c] - sig(f, c) * gain) > 1e-6f) {
                    fprintf(stderr, "  trame %llu c%d : %g au lieu de %g\n",
                            (unsigned long long)f, c, p[2*k+c], sig(f, c) * gain);
                    pb_read_commit(l, n);
                    return -1;
                }
    }
    pb_read_commit(l, n);
    return (long)n;
}

static uint64_t W(pb_link *l) { return PB_LOAD_ACQ(&l->hdr->write_idx); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage : test_plugin pedal_bridge.so bundle/ [--rt-loop]\n"); return 2; }
    int rt_loop = argc > 3 && !strcmp(argv[3], "--rt-loop");

    char dir[] = "/tmp/pb-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("PB_SHM_DIR", dir, 1);
    umask(022);

    void *so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
    T_CHECK(fn != NULL, "symbole lv2_descriptor exporté");
    if (!fn) return 1;
    const LV2_Descriptor *d = fn(0);
    T_CHECK(d && !strcmp(d->URI, "http://pedal-bridge.local/plugins/send"), "descripteur 0 = Send");

    Host A;
    T_CHECK(host_init(&A, d, argv[2]) == 0, "instanciation");
    struct stat st;
    char path[256];
    int all = 1;
    for (uint32_t i = 1; i <= PB_MAX_LINKS; i++) {
        pb_link_path(path, sizeof path, i, PB_DIR_SEND);
        if (stat(path, &st) != 0 || (st.st_mode & 0777) != 0666) all = 0;
    }
    T_CHECK(all, "8 fichiers Send créés en 0666");

    pb_link r2, r5;
    char err[256];
    T_CHECK(pb_link_open(&r2, 2, PB_DIR_SEND, 0, err, sizeof err) == 0, "lecteur ouvre le lien 2");
    T_CHECK(pb_link_open(&r5, 5, PB_DIR_SEND, 0, err, sizeof err) == 0, "lecteur ouvre le lien 5");

    /* --- Mode RT pour strace : lecteur simulé dans le même thread --- */
    if (rt_loop) {
        A.link = 2;
        d->activate(A.h);
        uint64_t me = pb_new_owner_id();
        pb_reader_take(&r2, me, 48000);
        host_run(&A, 0);                      /* premier run hors marqueurs */
        g_rt_allocs = 0;
        (void)getppid();                      /* marqueur début */
        for (int k = 0; k < 2000; k++) {
            pb_reader_beat(&r2, BLOCK);
            if (k == 1000) A.gain = -6.0f;    /* changement de gain pendant la boucle */
            if (k == 1500) A.link = 5;        /* changement de lien pendant la boucle */
            host_run(&A, 0);
            pb_span s;
            pb_read_commit(&r2, pb_read_reserve(&r2, PB_CAPACITY, &s));
        }
        (void)getppid();                      /* marqueur fin */
        printf("allocations pendant run() : %ld\n", g_rt_allocs);
        d->deactivate(A.h);
        d->cleanup(A.h);
        t_cleanup_dir(dir);
        return g_rt_allocs ? 1 : 0;
    }

    /* --- Sans lecteur : pass-through seulement --- */
    A.link = 2;
    d->activate(A.h);
    int pt = 1;
    for (int k = 0; k < 10; k++) pt &= host_run(&A, 0);
    T_CHECK(pt, "sans lecteur : sortie identique à l'entrée");
    T_CHECK(A.connected == 0.0f, "sans lecteur : Connecté = 0");
    T_CHECK(W(&r2) == 0, "sans lecteur : rien d'écrit dans le tampon");
    T_CHECK(r2.hdr->writer_owner != 0 && r2.hdr->writer_rate == 48000, "écrivain inscrit, SR 48000");
    T_CHECK(r2.hdr->writer_heartbeat == 10, "heartbeat de l'écrivain : 10 blocs");

    /* --- Avec lecteur --- */
    uint64_t me = pb_new_owner_id();
    pb_reader_take(&r2, me, 48000);
    pt = 1;
    for (int k = 0; k < 20; k++) { pb_reader_beat(&r2, BLOCK); pt &= host_run(&A, 0); }
    T_CHECK(pt, "avec lecteur : pass-through intact");
    T_CHECK(A.connected == 1.0f, "avec lecteur : Connecté = 1");
    long got = drain_check(&r2, &A, 1.0f);
    T_CHECK(got == 20 * BLOCK, "20 blocs écrits dès que le lecteur bat");

    /* --- Gain -6.02 dB (×0.5), lissé sur un bloc --- */
    A.gain = -6.0206f;
    pb_reader_beat(&r2, BLOCK); host_run(&A, 0);
    pb_span sp; pb_read_commit(&r2, pb_read_reserve(&r2, BLOCK, &sp));  /* bloc de transition */
    pt = 1;
    for (int k = 0; k < 3; k++) { pb_reader_beat(&r2, BLOCK); pt &= host_run(&A, 0); }
    T_CHECK(pt, "le gain ne touche pas la sortie");
    T_CHECK(drain_check(&r2, &A, 0.5f) == 3 * BLOCK, "gain -6 dB appliqué à la copie");

    /* --- Traitement en place (entrée = sortie) --- */
    host_connect(&A, 1);
    pt = 1;
    for (int k = 0; k < 3; k++) { pb_reader_beat(&r2, BLOCK); pt &= host_run(&A, 1); }
    T_CHECK(pt, "en place : signal intact");
    T_CHECK(drain_check(&r2, &A, 0.5f) == 3 * BLOCK, "en place : copie correcte");
    host_connect(&A, 0);

    /* --- Gain -60 dB = silence --- */
    A.gain = -60.0f;
    pb_reader_beat(&r2, BLOCK); host_run(&A, 0);
    pb_read_commit(&r2, pb_read_reserve(&r2, BLOCK, &sp));  /* bloc de transition */
    for (int k = 0; k < 3; k++) { pb_reader_beat(&r2, BLOCK); host_run(&A, 0); }
    T_CHECK(drain_check(&r2, &A, 0.0f) == 3 * BLOCK, "gain -60 dB : copie silencieuse");
    A.gain = 0;
    pb_reader_beat(&r2, BLOCK); host_run(&A, 0);
    pb_read_commit(&r2, pb_read_reserve(&r2, PB_CAPACITY, &sp));

    /* --- Changement de lien 2 -> 5 --- */
    uint64_t me5 = pb_new_owner_id();
    pb_reader_take(&r5, me5, 48000);
    A.link = 5;
    for (int k = 0; k < 6; k++) { pb_reader_beat(&r5, BLOCK); host_run(&A, 0); }
    T_CHECK(r2.hdr->writer_owner == 0, "lien 2 libéré");
    T_CHECK(r5.hdr->writer_owner != 0, "lien 5 pris");
    T_CHECK(drain_check(&r5, &A, 1.0f) == 5 * BLOCK, "audio sur le lien 5");
    T_CHECK(pb_read_available(&r2) == 0, "plus rien sur le lien 2");

    /* --- Lecteur qui disparaît --- */
    int k_off = 0;
    for (; k_off < 200 && A.connected == 1.0f; k_off++) host_run(&A, 0);
    T_CHECK(A.connected == 0.0f && k_off * BLOCK >= 12000 && k_off * BLOCK <= 12000 + 2 * BLOCK,
            "lecteur muet : Connecté = 0 après ~250 ms");
    uint64_t w5 = W(&r5);
    for (int k = 0; k < 5; k++) host_run(&A, 0);
    T_CHECK(W(&r5) == w5, "lecteur absent : plus d'écriture");
    pb_read_commit(&r5, pb_read_reserve(&r5, PB_CAPACITY, &sp));

    /* --- Tampon plein (lecteur vivant qui ne lit pas) --- */
    uint64_t ov0 = r5.hdr->overruns;
    pt = 1;
    for (int k = 0; k < 80; k++) { pb_reader_beat(&r5, BLOCK); pt &= host_run(&A, 0); }
    T_CHECK(pt, "tampon plein : pass-through intact");
    T_CHECK(pb_read_available(&r5) == PB_CAPACITY, "tampon plein à 8192 trames");
    T_CHECK(r5.hdr->overruns - ov0 > 0, "sur-débits comptés");
    pb_read_commit(&r5, pb_read_reserve(&r5, PB_CAPACITY, &sp));

    /* --- Deux instances sur le même lien : la dernière activée gagne --- */
    Host B;
    T_CHECK(host_init(&B, d, argv[2]) == 0, "2e instance");
    B.link = 5;
    d->activate(B.h);
    pb_reader_beat(&r5, BLOCK); host_run(&B, 0);   /* B prend la main */
    uint64_t wb = W(&r5);
    for (int k = 0; k < 4; k++) { pb_reader_beat(&r5, BLOCK); host_run(&A, 0); host_run(&B, 0); }
    T_CHECK(W(&r5) - wb == 4 * BLOCK, "un seul écrivain à la fois (B)");
    d->deactivate(B.h);
    d->cleanup(B.h);
    pb_reader_beat(&r5, BLOCK); host_run(&A, 0);
    pb_reader_beat(&r5, BLOCK); host_run(&A, 0);
    T_CHECK(W(&r5) - wb >= 5 * BLOCK, "B parti : A reprend la place libre");

    T_CHECK(g_rt_allocs == 0, "aucune allocation pendant run()");
    d->deactivate(A.h);
    T_CHECK(r5.hdr->writer_owner == 0, "deactivate libère le lien");
    d->cleanup(A.h);
    pb_link_close(&r2);
    pb_link_close(&r5);

    /* --- Dossier inaccessible : le plugin marche quand même --- */
    setenv("PB_SHM_DIR", "/nonexistent-pedal-bridge", 1);
    Host C;
    T_CHECK(host_init(&C, d, argv[2]) == 0, "instanciation sans dossier partagé");
    C.link = 3;
    d->activate(C.h);
    pt = 1;
    for (int k = 0; k < 5; k++) pt &= host_run(&C, 0);
    T_CHECK(pt && C.connected == 0.0f, "sans dossier : pass-through, Connecté = 0");
    d->deactivate(C.h);
    d->cleanup(C.h);

    T_CHECK(fn(1) && !strcmp(fn(1)->URI, "http://pedal-bridge.local/plugins/return"), "descripteur 1 = Return");
    T_CHECK(fn(6) == NULL, "6 descripteurs seulement (Send, Return, Looper et Drum mono et stéréo)");
    t_cleanup_dir(dir);
    return t_summary("test_plugin");
}
