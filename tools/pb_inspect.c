/*
 * pb_inspect : petit lecteur de contrôle des tampons pedal-bridge.
 *
 *   pb_inspect                   état de tous les liens (en-têtes, compteurs)
 *   pb_inspect --watch           pareil, rafraîchi chaque seconde
 *   pb_inspect --read N [--seconds S] [--wav fichier.wav]
 *                                agit comme lecteur du lien Send N : consomme
 *                                l'audio, affiche crête/RMS, enregistre au besoin
 *
 * N'ouvre que des fichiers existants : ne crée jamais rien dans /dev/shm.
 */
#define _GNU_SOURCE
#include "pb_shm.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_s(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static const char *owner_str(uint64_t o, char *buf, size_t n)
{
    if (!o) snprintf(buf, n, "-");
    else snprintf(buf, n, "pid %u", (unsigned)(o >> 32));
    return buf;
}

/* Un heartbeat est vivant s'il a bougé pendant 'window' secondes. */
static int beat_alive(const uint32_t *beat, double window)
{
    uint32_t a = PB_LOAD_ACQ(beat);
    double t0 = now_s();
    while (now_s() - t0 < window) {
        if (PB_LOAD_ACQ(beat) != a) return 1;
        sleep_s(0.002);
    }
    return 0;
}

static void print_status(void)
{
    printf("%-22s %6s %6s %-10s %-10s %6s %5s %5s %10s %10s\n",
           "fichier", "SR-ecr", "SR-lec", "ecrivain", "lecteur", "rempl", "ecr?", "lec?",
           "sur-debit", "sous-deb");
    for (int dir = 0; dir < 2; dir++) {
        for (uint32_t i = 1; i <= PB_MAX_LINKS; i++) {
            pb_link l;
            char err[256], path[256], a[32], b[32];
            pb_link_path(path, sizeof path, i, dir);
            int rc = pb_link_open(&l, i, dir, 0, err, sizeof err);
            if (rc == -ENOENT) continue;
            const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            if (rc) { printf("%-22s ERREUR %s\n", name, err); continue; }
            pb_header *h = l.hdr;
            uint64_t w = PB_LOAD_ACQ(&h->write_idx), r = PB_LOAD_ACQ(&h->read_idx);
            printf("%-22s %6u %6u %-10s %-10s %6llu %5s %5s %10llu %10llu\n",
                   name, h->writer_rate, h->reader_rate,
                   owner_str(PB_LOAD_ACQ(&h->writer_owner), a, sizeof a),
                   owner_str(PB_LOAD_ACQ(&h->reader_owner), b, sizeof b),
                   (unsigned long long)(w - r),
                   beat_alive(&h->writer_heartbeat, 0.1) ? "oui" : "non",
                   beat_alive(&h->reader_heartbeat, 0.1) ? "oui" : "non",
                   (unsigned long long)PB_LOAD_RLX(&h->overruns),
                   (unsigned long long)PB_LOAD_RLX(&h->underruns));
            pb_link_close(&l);
        }
    }
}

/* ---- WAV float 32 bits stéréo ---- */
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_header(FILE *f, uint32_t rate, uint32_t data_bytes)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 3); put16(f, 2); put32(f, rate); put32(f, rate * 8); put16(f, 8); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, data_bytes);
}

static double to_db(double v) { return v > 1e-9 ? 20.0 * log10(v) : -180.0; }

static int do_read(uint32_t link, double seconds, const char *wav)
{
    pb_link l;
    char err[256];
    if (pb_link_open(&l, link, PB_DIR_SEND, 0, err, sizeof err) != 0) {
        fprintf(stderr, "Impossible d'ouvrir le lien %u : %s\n"
                        "(le plugin Send doit être chargé dans PiPedal)\n", link, err);
        return 1;
    }
    pb_header *h = l.hdr;
    uint64_t me = pb_new_owner_id();
    uint32_t rate = h->writer_rate ? h->writer_rate : 48000;
    pb_reader_take(&l, me, rate);
    pb_reader_resync(&l, 0);

    FILE *wf = NULL;
    uint64_t wav_frames = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) { perror(wav); pb_link_close(&l); return 1; }
        wav_header(wf, rate, 0);
    }

    uint64_t ov0 = PB_LOAD_RLX(&h->overruns), un0 = PB_LOAD_RLX(&h->underruns);
    double t0 = now_s(), tprint = t0 + 0.5;
    double peak[2] = {0, 0}, sum2[2] = {0, 0};
    uint64_t nwin = 0, total = 0;
    uint32_t maxfill = 0, last_beat = PB_LOAD_ACQ(&h->writer_heartbeat);
    double last_beat_t = t0;

    printf("Lecture du lien %u (SR écrivain %u Hz). Ctrl-C pour arrêter.\n", link, rate);
    while (!g_stop && (seconds <= 0 || now_s() - t0 < seconds)) {
        pb_reader_beat(&l, 0);
        uint32_t avail = pb_read_available(&l);
        if (avail > maxfill) maxfill = avail;
        pb_span sp;
        uint32_t n = pb_read_reserve(&l, avail, &sp);
        for (int seg = 0; seg < 2; seg++) {
            const float *p = seg ? sp.p2 : sp.p1;
            uint32_t m = seg ? sp.n2 : sp.n1;
            for (uint32_t k = 0; k < m; k++)
                for (int c = 0; c < 2; c++) {
                    double v = p[2 * k + c];
                    if (fabs(v) > peak[c]) peak[c] = fabs(v);
                    sum2[c] += v * v;
                }
            if (wf && m) fwrite(p, sizeof(float) * 2, m, wf);
        }
        pb_read_commit(&l, n);
        nwin += n; total += n; wav_frames += wf ? n : 0;

        uint32_t beat = PB_LOAD_ACQ(&h->writer_heartbeat);
        double t = now_s();
        if (beat != last_beat) { last_beat = beat; last_beat_t = t; }

        if (t >= tprint) {
            int alive = t - last_beat_t < 0.25;
            double rl = nwin ? sqrt(sum2[0] / nwin) : 0, rr = nwin ? sqrt(sum2[1] / nwin) : 0;
            printf("t=%6.1fs  écrivain:%s  trames=%6llu (%.0f/s)  rempl.max=%4u  "
                   "crête G/D %6.1f/%6.1f dB  RMS G/D %6.1f/%6.1f dB  sur-débit=%llu\n",
                   t - t0, alive ? "oui" : "NON", (unsigned long long)nwin,
                   nwin / (t - tprint + 0.5), maxfill,
                   to_db(peak[0]), to_db(peak[1]), to_db(rl), to_db(rr),
                   (unsigned long long)(PB_LOAD_RLX(&h->overruns) - ov0));
            fflush(stdout);
            peak[0] = peak[1] = sum2[0] = sum2[1] = 0; nwin = 0; maxfill = 0;
            tprint = t + 0.5;
        }
        sleep_s(0.005);
    }

    printf("Total : %llu trames en %.1f s ; sur-débits %llu ; sous-débits %llu\n",
           (unsigned long long)total, now_s() - t0,
           (unsigned long long)(PB_LOAD_RLX(&h->overruns) - ov0),
           (unsigned long long)(PB_LOAD_RLX(&h->underruns) - un0));
    if (wf) {
        wav_header(wf, rate, (uint32_t)(wav_frames * 8));
        fclose(wf);
        printf("Enregistré : %s (%llu trames)\n", wav, (unsigned long long)wav_frames);
    }
    pb_reader_release(&l, me);
    pb_link_close(&l);
    return 0;
}

/* Copie jusqu'à n trames d'un lien (lecteur) vers un autre (écrivain). */
static uint32_t copy_frames(pb_link *src, pb_link *dst, uint32_t n)
{
    pb_span rs, ws;
    uint32_t a = pb_read_reserve(src, n, &rs);
    uint32_t b = pb_write_reserve(dst, a, &ws);
    /* Copier trame par trame à travers les deux paires de segments. */
    for (uint32_t k = 0; k < b; k++) {
        const float *s = k < rs.n1 ? rs.p1 + 2 * k : rs.p2 + 2 * (k - rs.n1);
        float *d = k < ws.n1 ? ws.p1 + 2 * k : ws.p2 + 2 * (k - ws.n1);
        d[0] = s[0]; d[1] = s[1];
    }
    pb_write_commit(dst, b);
    pb_read_commit(src, a);       /* ce qui n'a pas pu être écrit est perdu */
    if (b < a) pb_count_overrun(dst, a - b);
    return b;
}

/*
 * --tone N : écrit un son test (440 Hz, -20 dBFS) dans le Return N.
 * --loop S D : relit le Send S et le recopie dans le Return D (même horloge
 *              PiPedal des deux côtés : pas de dérive, pas de rééchantillonnage).
 * Le remplissage du Return est maintenu près de 'target' trames.
 */
static int do_write(uint32_t src, uint32_t dst, uint32_t target, double seconds)
{
    pb_link in = { 0 }, out;
    char err[256];
    if (pb_link_open(&out, dst, PB_DIR_RETURN, 0, err, sizeof err) != 0) {
        fprintf(stderr, "Impossible d'ouvrir le Return %u : %s\n"
                        "(le plugin Return doit être chargé dans PiPedal)\n", dst, err);
        return 1;
    }
    if (src && pb_link_open(&in, src, PB_DIR_SEND, 0, err, sizeof err) != 0) {
        fprintf(stderr, "Impossible d'ouvrir le Send %u : %s\n", src, err);
        pb_link_close(&out);
        return 1;
    }
    uint64_t me = pb_new_owner_id();
    uint32_t rate = out.hdr->reader_rate ? out.hdr->reader_rate : 48000;
    pb_writer_take(&out, me, rate);
    pb_writer_set_target(&out, target);
    if (src) {
        pb_reader_take(&in, me, rate);
        pb_reader_resync(&in, 0);
    }

    uint64_t ov0 = PB_LOAD_RLX(&out.hdr->overruns), un0 = PB_LOAD_RLX(&out.hdr->underruns);
    double t0 = now_s(), tprint = t0 + 1.0, phase = 0.0;
    const double dphi = 2.0 * M_PI * 440.0 / rate;
    uint32_t fmin = PB_CAPACITY, fmax = 0;
    uint32_t rbeat = PB_LOAD_ACQ(&out.hdr->reader_heartbeat);
    double rbeat_t = -1.0;  /* jamais vu bouger */

    if (src) printf("Boucle Send %u -> Return %u, réserve visée %u trames (%.1f ms). Ctrl-C pour arrêter.\n",
                    src, dst, target, 1000.0 * target / rate);
    else     printf("Son test 440 Hz -> Return %u, réserve visée %u trames (%.1f ms). Ctrl-C pour arrêter.\n",
                    dst, target, 1000.0 * target / rate);

    while (!g_stop && (seconds <= 0 || now_s() - t0 < seconds)) {
        pb_writer_beat(&out, 0);
        uint64_t fill = PB_LOAD_ACQ(&out.hdr->write_idx) - PB_LOAD_ACQ(&out.hdr->read_idx);
        if (fill < fmin) fmin = (uint32_t)fill;
        if (fill > fmax) fmax = (uint32_t)fill;

        if (src) {
            pb_reader_beat(&in, 0);
            copy_frames(&in, &out, PB_CAPACITY);
        } else if (fill < target) {
            pb_span ws;
            uint32_t n = pb_write_reserve(&out, target - (uint32_t)fill, &ws);
            for (uint32_t k = 0; k < n; k++) {
                float v = (float)(0.1 * sin(phase));
                phase += dphi;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
                float *d = k < ws.n1 ? ws.p1 + 2 * k : ws.p2 + 2 * (k - ws.n1);
                d[0] = d[1] = v;
            }
            pb_write_commit(&out, n);
        }

        double t = now_s();
        uint32_t rb = PB_LOAD_ACQ(&out.hdr->reader_heartbeat);
        if (rb != rbeat) { rbeat = rb; rbeat_t = t; }
        if (t >= tprint) {
            printf("t=%6.1fs  lecteur(Return):%s  rempl. min/max=%4u/%4u  sous-débits=%llu  sur-débits=%llu\n",
                   t - t0, (rbeat_t >= 0 && t - rbeat_t < 0.25) ? "oui" : "NON", fmin, fmax,
                   (unsigned long long)(PB_LOAD_RLX(&out.hdr->underruns) - un0),
                   (unsigned long long)(PB_LOAD_RLX(&out.hdr->overruns) - ov0));
            fflush(stdout);
            fmin = PB_CAPACITY; fmax = 0;
            tprint = t + 1.0;
        }
        sleep_s(0.001);
    }
    printf("Arrêt. Sous-débits %llu, sur-débits %llu\n",
           (unsigned long long)(PB_LOAD_RLX(&out.hdr->underruns) - un0),
           (unsigned long long)(PB_LOAD_RLX(&out.hdr->overruns) - ov0));
    pb_writer_set_target(&out, 0);
    pb_writer_release(&out, me);
    pb_link_close(&out);
    if (src) { pb_reader_release(&in, me); pb_link_close(&in); }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage : pb_inspect [--watch]\n"
        "        pb_inspect --read N [--seconds S] [--wav fichier.wav]\n"
        "        pb_inspect --tone N [--target T] [--seconds S]   son test vers le Return N\n"
        "        pb_inspect --loop S D [--target T] [--seconds S] Send S -> Return D\n"
        "Dossier : %s (variable PB_SHM_DIR)\n", pb_shm_dir());
}

int main(int argc, char **argv)
{
    int watch = 0;
    long link = 0;
    double seconds = 0;
    const char *wav = NULL;
    long tone = 0, lsrc = 0, ldst = 0, target = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch")) watch = 1;
        else if (!strcmp(argv[i], "--read") && i + 1 < argc) link = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--tone") && i + 1 < argc) tone = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--loop") && i + 2 < argc) {
            lsrc = strtol(argv[++i], NULL, 10);
            ldst = strtol(argv[++i], NULL, 10);
        }
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else { usage(); return 2; }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

#define LINK_OK(x) ((x) >= 1 && (x) <= (long)PB_MAX_LINKS)
    if (target < 0 || target > (long)PB_CAPACITY / 2) { usage(); return 2; }
    if (tone) {
        if (!LINK_OK(tone)) { usage(); return 2; }
        return do_write(0, (uint32_t)tone, target ? (uint32_t)target : 1024u, seconds);
    }
    if (lsrc || ldst) {
        if (!LINK_OK(lsrc) || !LINK_OK(ldst)) { usage(); return 2; }
        return do_write((uint32_t)lsrc, (uint32_t)ldst, target ? (uint32_t)target : 512u, seconds);
    }
    if (link) {
        if (!LINK_OK(link)) { usage(); return 2; }
        return do_read((uint32_t)link, seconds, wav);
    }
    do {
        if (watch) printf("\033[H\033[2J");
        printf("Dossier : %s\n", pb_shm_dir());
        print_status();
        fflush(stdout);
        if (watch) sleep_s(1.0);
    } while (watch && !g_stop);
    return 0;
}
