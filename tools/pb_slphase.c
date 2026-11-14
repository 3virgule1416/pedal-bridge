/*
 * pb_slphase : mesure le décalage entre le début d'une boucle SooperLooper et
 * la barre de mesure du transport JACK (pour les tests de synchro).
 *
 *   pb_slphase [port OSC SooperLooper, défaut 9951] [boucle, défaut 0] [n mesures, défaut 5]
 *
 * Lit la position JACK (mesure, temps, tick), puis demande loop_pos et loop_len
 * à SooperLooper par OSC. Décalage = position dans la mesure - (loop_pos mod mesure).
 * 0 ms : la boucle commence pile sur le 1.
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <jack/transport.h>

#include <arpa/inet.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static size_t pad4(size_t n) { return (n + 4) & ~(size_t)3; }   /* chaîne + \0, arrondi à 4 */

static size_t osc_str(char *p, const char *s)
{
    size_t n = strlen(s), m = pad4(n);
    memset(p, 0, m);
    memcpy(p, s, n);
    return m;
}

/* Envoie /sl/N/get <param> <url> /r et lit la valeur (float) de la réponse. */
static int sl_get(int s, int port, int myport, int loop, const char *param, float *out)
{
    char msg[256], addr[32], url[64];
    snprintf(addr, sizeof addr, "/sl/%d/get", loop);
    snprintf(url, sizeof url, "osc.udp://127.0.0.1:%d/", myport);
    size_t n = osc_str(msg, addr);
    n += osc_str(msg + n, ",sss");
    n += osc_str(msg + n, param);
    n += osc_str(msg + n, url);
    n += osc_str(msg + n, "/r");
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, msg, n, 0, (struct sockaddr *)&a, sizeof a);
    char rep[256];
    ssize_t r = recv(s, rep, sizeof rep, 0);
    if (r < 8) return -1;
    uint32_t be;
    memcpy(&be, rep + r - 4, 4);           /* dernier argument : le float */
    be = ntohl(be);
    memcpy(out, &be, 4);
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 9951;
    int loop = argc > 2 ? atoi(argv[2]) : 0;
    int count = argc > 3 ? atoi(argv[3]) : 5;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0 };
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&me, sizeof me);
    socklen_t ml = sizeof me;
    getsockname(s, (struct sockaddr *)&me, &ml);
    int myport = ntohs(me.sin_port);
    struct timeval tv = { 0, 300000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    jack_client_t *c = jack_client_open("pb_slphase", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }

    float len = 0;
    if (sl_get(s, port, myport, loop, "loop_len", &len) || len <= 0) {
        fprintf(stderr, "Pas de boucle %d dans SooperLooper (port %d)\n", loop, port);
        return 1;
    }
    for (int k = 0; k < count; k++) {
        jack_position_t p;
        jack_transport_query(c, &p);
        double t0 = now_s();
        float pos;
        if (sl_get(s, port, myport, loop, "loop_pos", &pos)) { fprintf(stderr, "pas de réponse\n"); return 1; }
        double rtt = now_s() - t0;
        if (!(p.valid & JackPositionBBT) || p.beats_per_minute <= 0) { fprintf(stderr, "pas de mesure JACK\n"); return 1; }
        const double beat_s = 60.0 / p.beats_per_minute;
        const double bar_s = beat_s * p.beats_per_bar;
        const double in_bar = ((p.beat - 1) + p.tick / p.ticks_per_beat) * beat_s;
        double d = fmod(in_bar - fmod(pos, bar_s) + 2 * bar_s, bar_s);
        if (d > bar_s / 2) d -= bar_s;
        printf("mesure %d temps %d | boucle %.3f s / %.3f s (%.2f mesures) | décalage %+7.1f ms"
               " (%+.2f temps) | aller-retour OSC %.1f ms\n",
               p.bar, p.beat, pos, len, len / bar_s, d * 1000, d / beat_s, rtt * 1000);
        usleep(700000);
    }
    jack_client_close(c);
    return 0;
}
