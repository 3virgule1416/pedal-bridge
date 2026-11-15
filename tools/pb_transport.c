/*
 * pb_transport : pilote le transport JACK.
 *
 *   pb_transport start | stop | zero | query
 *   pb_transport pause  [port OSC SooperLooper, défaut 9951]
 *   pb_transport resume [port OSC SooperLooper, défaut 9951]
 *
 * pause  : arrête le transport ET met toutes les boucles en pause (pause_on).
 * resume : relance toutes les boucles (pause_off) ET le transport, sans
 *          revenir au début : batterie et boucles repartent d'où elles étaient.
 * Les deux ordres partent à quelques microsecondes d'intervalle (client JACK
 * et socket OSC ouverts d'avance), pour que batterie et boucles restent calées.
 * Pas de « trigger » : une boucle relancée par trigger perd la compensation de
 * latence de SooperLooper (22,7 ms mesurés avec pb_lag).
 *
 * Serveur : variable JACK_DEFAULT_SERVER.
 */
#include <jack/jack.h>
#include <jack/transport.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t osc_str(char *p, const char *s)
{
    size_t n = strlen(s), m = (n + 4) & ~(size_t)3;
    memset(p, 0, m);
    memcpy(p, s, n);
    return m;
}

/* Message OSC « /sl/-1/hit <cmd> » prêt à envoyer. */
static size_t osc_hit_all(char *msg, const char *cmd)
{
    size_t n = osc_str(msg, "/sl/-1/hit");
    n += osc_str(msg + n, ",s");
    n += osc_str(msg + n, cmd);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage : pb_transport start|stop|zero|query|pause|resume [port OSC]\n");
        return 2;
    }
    const char *cmd = argv[1];
    jack_client_t *c = jack_client_open("pb_transport", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }

    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume")) {
        const int port = argc > 2 ? atoi(argv[2]) : 9951;
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char msg[64];
        if (!strcmp(cmd, "pause")) {
            const size_t n = osc_hit_all(msg, "pause_on");
            jack_transport_stop(c);
            sendto(s, msg, n, 0, (struct sockaddr *)&a, sizeof a);
        } else {
            const size_t n = osc_hit_all(msg, "pause_off");
            sendto(s, msg, n, 0, (struct sockaddr *)&a, sizeof a);
            jack_transport_start(c);
        }
        close(s);
    }
    else if (!strcmp(cmd, "start")) jack_transport_start(c);
    else if (!strcmp(cmd, "stop")) jack_transport_stop(c);
    else if (!strcmp(cmd, "zero")) jack_transport_locate(c, 0);
    else if (!strcmp(cmd, "query")) {
        jack_position_t p;
        jack_transport_state_t s = jack_transport_query(c, &p);
        printf("%s frame %u mesure %d temps %d tick %d tempo %.2f\n",
               s == JackTransportRolling ? "lecture" : "arrêt", p.frame,
               p.bar, p.beat, p.tick, p.beats_per_minute);
    } else { jack_client_close(c); return 2; }
    jack_client_close(c);
    return 0;
}
