#ifndef PB_TEST_UTIL_H
#define PB_TEST_UTIL_H

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "pb_shm.h"

/* Nettoie le dossier temporaire du test : seulement les fichiers que le test
 * a lui-même créés (pedal-bridge-L*-send/return), puis le dossier vide. */
static inline void t_cleanup_dir(const char *dir)
{
    char path[512];
    for (unsigned i = 1; i <= PB_MAX_LINKS; i++)
        for (int d = 0; d < 2; d++) {
            snprintf(path, sizeof path, "%s/pedal-bridge-L%u-%s", dir, i, d ? "return" : "send");
            unlink(path);
        }
    rmdir(dir);
}

static int t_pass, t_fail;

#define T_CHECK(cond, msg) do {                                   \
        if (cond) { t_pass++; printf("  ok    %s\n", msg); }       \
        else { t_fail++; printf("  ÉCHEC %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

static inline double t_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline int t_summary(const char *name)
{
    printf("%s : %d ok, %d échec(s)\n", name, t_pass, t_fail);
    return t_fail ? 1 : 0;
}

#endif
