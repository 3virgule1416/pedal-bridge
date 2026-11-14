/*
 * pb_onsets : écoute un port JACK (ex. Hydrogen:out_L) et note sur quel temps
 * de la mesure JACK tombe chaque attaque (coup de batterie).
 *
 *   pb_onsets [port source, défaut Hydrogen:out_L] [secondes, défaut 8]
 *
 * Lecture seulement : un port d'entrée, rien n'est envoyé nulle part.
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <jack/transport.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXEV 4096

typedef struct { int bar; double beat; float level; } Ev;

static jack_client_t *g_c;
static jack_port_t *g_in;
static Ev g_ev[MAXEV];
static unsigned g_n;                   /* écrit par le callback seulement */
static unsigned g_quiet = 100000;      /* trames sous le seuil bas */
static int g_armed = 1;

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    const float *x = jack_port_get_buffer(g_in, n);
    jack_position_t p;
    jack_transport_state_t st = jack_transport_query(g_c, &p);
    if (st != JackTransportRolling || !(p.valid & JackPositionBBT)) return 0;
    const double fpb = p.frame_rate * 60.0 / p.beats_per_minute;     /* trames par temps */
    const double beat0 = (p.beat - 1) + p.tick / p.ticks_per_beat;  /* temps au début du bloc */
    for (jack_nframes_t i = 0; i < n; i++) {
        const float a = fabsf(x[i]);
        if (a < 0.01f) { if (g_quiet < 1000000) g_quiet++; }
        else g_quiet = 0;
        if (g_quiet > 2400) g_armed = 1;                 /* 50 ms de calme : prêt */
        if (g_armed && a > 0.1f) {
            g_armed = 0;
            unsigned k = __atomic_load_n(&g_n, __ATOMIC_RELAXED);
            if (k < MAXEV) {
                double b = beat0 + i / fpb;
                int bar = p.bar;
                while (b >= p.beats_per_bar) { b -= p.beats_per_bar; bar++; }
                g_ev[k].bar = bar; g_ev[k].beat = b + 1; g_ev[k].level = a;
                __atomic_store_n(&g_n, k + 1, __ATOMIC_RELEASE);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *src = argc > 1 ? argv[1] : "Hydrogen:out_L";
    double secs = argc > 2 ? atof(argv[2]) : 8.0;
    g_c = jack_client_open("pb_onsets", JackNoStartServer, NULL);
    if (!g_c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }
    g_in = jack_port_register(g_c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(g_c, process, NULL);
    jack_activate(g_c);
    if (jack_connect(g_c, src, jack_port_name(g_in))) {
        fprintf(stderr, "Connexion à %s impossible\n", src);
        jack_client_close(g_c);
        return 1;
    }
    unsigned shown = 0;
    for (double t = 0; t < secs; t += 0.05) {
        usleep(50000);
        unsigned k = __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
        for (; shown < k; shown++)
            printf("  attaque mesure %3d, temps %.3f (niveau %.2f)\n",
                   g_ev[shown].bar, g_ev[shown].beat, g_ev[shown].level);
        fflush(stdout);
    }
    if (!shown) printf("  aucune attaque (Hydrogen joue-t-il ? transport en lecture ?)\n");
    jack_client_close(g_c);
    return 0;
}
