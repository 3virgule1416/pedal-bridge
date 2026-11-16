/*
 * pb_timebase : maître du temps JACK (« timebase master »).
 *
 * Hydrogen 1.2.0-beta publie une mesure fausse quand il est maître (le temps
 * reste à 1, un « temps » dure une mesure entière). SooperLooper, synchronisé
 * sur « Jack/Host », se cale alors au mauvais endroit. pb_timebase publie une
 * mesure correcte (mesure, temps, tick, tempo) calculée à partir de la
 * position du transport JACK. Hydrogen garde J.Trans et éteint J.Master.
 *
 *   pb_timebase [--bpm 120] [--beats 4] [--server NOM]   démarre le maître
 *   pb_timebase --set 115                                change le tempo
 *   pb_timebase --get                                    affiche le tempo
 *
 * Le changement de tempo passe par UDP sur 127.0.0.1:9980 (texte : un nombre ;
 * « beats N » change le nombre de temps par mesure, ex. 3 pour une valse).
 * La mesure est comptée depuis la trame 0 : changer le nombre de temps
 * pendant la lecture fait sauter la position (à faire de préférence à l'arrêt).
 * Le callback JACK ne fait que du calcul.
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <jack/transport.h>

#include <arpa/inet.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CTL_PORT 9980
#define TPB      1920.0      /* ticks par temps */

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double g_bpm_req = 120.0;   /* demandé (écrit par le thread principal) */
static double g_beats = 4.0;       /* temps par mesure (écrit par le thread principal) */

/* Lecture/écriture atomiques du tempo (double : versions génériques de GCC). */
static double bpm_get(void) { double v; __atomic_load(&g_bpm_req, &v, __ATOMIC_RELAXED); return v; }
static void bpm_set(double v) { __atomic_store(&g_bpm_req, &v, __ATOMIC_RELAXED); }
static double beats_get(void) { double v; __atomic_load(&g_beats, &v, __ATOMIC_RELAXED); return v; }
static void beats_set(double v) { __atomic_store(&g_beats, &v, __ATOMIC_RELAXED); }
static jack_nframes_t g_sr = 48000;

/* Tempo vu par le callback. */
static double   cb_bpm = 120.0;

static void timebase(jack_transport_state_t state, jack_nframes_t nframes,
                     jack_position_t *pos, int new_pos, void *arg)
{
    (void)state; (void)nframes; (void)new_pos; (void)arg;
    /* Mesure calculée depuis la trame 0 avec le tempo actuel, SANS ancrage aux
     * changements de tempo : SooperLooper (synchro JACK) place sa grille de
     * cycles par fmod(trame, durée du cycle) depuis la trame 0. Avec un ancrage,
     * les deux grilles se décalent après un changement de tempo (mesuré : départs
     * à 1,98 temps au lieu de 1,00). Prix : changer le tempo pendant la lecture
     * fait sauter la position. */
    cb_bpm = bpm_get();
    const double beats = beats_get();
    const jack_nframes_t f = pos->frame;
    double total = (double)f * cb_bpm / (60.0 * g_sr);
    if (total < 0) total = 0;
    const double bars = floor(total / beats);
    const double in_bar = total - bars * beats;
    const double beat = floor(in_bar);

    pos->valid = JackPositionBBT;
    pos->bar = (int32_t)bars + 1;
    pos->beat = (int32_t)beat + 1;
    pos->tick = (int32_t)((in_bar - beat) * TPB);
    pos->bar_start_tick = bars * beats * TPB;
    pos->beats_per_bar = (float)beats;
    pos->beat_type = 4.0f;
    pos->ticks_per_beat = TPB;
    pos->beats_per_minute = cb_bpm;
}

static int g_port = CTL_PORT;

static int send_ctl(const char *msg, char *reply, size_t rlen)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(g_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
    ssize_t n = recv(s, reply, rlen - 1, 0);
    close(s);
    if (n <= 0) return -1;
    reply[n] = 0;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage : pb_timebase [--bpm 120] [--beats 4] [--server NOM] [--port 9980]\n"
                    "        pb_timebase [--port 9980] --set BPM | --get\n");
}

int main(int argc, char **argv)
{
    const char *server = NULL, *set = NULL;
    int get = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--set") && i + 1 < argc) set = argv[++i];
        else if (!strcmp(argv[i], "--get")) get = 1;
        else if (!strcmp(argv[i], "--bpm") && i + 1 < argc) g_bpm_req = atof(argv[++i]);
        else if (!strcmp(argv[i], "--beats") && i + 1 < argc) g_beats = atof(argv[++i]);
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (set || get) {
        char rep[128];
        if (send_ctl(set ? set : "?", rep, sizeof rep)) { fprintf(stderr, "pb_timebase ne répond pas\n"); return 1; }
        printf("%s\n", rep);
        return 0;
    }
    if (g_bpm_req < 20 || g_bpm_req > 400 || g_beats < 1 || g_beats > 16) { usage(); return 2; }
    cb_bpm = g_bpm_req;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(g_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("pb_timebase : port UDP 9980 (déjà lancé ?)");
        return 1;
    }
    struct timeval tv = { 0, 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    jack_status_t st;
    jack_client_t *c = server
        ? jack_client_open("pb_timebase", JackNoStartServer | JackServerName, &st, server)
        : jack_client_open("pb_timebase", JackNoStartServer, &st);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }
    g_sr = jack_get_sample_rate(c);
    if (jack_set_timebase_callback(c, 0, timebase, NULL) != 0) {
        fprintf(stderr, "Impossible de devenir maître du temps JACK\n");
        return 1;
    }
    jack_activate(c);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    printf("pb_timebase : maître du temps JACK, %.2f BPM, %g temps par mesure\n", g_bpm_req, g_beats);
    fflush(stdout);

    while (!g_stop) {
        char buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        char rep[128];
        if (!strncmp(buf, "beats ", 6)) {
            const double v = atof(buf + 6);
            if (v >= 1 && v <= 16) {
                beats_set(v);
                jack_position_t p;
                if (jack_transport_query(c, &p) == JackTransportStopped)
                    jack_transport_locate(c, p.frame);
                printf("temps par mesure : %g\n", v);
                fflush(stdout);
            }
        } else if (buf[0] != '?') {
            double v = atof(buf);
            if (v >= 20 && v <= 400) {
                bpm_set(v);
                /* Transport arrêté : JACK ne rappelle pas le maître du temps.
                 * Un repositionnement au même endroit force la mise à jour. */
                jack_position_t p;
                if (jack_transport_query(c, &p) == JackTransportStopped)
                    jack_transport_locate(c, p.frame);
                printf("tempo : %.2f BPM\n", v);
                fflush(stdout);
            }
        }
        snprintf(rep, sizeof rep, "tempo %.2f BPM, %g temps par mesure",
                 bpm_get(), beats_get());
        sendto(s, rep, strlen(rep), 0, (struct sockaddr *)&from, fl);
    }
    jack_release_timebase(c);
    jack_client_close(c);
    close(s);
    return 0;
}
