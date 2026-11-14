/*
 * pb_slhit : envoie une commande à SooperLooper (ex. record) et note, à la
 * trame JACK près, chaque changement d'état de la boucle qui suit.
 *
 *   pb_slhit [port OSC, 9951] [boucle, 0] [commande, record] [secondes à surveiller, 4]
 *
 * États SooperLooper : 0 arrêt, 1 attente départ, 2 enregistrement,
 * 3 attente fin, 4 lecture, 5 overdub, ...
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <jack/transport.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static size_t pad4(size_t n) { return (n + 4) & ~(size_t)3; }
static size_t osc_str(char *p, const char *s)
{
    size_t n = strlen(s), m = pad4(n);
    memset(p, 0, m);
    memcpy(p, s, n);
    return m;
}

static int g_s, g_port, g_myport;

static void osc_send(const char *addr, const char *types, const char *a1, const char *a2, const char *a3)
{
    char msg[256];
    size_t n = osc_str(msg, addr);
    n += osc_str(msg + n, types);
    if (a1) n += osc_str(msg + n, a1);
    if (a2) n += osc_str(msg + n, a2);
    if (a3) n += osc_str(msg + n, a3);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(g_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(g_s, msg, n, 0, (struct sockaddr *)&a, sizeof a);
}

static int get_state(int loop, float *v)
{
    char addr[32], url[64];
    snprintf(addr, sizeof addr, "/sl/%d/get", loop);
    snprintf(url, sizeof url, "osc.udp://127.0.0.1:%d/", g_myport);
    osc_send(addr, ",sss", "state", url, "/r");
    char rep[256];
    ssize_t r = recv(g_s, rep, sizeof rep, 0);
    if (r < 8) return -1;
    uint32_t be;
    memcpy(&be, rep + r - 4, 4);
    be = ntohl(be);
    memcpy(v, &be, 4);
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void show(jack_client_t *c, const char *what)
{
    jack_position_t p;
    jack_transport_state_t st = jack_transport_query(c, &p);
    double beat = (p.beat - 1) + (p.ticks_per_beat > 0 ? p.tick / p.ticks_per_beat : 0);
    printf("  %-26s trame %9u | mesure %d, temps %.2f | %s\n", what, p.frame, p.bar, beat + 1,
           st == JackTransportRolling ? "lecture" : "arrêt");
}

int main(int argc, char **argv)
{
    g_port = argc > 1 ? atoi(argv[1]) : 9951;
    int loop = argc > 2 ? atoi(argv[2]) : 0;
    const char *cmd = argc > 3 ? argv[3] : "record";
    double watch = argc > 4 ? atof(argv[4]) : 4.0;

    g_s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0 };
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(g_s, (struct sockaddr *)&me, sizeof me);
    socklen_t ml = sizeof me;
    getsockname(g_s, (struct sockaddr *)&me, &ml);
    g_myport = ntohs(me.sin_port);
    struct timeval tv = { 0, 200000 };
    setsockopt(g_s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    jack_client_t *c = jack_client_open("pb_slhit", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }

    static const char *names[] = { "arrêt", "attente départ", "enregistrement", "attente fin",
                                   "lecture", "overdub", "multiply", "insert", "replace",
                                   "delay", "muet", "scratch", "lecture unique", "substitute", "pause" };
    float st = -1, prev = -1;
    get_state(loop, &prev);
    char addr[32], tag[64];
    snprintf(addr, sizeof addr, "/sl/%d/hit", loop);
    snprintf(tag, sizeof tag, "« %s » envoyé", cmd);
    if (strcmp(cmd, "-") != 0) {           /* « - » : surveiller sans rien envoyer */
        show(c, tag);
        osc_send(addr, ",s", cmd, NULL, NULL);
    }
    double t0 = now_s();
    while (now_s() - t0 < watch) {
        if (get_state(loop, &st) == 0 && st != prev) {
            int k = (int)st;
            snprintf(tag, sizeof tag, "-> %s", k >= 0 && k < 15 ? names[k] : "?");
            show(c, tag);
            prev = st;
        }
        usleep(500);
    }
    jack_client_close(c);
    return 0;
}
