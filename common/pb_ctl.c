/* pedal-bridge : ouverture du fichier de contrôle (hors temps réel). */
#define _GNU_SOURCE
#include "pb_ctl.h"
#include "pb_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int fail(char *err, size_t errlen, const char *path, int e, const char *what)
{
    if (err && errlen) snprintf(err, errlen, "%s: %s%s", path, strerror(e), what);
    return -e;
}

int pb_ctl_open(pb_ctl **out, int create, char *err, size_t errlen)
{
    char path[256];
    *out = NULL;
    int n = snprintf(path, sizeof path, "%s/pedal-bridge-control", pb_shm_dir());
    if (n < 0 || (size_t)n >= sizeof path) return fail(err, errlen, "chemin", ENAMETOOLONG, "");

    int created = 0;
    int fd = open(path, O_RDWR | O_CLOEXEC);       /* sans O_CREAT d'abord : voir pb_shm.c */
    if (fd < 0 && errno == ENOENT && create) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) created = 1;
        else if (errno == EEXIST) fd = open(path, O_RDWR | O_CLOEXEC);
    }
    if (fd < 0) return fail(err, errlen, path, errno, "");

    if (created) {
        if (fchmod(fd, 0666) != 0 || ftruncate(fd, (off_t)PB_CTL_BYTES) != 0) {
            int e = errno; close(fd); return fail(err, errlen, path, e, "");
        }
    } else {
        struct stat st;
        for (int i = 0; i < 40; i++) {
            if (fstat(fd, &st) != 0) { int e = errno; close(fd); return fail(err, errlen, path, e, ""); }
            if ((size_t)st.st_size >= PB_CTL_BYTES) break;
            sleep_ms(5);
        }
        if ((size_t)st.st_size != PB_CTL_BYTES) { close(fd); return fail(err, errlen, path, EPROTO, " (taille inattendue)"); }
    }

    void *p = mmap(NULL, PB_CTL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    int e = errno;
    close(fd);
    if (p == MAP_FAILED) return fail(err, errlen, path, e, "");
    (void)mlock(p, PB_CTL_BYTES);

    pb_ctl *c = (pb_ctl *)p;
    if (created) {
        c->version = PB_CTL_VERSION;
        c->bytes = PB_CTL_BYTES;
        c->creator_uid = (uint32_t)getuid();
        c->looper_loop = 1;
        c->looper_tempo = 12000;
        c->drum_groove = 1;
        c->drum_tempo = 12000;
        PB_STORE_REL(&c->magic, PB_CTL_MAGIC);
    } else {
        for (int i = 0; i < 40 && PB_LOAD_ACQ(&c->magic) != PB_CTL_MAGIC; i++) sleep_ms(5);
        if (PB_LOAD_ACQ(&c->magic) != PB_CTL_MAGIC || c->version != PB_CTL_VERSION || c->bytes != PB_CTL_BYTES) {
            munmap(p, PB_CTL_BYTES);
            return fail(err, errlen, path, EPROTO, " (en-tete incompatible)");
        }
    }
    *out = c;
    return 0;
}

void pb_ctl_close(pb_ctl *c)
{
    if (!c) return;
    munlock(c, PB_CTL_BYTES);
    munmap(c, PB_CTL_BYTES);
}
