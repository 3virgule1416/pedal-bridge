/*
 * pb_lag : décalage entre deux ports JACK qui portent le même son (ex. la
 * batterie d'Hydrogen et la même batterie rejouée par une boucle).
 *
 *   pb_lag PORT_A PORT_B [secondes, défaut 4] [plage ms, défaut 60]
 *
 * Enregistre les deux ports en même temps, puis cherche par corrélation le
 * décalage qui les superpose le mieux. Positif : B est EN RETARD sur A.
 * Lecture seulement : deux ports d'entrée, rien n'est envoyé nulle part.
 */
#define _GNU_SOURCE
#include <jack/jack.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jack_port_t *g_in[2];
static float *g_buf[2];
static size_t g_cap, g_n;               /* g_n : écrit par le callback seulement */

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    size_t k = __atomic_load_n(&g_n, __ATOMIC_RELAXED);
    if (k >= g_cap) return 0;
    size_t m = n;
    if (m > g_cap - k) m = g_cap - k;
    for (int c = 0; c < 2; c++)
        memcpy(g_buf[c] + k, jack_port_get_buffer(g_in[c], n), m * sizeof(float));
    __atomic_store_n(&g_n, k + m, __ATOMIC_RELEASE);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage : pb_lag PORT_A PORT_B [secondes] [plage ms]\n");
        return 2;
    }
    double secs = argc > 3 ? atof(argv[3]) : 4.0;
    double range_ms = argc > 4 ? atof(argv[4]) : 60.0;

    jack_client_t *c = jack_client_open("pb_lag", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }
    const double sr = jack_get_sample_rate(c);
    g_cap = (size_t)(secs * sr);
    for (int i = 0; i < 2; i++) {
        g_buf[i] = calloc(g_cap, sizeof(float));
        char nm[8];
        snprintf(nm, sizeof nm, "in%c", 'A' + i);
        g_in[i] = jack_port_register(c, nm, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    if (!g_buf[0] || !g_buf[1]) { fprintf(stderr, "mémoire\n"); return 1; }
    jack_set_process_callback(c, process, NULL);
    jack_activate(c);
    for (int i = 0; i < 2; i++)
        if (jack_connect(c, argv[1 + i], jack_port_name(g_in[i]))) {
            fprintf(stderr, "Connexion à %s impossible\n", argv[1 + i]);
            jack_client_close(c);
            return 1;
        }
    while (__atomic_load_n(&g_n, __ATOMIC_ACQUIRE) < g_cap) usleep(20000);
    jack_client_close(c);

    const float *a = g_buf[0], *b = g_buf[1];
    const long L = (long)(range_ms * sr / 1000.0);
    const long n = (long)g_cap;
    double ea = 0, eb = 0;
    for (long i = L; i < n - L; i++) { ea += (double)a[i] * a[i]; eb += (double)b[i] * b[i]; }
    if (ea < 1e-9 || eb < 1e-9) {
        printf("Signal trop faible : A %.2e, B %.2e (rien ne joue ?)\n", ea, eb);
        return 1;
    }
    double *xc = malloc(sizeof(double) * (size_t)(2 * L + 1));
    if (!xc) { fprintf(stderr, "mémoire\n"); return 1; }
    long best = 0;
    double bestv = -1e300;
    for (long lag = -L; lag <= L; lag++) {   /* b[i + lag] ≈ a[i] : B en retard de lag */
        double s = 0;
        for (long i = L; i < n - L; i++) s += (double)a[i] * b[i + lag];
        xc[lag + L] = s;
        if (s > bestv) { bestv = s; best = lag; }
    }
    const double r = bestv / sqrt(ea * eb);
    printf("Décalage : B %s A de %ld trames = %.2f ms (corrélation %.3f, sur %.1f s)\n",
           best >= 0 ? "en retard sur" : "en avance sur", labs(best), labs(best) * 1000.0 / sr, r, secs);
    /* Deuxième pic (maximum local à plus de 1 ms du premier) : une deuxième
     * copie décalée du même son, par exemple une couche d'overdub mal placée. */
    const long gap = (long)(sr / 1000.0);
    long best2 = 0;
    double v2 = -1e300;
    for (long k = 1; k < 2 * L; k++) {
        const long lag = k - L;
        if (labs(lag - best) <= gap) continue;
        if (xc[k] > xc[k - 1] && xc[k] >= xc[k + 1] && xc[k] > v2) { v2 = xc[k]; best2 = lag; }
    }
    if (v2 > 0)
        printf("2e pic     : %+ld trames = %+.2f ms, hauteur %.0f %% du premier\n",
               best2, best2 * 1000.0 / sr, 100.0 * v2 / bestv);
    free(xc);
    return 0;
}
