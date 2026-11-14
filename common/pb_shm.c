/* pedal-bridge : ouverture et mapping des tampons partagés (hors temps réel). */
#define _GNU_SOURCE
#include "pb_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *pb_shm_dir(void)
{
    const char *d = getenv("PB_SHM_DIR");
    return (d && *d) ? d : "/dev/shm";
}

int pb_link_path(char *buf, size_t len, uint32_t link, int direction)
{
    if (link < 1 || link > PB_MAX_LINKS) return -EINVAL;
    int n = snprintf(buf, len, "%s/pedal-bridge-L%u-%s", pb_shm_dir(), link,
                     direction == PB_DIR_SEND ? "send" : "return");
    return (n < 0 || (size_t)n >= len) ? -ENAMETOOLONG : 0;
}

uint64_t pb_new_owner_id(void)
{
    static uint32_t counter;
    uint32_t c = __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED);
    uint64_t id = ((uint64_t)(uint32_t)getpid() << 32) | c;
    return id ? id : 1;
}

static void set_err(char *err, size_t errlen, const char *fmt, const char *path, int e)
{
    if (err && errlen) snprintf(err, errlen, fmt, path, strerror(e));
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Vérifie l'en-tête d'un fichier existant. Attend jusqu'à ~200 ms qu'un autre
 * processus finisse de l'initialiser. */
static int check_header(pb_header *h, uint32_t link, int direction)
{
    for (int i = 0; i < 40 && PB_LOAD_ACQ(&h->magic) != PB_MAGIC; i++)
        sleep_ms(5);
    if (PB_LOAD_ACQ(&h->magic) != PB_MAGIC) return -EPROTO;
    if (h->version != PB_VERSION || h->header_bytes != PB_HEADER_BYTES ||
        h->channels != PB_CHANNELS || h->capacity != PB_CAPACITY ||
        h->link != link || h->direction != (uint32_t)direction)
        return -EPROTO;
    return 0;
}

int pb_link_open(pb_link *l, uint32_t link, int direction, int create,
                 char *err, size_t errlen)
{
    char path[256];
    int rc = pb_link_path(path, sizeof path, link, direction);
    memset(l, 0, sizeof *l);
    if (rc) { set_err(err, errlen, "%s: %s", "chemin", -rc); return rc; }

    int created = 0;
    /* D'abord sans O_CREAT : fs.protected_regular refuse O_CREAT sur un fichier
     * d'un autre compte dans /dev/shm, même en 0666. */
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            created = 1;
        } else if (errno == EEXIST) {
            fd = open(path, O_RDWR | O_CLOEXEC);  /* un autre l'a créé entre-temps */
        }
    }
    if (fd < 0) {
        int e = errno;
        set_err(err, errlen, "%s: %s", path, e);
        return -e;
    }

    if (created) {
        /* Droits ouverts, peu importe l'umask du processus. */
        if (fchmod(fd, 0666) != 0 || ftruncate(fd, (off_t)PB_FILE_BYTES) != 0) {
            int e = errno;
            set_err(err, errlen, "%s: %s", path, e);
            close(fd);
            return -e;
        }
    } else {
        struct stat st;
        /* Le créateur fait ftruncate juste après open : on lui laisse un instant. */
        for (int i = 0; i < 40; i++) {
            if (fstat(fd, &st) != 0) { int e = errno; set_err(err, errlen, "%s: %s", path, e); close(fd); return -e; }
            if ((size_t)st.st_size >= PB_FILE_BYTES) break;
            sleep_ms(5);
        }
        if ((size_t)st.st_size != PB_FILE_BYTES) {
            set_err(err, errlen, "%s: %s (taille inattendue)", path, EPROTO);
            close(fd);
            return -EPROTO;
        }
    }

    void *p = mmap(NULL, PB_FILE_BYTES, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, fd, 0);
    int e = errno;
    close(fd);
    if (p == MAP_FAILED) { set_err(err, errlen, "%s: %s", path, e); return -e; }

    /* Éviter les défauts de page dans le thread audio (échec sans gravité). */
    (void)mlock(p, PB_FILE_BYTES);

    pb_header *h = (pb_header *)p;
    if (created) {
        h->version = PB_VERSION;
        h->header_bytes = PB_HEADER_BYTES;
        h->channels = PB_CHANNELS;
        h->capacity = PB_CAPACITY;
        h->link = link;
        h->direction = (uint32_t)direction;
        h->creator_uid = (uint32_t)getuid();
        PB_STORE_REL(&h->magic, PB_MAGIC);  /* publié en dernier */
    } else {
        rc = check_header(h, link, direction);
        if (rc) {
            set_err(err, errlen, "%s: %s (en-tete incompatible)", path, -rc);
            munmap(p, PB_FILE_BYTES);
            return rc;
        }
        /* Toucher chaque page au cas où MAP_POPULATE/mlock n'aurait pas suffi. */
        volatile const uint8_t *b = (const uint8_t *)p;
        uint8_t sink = 0;
        for (size_t off = 0; off < PB_FILE_BYTES; off += 4096) sink ^= b[off];
        (void)sink;
    }

    l->hdr = h;
    l->data = (float *)((uint8_t *)p + PB_HEADER_BYTES);
    l->mask = PB_CAPACITY - 1u;
    l->link = link;
    l->direction = (uint32_t)direction;
    return 0;
}

void pb_link_close(pb_link *l)
{
    if (l && l->hdr) {
        munlock(l->hdr, PB_FILE_BYTES);
        munmap(l->hdr, PB_FILE_BYTES);
    }
    if (l) memset(l, 0, sizeof *l);
}
