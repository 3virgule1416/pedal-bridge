/*
 * test_return : charge pedal_bridge.so (dlopen) et vérifie le plugin Return.
 * Le test joue le rôle du client JACK (écrivain des tampons -return).
 *
 *   test_return <chemin/pedal_bridge.so> <dossier du bundle> [--rt-loop]
 */
#define _GNU_SOURCE
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

#define BLOCK 128
#define RATE  48000.0
#define TARGET 256u

enum { P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_LINK, P_MODE, P_GAIN, P_CONNECTED };

static const float IN_L = 0.25f, IN_R = -0.125f;

typedef struct {
    const LV2_Descriptor *d;
    LV2_Handle h;
    float in[2][BLOCK], out[2][BLOCK];
    float link, mode, gain, connected;
    int inplace;
    float prev_wet[2];   /* pour mesurer les sauts d'un bloc à l'autre */
    float max_jump;
} Host;

/* Signal écrit par le « client » : dépend de l'indice absolu dans le tampon. */
static float X(uint64_t f, int c) { return 0.5f * sinf((float)(f % 100000) * (c ? 0.013f : 0.0091f)); }

static void host_connect(Host *t, int inplace)
{
    const LV2_Descriptor *d = t->d;
    t->inplace = inplace;
    d->connect_port(t->h, P_IN_L, t->in[0]);
    d->connect_port(t->h, P_IN_R, t->in[1]);
    d->connect_port(t->h, P_OUT_L, inplace ? t->in[0] : t->out[0]);
    d->connect_port(t->h, P_OUT_R, inplace ? t->in[1] : t->out[1]);
    d->connect_port(t->h, P_LINK, &t->link);
    d->connect_port(t->h, P_MODE, &t->mode);
    d->connect_port(t->h, P_GAIN, &t->gain);
    d->connect_port(t->h, P_CONNECTED, &t->connected);
}

static int host_init(Host *t, const LV2_Descriptor *d, const char *bundle, float link)
{
    memset(t, 0, sizeof *t);
    t->d = d;
    t->link = link;
    static const LV2_Feature *none[] = { NULL };
    t->h = d->instantiate(d, RATE, bundle, none);
    if (!t->h) return -1;
    host_connect(t, 0);
    return 0;
}

static const float *O(const Host *t, int c) { return t->inplace ? t->in[c] : t->out[c]; }

/* Un bloc : entrée constante, run(), suivi des sauts du retour (sortie - entrée). */
static void run1(Host *t)
{
    for (int i = 0; i < BLOCK; i++) { t->in[0][i] = IN_L; t->in[1][i] = IN_R; }
    g_in_rt = 1;
    t->d->run(t->h, BLOCK);
    g_in_rt = 0;
    if (t->mode == 0) {
        for (int c = 0; c < 2; c++) {
            const float in = c ? IN_R : IN_L;
            for (int i = 0; i < BLOCK; i++) {
                float w = O(t, c)[i] - in;
                float j = fabsf(w - t->prev_wet[c]);
                if (j > t->max_jump) t->max_jump = j;
                t->prev_wet[c] = w;
            }
        }
    }
}

/* Sortie == entrée × dry (exact). */
static int check_dry(const Host *t, float dry)
{
    for (int i = 0; i < BLOCK; i++)
        if (O(t, 0)[i] != IN_L * dry || O(t, 1)[i] != IN_R * dry) return 0;
    return 1;
}

/* Sortie == entrée × dry + X(f0+i) × g. */
static int check_block(const Host *t, float dry, uint64_t f0, float g)
{
    for (int i = 0; i < BLOCK; i++)
        for (int c = 0; c < 2; c++) {
            float want = (c ? IN_R : IN_L) * dry + X(f0 + (uint64_t)i, c) * g;
            if (fabsf(O(t, c)[i] - want) > 2e-6f) {
                fprintf(stderr, "  i=%d c=%d : %g au lieu de %g\n", i, c, O(t, c)[i], want);
                return 0;
            }
        }
    return 1;
}

/* Le « client » écrit n trames. */
static void wr(pb_link *l, uint32_t n)
{
    pb_span s;
    uint64_t f = PB_LOAD_RLX(&l->hdr->write_idx);
    uint32_t got = pb_write_reserve(l, n, &s);
    for (uint32_t k = 0; k < got; k++, f++) {
        float *p = k < s.n1 ? s.p1 + 2 * k : s.p2 + 2 * (k - s.n1);
        p[0] = X(f, 0); p[1] = X(f, 1);
    }
    pb_write_commit(l, got);
}

static uint64_t R(pb_link *l) { return PB_LOAD_ACQ(&l->hdr->read_idx); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage : test_return pedal_bridge.so bundle/ [--rt-loop]\n"); return 2; }
    int rt_loop = argc > 3 && !strcmp(argv[3], "--rt-loop");

    char dir[] = "/tmp/pb-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("PB_SHM_DIR", dir, 1);
    umask(022);

    void *so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
    const LV2_Descriptor *d = fn ? fn(1) : NULL;
    T_CHECK(d && !strcmp(d->URI, "http://pedal-bridge.local/plugins/return"), "descripteur 1 = Return");
    if (!d) return 1;

    Host A;
    T_CHECK(host_init(&A, d, argv[2], 3) == 0, "instanciation");
    char path[256], err[256];
    struct stat st;
    int all = 1;
    for (uint32_t i = 1; i <= PB_MAX_LINKS; i++) {
        pb_link_path(path, sizeof path, i, PB_DIR_RETURN);
        if (stat(path, &st) != 0 || (st.st_mode & 0777) != 0666) all = 0;
    }
    T_CHECK(all, "8 fichiers Return créés en 0666");

    pb_link w3, w6;
    T_CHECK(pb_link_open(&w3, 3, PB_DIR_RETURN, 0, err, sizeof err) == 0, "écrivain ouvre le lien 3");
    T_CHECK(pb_link_open(&w6, 6, PB_DIR_RETURN, 0, err, sizeof err) == 0, "écrivain ouvre le lien 6");
    uint64_t me = pb_new_owner_id();

    /* --- Mode RT pour strace --- */
    if (rt_loop) {
        d->activate(A.h);
        pb_writer_take(&w3, me, 48000); pb_writer_set_target(&w3, TARGET);
        pb_writer_take(&w6, me, 48000); pb_writer_set_target(&w6, TARGET);
        run1(&A);
        g_rt_allocs = 0;
        (void)getppid();                       /* marqueur début */
        for (int k = 0; k < 3000; k++) {
            if (k % 500 < 450) { wr(&w3, BLOCK); wr(&w6, BLOCK); }   /* trous => sous-débits */
            pb_writer_beat(&w3, BLOCK); pb_writer_beat(&w6, BLOCK);
            if (k == 1000) A.gain = -6.0f;
            if (k == 1500) A.mode = 1;
            if (k == 2000) A.mode = 0;
            if (k == 2500) A.link = 6;
            run1(&A);
        }
        (void)getppid();                       /* marqueur fin */
        printf("allocations pendant run() : %ld\n", g_rt_allocs);
        d->deactivate(A.h);
        d->cleanup(A.h);
        t_cleanup_dir(dir);
        return g_rt_allocs ? 1 : 0;
    }

    d->activate(A.h);

    /* --- 1. Sans écrivain --- */
    int ok = 1;
    for (int k = 0; k < 5; k++) { run1(&A); ok &= check_dry(&A, 1); }
    T_CHECK(ok, "sans écrivain, Add : sortie identique à l'entrée");
    T_CHECK(A.connected == 0.0f, "sans écrivain : Connecté = 0");
    T_CHECK(w3.hdr->reader_owner != 0 && w3.hdr->reader_rate == 48000, "lecteur inscrit, SR 48000");
    T_CHECK(w3.hdr->reader_heartbeat == 5, "heartbeat du lecteur : 5 blocs");

    A.mode = 1; run1(&A);                       /* bloc de transition */
    ok = 1;
    for (int k = 0; k < 3; k++) { run1(&A); ok &= check_dry(&A, 0); }
    T_CHECK(ok, "sans écrivain, Replace : silence");
    A.mode = 0; run1(&A);
    run1(&A);
    T_CHECK(check_dry(&A, 1), "retour en Add");

    /* --- 2. Écrivain présent --- */
    pb_writer_take(&w3, me, 48000);
    pb_writer_set_target(&w3, TARGET);
    wr(&w3, 512); pb_writer_beat(&w3, BLOCK);
    A.max_jump = 0;
    run1(&A);
    T_CHECK(A.connected == 1.0f, "écrivain présent : Connecté = 1");
    T_CHECK(pb_read_available(&w3) == TARGET - BLOCK,
            "démarrage : surplus jeté, réserve = cible (256) moins un bloc");
    ok = 1;
    for (int k = 0; k < 10; k++) {
        wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK);
        uint64_t f0 = R(&w3);
        run1(&A);
        ok &= check_block(&A, 1, f0, 1.0f);
    }
    T_CHECK(ok, "Add : sortie = entrée + retour, trame par trame");
    T_CHECK(A.max_jump < 0.02f, "fondu d'entrée sans saut");

    /* Gain -6 dB */
    A.gain = -6.0206f;
    wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK); run1(&A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK);
        uint64_t f0 = R(&w3); run1(&A); ok &= check_block(&A, 1, f0, 0.5f);
    }
    T_CHECK(ok, "gain -6 dB sur le retour seulement");
    A.gain = 0; wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK); run1(&A);

    /* Replace */
    A.mode = 1;
    wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK); run1(&A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK);
        uint64_t f0 = R(&w3); run1(&A); ok &= check_block(&A, 0, f0, 1.0f);
    }
    T_CHECK(ok, "Replace : sortie = retour seulement");
    A.mode = 0;
    wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK); run1(&A);

    /* En place */
    host_connect(&A, 1);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK);
        uint64_t f0 = R(&w3); run1(&A); ok &= check_block(&A, 1, f0, 1.0f);
    }
    T_CHECK(ok, "en place (entrée = sortie) : correct");
    host_connect(&A, 0);

    /* --- 3. Sous-débit : l'écrivain bat mais n'écrit plus --- */
    wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK); run1(&A);
    uint64_t un0 = w3.hdr->underruns;
    A.max_jump = 0;
    for (int k = 0; k < 3; k++) { pb_writer_beat(&w3, BLOCK); run1(&A); }
    T_CHECK(w3.hdr->underruns - un0 > 0, "sous-débit compté");
    T_CHECK(A.max_jump < 0.02f, "tampon vide : fin du retour sans saut (pas de clic)");
    pb_writer_beat(&w3, BLOCK); run1(&A);
    T_CHECK(check_dry(&A, 1) && A.connected == 1.0f, "en attente : signal intact, toujours connecté");

    /* Reprise */
    A.max_jump = 0;
    wr(&w3, 512); pb_writer_beat(&w3, BLOCK); run1(&A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        wr(&w3, BLOCK); pb_writer_beat(&w3, BLOCK);
        uint64_t f0 = R(&w3); run1(&A); ok &= check_block(&A, 1, f0, 1.0f);
    }
    T_CHECK(ok && A.max_jump < 0.02f, "reprise après sous-débit, avec fondu");

    /* --- 4. L'écrivain disparaît --- */
    A.max_jump = 0;
    int k_off = 0;
    for (; k_off < 200 && A.connected == 1.0f; k_off++) run1(&A);
    T_CHECK(A.connected == 0.0f && k_off * BLOCK >= 12000 && k_off * BLOCK <= 12000 + 2 * BLOCK,
            "écrivain muet : Connecté = 0 après ~250 ms");
    T_CHECK(A.max_jump < 0.02f, "écrivain disparu : fin sans saut");
    run1(&A);
    T_CHECK(check_dry(&A, 1), "écrivain disparu : signal intact");

    /* --- 5. Vieux son accumulé pendant que le plugin était arrêté --- */
    d->deactivate(A.h);
    T_CHECK(w3.hdr->reader_owner == 0, "deactivate libère le lien");
    wr(&w3, 4096); pb_writer_beat(&w3, BLOCK);
    d->activate(A.h);
    pb_writer_beat(&w3, BLOCK); run1(&A);
    pb_writer_beat(&w3, BLOCK); run1(&A);
    T_CHECK(pb_read_available(&w3) == TARGET - BLOCK, "réactivation : vieux son jeté (latence pas accumulée)");

    /* --- 6. Changement de lien 3 -> 6 --- */
    A.link = 6; run1(&A);
    T_CHECK(w3.hdr->reader_owner == 0 && w6.hdr->reader_owner != 0, "lien 3 libéré, lien 6 pris");
    pb_writer_take(&w6, me, 48000);
    pb_writer_set_target(&w6, TARGET);
    wr(&w6, 512); pb_writer_beat(&w6, BLOCK); run1(&A);
    ok = 1;
    for (int k = 0; k < 3; k++) {
        wr(&w6, BLOCK); pb_writer_beat(&w6, BLOCK);
        uint64_t f0 = R(&w6); run1(&A); ok &= check_block(&A, 1, f0, 1.0f);
    }
    T_CHECK(ok, "audio du lien 6");

    /* --- 7. Deux instances sur le même lien : la dernière activée lit --- */
    Host B;
    T_CHECK(host_init(&B, d, argv[2], 6) == 0, "2e instance");
    d->activate(B.h);
    wr(&w6, BLOCK); pb_writer_beat(&w6, BLOCK); run1(&A); run1(&B);
    uint32_t av = pb_read_available(&w6);
    ok = 1;
    for (int k = 0; k < 4; k++) {
        wr(&w6, BLOCK); pb_writer_beat(&w6, BLOCK);
        run1(&A); run1(&B);
        if (k > 0) ok &= check_dry(&A, 1);
    }
    T_CHECK(ok, "l'ancienne instance ne lit plus (signal intact)");
    T_CHECK(pb_read_available(&w6) <= av + BLOCK, "un seul lecteur consomme");
    d->deactivate(B.h);
    d->cleanup(B.h);

    T_CHECK(g_rt_allocs == 0, "aucune allocation pendant run()");
    d->deactivate(A.h);
    d->cleanup(A.h);
    pb_link_close(&w3);
    pb_link_close(&w6);

    /* --- 8. Dossier inaccessible --- */
    setenv("PB_SHM_DIR", "/nonexistent-pedal-bridge", 1);
    Host C;
    T_CHECK(host_init(&C, d, argv[2], 2) == 0, "instanciation sans dossier partagé");
    d->activate(C.h);
    run1(&C);
    T_CHECK(check_dry(&C, 1) && C.connected == 0.0f, "sans dossier, Add : signal intact");
    C.mode = 1; run1(&C); run1(&C);
    T_CHECK(check_dry(&C, 0), "sans dossier, Replace : silence");
    d->deactivate(C.h);
    d->cleanup(C.h);

    t_cleanup_dir(dir);
    return t_summary("test_return");
}
