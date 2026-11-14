/*
 * pb_fakehost : faux PiPedal pour tester le client JACK sans toucher au vrai.
 *
 * Charge les vrais plugins Bridge Send et Bridge Return (dlopen) et les fait
 * tourner dans un thread temps réel cadencé par l'horloge système, avec un
 * décalage volontaire (--ppm) pour simuler la dérive entre la carte son de
 * PiPedal et l'horloge de JACK.
 *
 *   Entrée du Send   : G = sinus 440 Hz (détection de clics)
 *                      D = une impulsion par seconde (mesure de latence)
 *   Sortie du Return : mode Replace, analysée en continu.
 *
 * Avec le client branché en boucle (sendN -> returnN dans JACK), on mesure :
 *   - la latence aller-retour (impulsion émise -> impulsion reçue) ;
 *   - les clics : saut dans le sinus (résidu de la récurrence
 *     x[n+1] + x[n-1] = 2·cos(w)·x[n]) ;
 *   - les coupures (le sinus disparaît).
 *
 *   pb_fakehost pedal_bridge.so bundle/ [--link N] [--rate R] [--ppm P]
 *               [--block B] [--seconds S] [--warmup W] [--report S]
 *
 * Code de sortie 0 si, après la période de chauffe : aucun clic, aucune
 * coupure, au moins une latence mesurée et une latence stable (< 3 ms d'écart).
 */
#define _GNU_SOURCE
#include "pb_shm.h"

#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAXB 4096

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Paramètres */
static const char *g_so, *g_bundle;
static uint32_t g_link = 1, g_rate = 48000, g_block = 128;
static double g_ppm = 0, g_seconds = 30, g_warmup = 15, g_report = 5;
static double g_click_at = -1;   /* contrôle du détecteur : saut de phase à t secondes */
static const char *g_wav;        /* --wav : enregistre la sortie du Return */
static float *g_rec;             /* préalloué : seconds × rate × 2 */
static uint64_t g_rec_cap, g_rec_n;

/* Résultats (écrits par le thread audio, lus par le principal). */
typedef struct {
    uint64_t frames;
    uint64_t glitches, dropouts, starts;
    uint64_t lat_n;
    double   lat_sum, lat_min, lat_max;   /* en trames */
    double   resid_max;
    int      present;
    uint64_t late_wakeups;
    float    send_conn, ret_conn;
} Stats;

static Stats g_win;        /* fenêtre courante (remise à zéro à chaque rapport) */
static Stats g_total;      /* depuis la fin de la chauffe */
/* Verrou court entre le thread audio et l'affichage. Héritage de priorité :
 * sinon le thread d'affichage (non temps réel) peut bloquer le thread audio. */
static pthread_mutex_t g_mx;
static int g_counting;     /* 1 après la chauffe */

static void stats_reset(Stats *s) { memset(s, 0, sizeof *s); s->lat_min = 1e18; s->lat_max = -1e18; }

static void stats_add_lat(Stats *s, double l)
{
    s->lat_n++; s->lat_sum += l;
    if (l < s->lat_min) s->lat_min = l;
    if (l > s->lat_max) s->lat_max = l;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- Plugins ---- */
typedef struct {
    const LV2_Descriptor *d;
    LV2_Handle h;
    float link, mode, gain, connected;
} Plug;

static float in_l[MAXB], in_r[MAXB], thru_l[MAXB], thru_r[MAXB];
static float zero_l[MAXB], zero_r[MAXB], out_l[MAXB], out_r[MAXB];

static int plug_load(Plug *p, LV2_Descriptor_Function fn, int idx)
{
    static const LV2_Feature *none[] = { NULL };
    memset(p, 0, sizeof *p);
    p->d = fn(idx);
    if (!p->d) return -1;
    p->h = p->d->instantiate(p->d, g_rate, g_bundle, none);
    if (!p->h) return -1;
    p->link = (float)g_link;
    if (idx == 0) {               /* Send : in, out, link, gain, connected */
        p->d->connect_port(p->h, 0, in_l);   p->d->connect_port(p->h, 1, in_r);
        p->d->connect_port(p->h, 2, thru_l); p->d->connect_port(p->h, 3, thru_r);
        p->d->connect_port(p->h, 4, &p->link);
        p->d->connect_port(p->h, 5, &p->gain);
        p->d->connect_port(p->h, 6, &p->connected);
    } else {                      /* Return : in, out, link, mode, gain, connected */
        p->mode = 1;              /* Replace : on n'entend que le retour */
        p->d->connect_port(p->h, 0, zero_l); p->d->connect_port(p->h, 1, zero_r);
        p->d->connect_port(p->h, 2, out_l);  p->d->connect_port(p->h, 3, out_r);
        p->d->connect_port(p->h, 4, &p->link);
        p->d->connect_port(p->h, 5, &p->mode);
        p->d->connect_port(p->h, 6, &p->gain);
        p->d->connect_port(p->h, 7, &p->connected);
    }
    p->d->activate(p->h);
    return 0;
}

static Plug g_send, g_ret;

/* ---- Thread audio ---- */
static void *audio_thread(void *arg)
{
    (void)arg;
    const double w = 2.0 * M_PI * 440.0 / g_rate;
    const double k2c = 2.0 * cos(w);
    const double period = g_block / (g_rate * (1.0 + g_ppm * 1e-6));
    double phase = 0;
    uint64_t g = 0;                       /* trames émises */
    uint64_t pend[64]; int ph = 0, pt = 0; /* impulsions émises en attente */
    double y1 = 0, y2 = 0;                /* deux dernières valeurs du canal G */
    int quiet = 1;                        /* 1 = pas de sinus en sortie */
    uint32_t quiet_run = 0, loud_run = 0;
    /* Recherche du pic d'impulsion */
    int in_pulse = 0; uint32_t pulse_len = 0; double pk = 0; uint64_t pk_at = 0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double next_s = next.tv_sec + next.tv_nsec * 1e-9;

    while (!g_stop) {
        next_s += period;
        next.tv_sec = (time_t)next_s;
        next.tv_nsec = (long)((next_s - (double)next.tv_sec) * 1e9);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        int late = now_s() - next_s > period;   /* réveil en retard d'un bloc entier */

        if (g_click_at >= 0 && g >= (uint64_t)(g_click_at * g_rate)) {
            phase += 1.0;            /* clic volontaire dans le signal envoyé */
            g_click_at = -1;
        }
        for (uint32_t i = 0; i < g_block; i++) {
            in_l[i] = (float)(0.5 * sin(phase));
            phase += w;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
            uint64_t f = g + i;
            if (f % g_rate == 0) {
                in_r[i] = 0.9f;
                pend[ph] = f; ph = (ph + 1) % 64;
            } else in_r[i] = 0.0f;
        }
        g_send.d->run(g_send.h, g_block);
        g_ret.d->run(g_ret.h, g_block);
        if (g_rec)
            for (uint32_t i = 0; i < g_block && g_rec_n < g_rec_cap; i++) {
                g_rec[2 * g_rec_n] = out_l[i];
                g_rec[2 * g_rec_n + 1] = out_r[i];
                g_rec_n++;
            }

        pthread_mutex_lock(&g_mx);
        Stats *s[2] = { &g_win, g_counting ? &g_total : NULL };
        for (uint32_t i = 0; i < g_block; i++) {
            const uint64_t f = g + i;
            const double y = out_l[i];
            /* --- présence du sinus (amplitude 0,5) ---
             * Présent : on a vu |y| > 0,25 dans les 128 dernières trames
             * (le sinus y passe au moins tous les ~20 trames).
             * Démarrage : 256 trames présentes d'affilée. */
            if (fabs(y) > 0.25) quiet_run = 0; else quiet_run++;
            const int loud_now = quiet_run < 128;
            if (loud_now) loud_run++; else loud_run = 0;
            if (quiet && loud_run >= 256) {
                quiet = 0;
                for (int k = 0; k < 2; k++) if (s[k]) s[k]->starts++;
            }
            if (!quiet && !loud_now) {           /* plus de sinus : coupure */
                quiet = 1;
                for (int k = 0; k < 2; k++) if (s[k]) s[k]->dropouts++;
            }
            /* --- clics : résidu de la récurrence du sinus --- */
            if (!quiet) {
                double r = fabs(y + y2 - k2c * y1);
                for (int k = 0; k < 2; k++) if (s[k]) {
                    if (r > s[k]->resid_max) s[k]->resid_max = r;
                    if (r > 0.01) s[k]->glitches++;
                }
            }
            y2 = y1; y1 = y;
            /* --- latence : pic de l'impulsion sur D --- */
            const double z = fabs(out_r[i]);
            if (!in_pulse && z > 0.3) { in_pulse = 1; pulse_len = 0; pk = 0; }
            if (in_pulse) {
                if (z > pk) { pk = z; pk_at = f; }
                if (++pulse_len >= 64) {
                    in_pulse = 0;
                    /* associer à la plus ancienne impulsion émise avant ce pic */
                    while (pt != ph && pend[pt] + (uint64_t)g_rate < pk_at) pt = (pt + 1) % 64;
                    if (pt != ph && pend[pt] <= pk_at) {
                        double lat = (double)(pk_at - pend[pt]);
                        pt = (pt + 1) % 64;
                        for (int k = 0; k < 2; k++) if (s[k]) stats_add_lat(s[k], lat);
                    }
                }
            }
        }
        for (int k = 0; k < 2; k++) if (s[k]) {
            s[k]->frames += g_block;
            s[k]->present = !quiet;
            if (late) s[k]->late_wakeups++;
            s[k]->send_conn = g_send.connected;
            s[k]->ret_conn = g_ret.connected;
        }
        pthread_mutex_unlock(&g_mx);
        g += g_block;
    }
    return NULL;
}

static void print_line(const char *tag, const Stats *s)
{
    const double ms = 1000.0 / g_rate;
    if (s->lat_n)
        printf("%s latence %.2f ms (min %.2f, max %.2f, %llu mesures)", tag,
               s->lat_sum / s->lat_n * ms, s->lat_min * ms, s->lat_max * ms,
               (unsigned long long)s->lat_n);
    else
        printf("%s latence -", tag);
    printf(" | clics %llu, coupures %llu, départs %llu, résidu max %.1e, réveils en retard %llu"
           " | Send:%s Return:%s\n",
           (unsigned long long)s->glitches, (unsigned long long)s->dropouts,
           (unsigned long long)s->starts, s->resid_max, (unsigned long long)s->late_wakeups,
           s->send_conn > 0.5f ? "connecté" : "-", s->ret_conn > 0.5f ? "connecté" : "-");
}

int main(int argc, char **argv)
{
    if (argc < 3) goto usage;
    g_so = argv[1];
    g_bundle = argv[2];
    for (int i = 3; i < argc; i++) {
        if (i + 1 >= argc) goto usage;
        const char *a = argv[i], *v = argv[++i];
        if (!strcmp(a, "--link")) g_link = (uint32_t)atoi(v);
        else if (!strcmp(a, "--rate")) g_rate = (uint32_t)atoi(v);
        else if (!strcmp(a, "--ppm")) g_ppm = atof(v);
        else if (!strcmp(a, "--block")) g_block = (uint32_t)atoi(v);
        else if (!strcmp(a, "--seconds")) g_seconds = atof(v);
        else if (!strcmp(a, "--warmup")) g_warmup = atof(v);
        else if (!strcmp(a, "--report")) g_report = atof(v);
        else if (!strcmp(a, "--click-at")) g_click_at = atof(v);
        else if (!strcmp(a, "--wav")) g_wav = v;
        else goto usage;
    }
    if (g_block == 0 || g_block > MAXB || g_link < 1 || g_link > PB_MAX_LINKS) goto usage;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    mlockall(MCL_CURRENT | MCL_FUTURE);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_mx, &ma);

    void *so = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
    if (!fn || plug_load(&g_send, fn, 0) || plug_load(&g_ret, fn, 1)) {
        fprintf(stderr, "Chargement des plugins impossible\n");
        return 1;
    }
    stats_reset(&g_win);
    stats_reset(&g_total);
    if (g_wav) {
        g_rec_cap = (uint64_t)((g_seconds + 1) * g_rate);
        g_rec = calloc(g_rec_cap * 2, sizeof(float));
        if (!g_rec) { fprintf(stderr, "mémoire insuffisante pour --wav\n"); return 1; }
    }

    printf("pb_fakehost : %u Hz %+.0f ppm, blocs de %u, lien %u, %.0f s (chauffe %.0f s)\n",
           g_rate, g_ppm, g_block, g_link, g_seconds, g_warmup);
    fflush(stdout);

    pthread_t th;
    pthread_attr_t at;
    struct sched_param sp = { .sched_priority = 70 };
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_FIFO);
    pthread_attr_setschedparam(&at, &sp);
    if (pthread_create(&th, &at, audio_thread, NULL) != 0) {
        fprintf(stderr, "Avertissement : pas de SCHED_FIFO, thread normal\n");
        pthread_create(&th, NULL, audio_thread, NULL);
    }

    const double t0 = now_s();
    double next_rep = t0 + g_report;
    while (!g_stop && now_s() - t0 < g_seconds) {
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
        double t = now_s();
        if (!g_counting && t - t0 >= g_warmup) {
            pthread_mutex_lock(&g_mx);
            g_counting = 1;
            pthread_mutex_unlock(&g_mx);
        }
        if (t >= next_rep) {
            pthread_mutex_lock(&g_mx);
            Stats s = g_win;
            stats_reset(&g_win);
            pthread_mutex_unlock(&g_mx);
            char tag[32];
            snprintf(tag, sizeof tag, "[%5.0fs]%s", t - t0, g_counting ? "" : " (chauffe)");
            print_line(tag, &s);
            fflush(stdout);
            next_rep = t + g_report;
        }
    }
    g_stop = 1;
    pthread_join(th, NULL);

    g_send.d->deactivate(g_send.h); g_send.d->cleanup(g_send.h);
    g_ret.d->deactivate(g_ret.h);   g_ret.d->cleanup(g_ret.h);

    if (g_rec) {
        FILE *wf = fopen(g_wav, "wb");
        if (wf) {
            uint32_t bytes = (uint32_t)(g_rec_n * 8), v32;
            uint16_t v16;
            fwrite("RIFF", 1, 4, wf); v32 = 36 + bytes; fwrite(&v32, 4, 1, wf);
            fwrite("WAVEfmt ", 1, 8, wf); v32 = 16; fwrite(&v32, 4, 1, wf);
            v16 = 3; fwrite(&v16, 2, 1, wf); v16 = 2; fwrite(&v16, 2, 1, wf);
            v32 = g_rate; fwrite(&v32, 4, 1, wf); v32 = g_rate * 8; fwrite(&v32, 4, 1, wf);
            v16 = 8; fwrite(&v16, 2, 1, wf); v16 = 32; fwrite(&v16, 2, 1, wf);
            fwrite("data", 1, 4, wf); fwrite(&bytes, 4, 1, wf);
            fwrite(g_rec, 8, g_rec_n, wf);
            fclose(wf);
            printf("Sortie du Return enregistrée : %s (%.1f s)\n", g_wav, (double)g_rec_n / g_rate);
        }
    }
    Stats s = g_total;
    print_line("TOTAL après chauffe :", &s);
    const double jitter_ms = s.lat_n ? (s.lat_max - s.lat_min) * 1000.0 / g_rate : 1e9;
    int ok = s.glitches == 0 && s.dropouts == 0 && s.lat_n > 0 && jitter_ms < 3.0;
    printf("RÉSULTAT : %s\n", ok ? "OK" : "ÉCHEC");
    return ok ? 0 : 1;

usage:
    fprintf(stderr, "usage : pb_fakehost pedal_bridge.so bundle/ [--link N] [--rate R] [--ppm P]"
                    " [--block B] [--seconds S] [--warmup W] [--report S] [--click-at T]\n");
    return 2;
}
