/*
 * test_ring : un écrivain et un lecteur dans deux threads, blocs de tailles
 * aléatoires, des millions de trames. Vérifie qu'aucune trame n'est perdue,
 * dupliquée ou désordonnée, et que les deux canaux restent alignés.
 * Vérifie aussi la création (droits 0666), la réouverture et les en-têtes.
 */
#define _GNU_SOURCE
#include "pb_shm.h"
#include "test_util.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TOTAL_FRAMES (6u * 1000u * 1000u)

static pb_link g_w, g_r;
static volatile int g_fail;

static float val(uint64_t i) { return (float)(i & 0xFFFFFu); }  /* exact en float */

static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void *writer(void *arg)
{
    (void)arg;
    uint32_t seed = 1;
    uint64_t i = 0;
    while (i < TOTAL_FRAMES && !g_fail) {
        uint32_t want = 1 + rnd(&seed) % 700;
        if (want > TOTAL_FRAMES - i) want = (uint32_t)(TOTAL_FRAMES - i);
        pb_span s;
        uint32_t n = pb_write_reserve(&g_w, want, &s);
        if (!n) { sched_yield(); continue; }
        for (uint32_t k = 0; k < s.n1; k++, i++) { s.p1[2*k] = val(i); s.p1[2*k+1] = -val(i); }
        for (uint32_t k = 0; k < s.n2; k++, i++) { s.p2[2*k] = val(i); s.p2[2*k+1] = -val(i); }
        pb_write_commit(&g_w, n);
        pb_writer_beat(&g_w, n);
    }
    return NULL;
}

static void *reader(void *arg)
{
    (void)arg;
    uint32_t seed = 7;
    uint64_t i = 0;
    while (i < TOTAL_FRAMES && !g_fail) {
        uint32_t want = 1 + rnd(&seed) % 900;
        pb_span s;
        uint32_t n = pb_read_reserve(&g_r, want, &s);
        if (!n) { sched_yield(); continue; }
        for (int seg = 0; seg < 2; seg++) {
            const float *p = seg ? s.p2 : s.p1;
            uint32_t m = seg ? s.n2 : s.n1;
            for (uint32_t k = 0; k < m; k++, i++) {
                if (p[2*k] != val(i) || p[2*k+1] != -val(i)) {
                    fprintf(stderr, "  trame %llu : lu %g/%g, attendu %g/%g\n",
                            (unsigned long long)i, p[2*k], p[2*k+1], val(i), -val(i));
                    g_fail = 1;
                    return NULL;
                }
            }
        }
        pb_read_commit(&g_r, n);
    }
    return NULL;
}

int main(void)
{
    char dir[] = "/tmp/pb-test-XXXXXX";
    T_CHECK(mkdtemp(dir) != NULL, "dossier temporaire");
    setenv("PB_SHM_DIR", dir, 1);
    umask(022);  /* le fichier doit quand même finir en 0666 */

    char err[256], path[256];
    pb_link tmp;
    T_CHECK(pb_link_open(&tmp, 3, PB_DIR_SEND, 0, err, sizeof err) == -ENOENT,
            "sans create : fichier absent -> ENOENT, rien de créé");
    T_CHECK(pb_link_open(&tmp, 0, PB_DIR_SEND, 1, err, sizeof err) == -EINVAL, "lien 0 refusé");
    T_CHECK(pb_link_open(&tmp, 9, PB_DIR_SEND, 1, err, sizeof err) == -EINVAL, "lien 9 refusé");

    T_CHECK(pb_link_open(&g_w, 3, PB_DIR_SEND, 1, err, sizeof err) == 0, "création du lien 3");
    pb_link_path(path, sizeof path, 3, PB_DIR_SEND);
    struct stat st;
    T_CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0666, "droits 0666 malgré umask 022");
    T_CHECK((size_t)st.st_size == PB_FILE_BYTES, "taille du fichier");
    T_CHECK(g_w.hdr->magic == PB_MAGIC && g_w.hdr->version == PB_VERSION &&
            g_w.hdr->link == 3 && g_w.hdr->direction == PB_DIR_SEND, "en-tête");

    T_CHECK(pb_link_open(&g_r, 3, PB_DIR_SEND, 0, err, sizeof err) == 0, "réouverture sans create");
    T_CHECK(pb_link_open(&tmp, 3, PB_DIR_RETURN, 0, err, sizeof err) == -ENOENT,
            "le sens return est un autre fichier");

    /* Propriété écrivain/lecteur. */
    uint64_t a = pb_new_owner_id(), b = pb_new_owner_id();
    T_CHECK(a != b && a && b, "identifiants uniques");
    T_CHECK(pb_writer_claim(&g_w, a) == 1, "écrivain A prend la place libre");
    T_CHECK(pb_writer_claim(&g_w, b) == 0, "écrivain B refusé");
    pb_writer_take(&g_w, b, 48000);
    T_CHECK(pb_writer_claim(&g_w, a) == 0 && pb_writer_claim(&g_w, b) == 1, "B prend la main (take)");
    pb_writer_release(&g_w, a);
    T_CHECK(g_w.hdr->writer_owner == b, "A ne peut pas libérer la place de B");
    pb_writer_release(&g_w, b);
    T_CHECK(g_w.hdr->writer_owner == 0, "B libère");

    /* Tampon plein : l'écrivain obtient moins que demandé. */
    pb_span s;
    T_CHECK(pb_write_reserve(&g_w, PB_CAPACITY + 10, &s) == PB_CAPACITY, "réserve limitée à la capacité");
    pb_write_commit(&g_w, PB_CAPACITY);
    T_CHECK(pb_write_reserve(&g_w, 1, &s) == 0, "plein : 0 trame");
    pb_reader_resync(&g_r, 100);
    T_CHECK(pb_read_available(&g_r) == 100, "resync garde 100 trames");
    pb_reader_resync(&g_r, 0);
    T_CHECK(pb_read_available(&g_r) == 0, "resync à vide");

    /* Détection du correspondant. */
    pb_watch w;
    pb_watch_init(&w, 1000);
    T_CHECK(pb_watch_update(&w, 555, 128) == 0 && !w.alive, "heartbeat figé au départ : absent");
    T_CHECK(pb_watch_update(&w, 556, 128) == 1 && w.alive, "heartbeat qui bouge : présent");
    for (int k = 0; k < 7; k++) pb_watch_update(&w, 556, 128);
    T_CHECK(w.alive, "encore présent après 896 trames");
    pb_watch_update(&w, 556, 128);
    T_CHECK(!w.alive, "absent après 1024 trames sans battement");

    /* Débit concurrent : repartir d'indices quelconques (non nuls). */
    pb_reader_resync(&g_r, 0);
    pthread_t tw, tr;
    double t0 = t_now();
    pthread_create(&tr, NULL, reader, NULL);
    pthread_create(&tw, NULL, writer, NULL);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);
    double dt = t_now() - t0;
    T_CHECK(!g_fail, "6 M trames transférées sans perte ni désordre");
    printf("  (%.2f s, %.1f M trames/s)\n", dt, TOTAL_FRAMES / dt / 1e6);
    T_CHECK(g_w.hdr->overruns == 0 && g_w.hdr->underruns == 0, "compteurs de débit à zéro");

    pb_link_close(&g_w);
    pb_link_close(&g_r);
    t_cleanup_dir(dir);
    return t_summary("test_ring");
}
