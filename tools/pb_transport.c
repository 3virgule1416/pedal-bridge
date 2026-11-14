/*
 * pb_transport : pilote le transport JACK (pour les tests).
 *   pb_transport start | stop | zero | query
 * Serveur : variable JACK_DEFAULT_SERVER.
 */
#include <jack/jack.h>
#include <jack/transport.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage : pb_transport start|stop|zero|query\n"); return 2; }
    jack_client_t *c = jack_client_open("pb_transport", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "Impossible de joindre JACK\n"); return 1; }
    if (!strcmp(argv[1], "start")) jack_transport_start(c);
    else if (!strcmp(argv[1], "stop")) jack_transport_stop(c);
    else if (!strcmp(argv[1], "zero")) jack_transport_locate(c, 0);
    else if (!strcmp(argv[1], "query")) {
        jack_position_t p;
        jack_transport_state_t s = jack_transport_query(c, &p);
        printf("%s frame %u mesure %d temps %d tick %d tempo %.2f\n",
               s == JackTransportRolling ? "lecture" : "arrêt", p.frame,
               p.bar, p.beat, p.tick, p.beats_per_minute);
    } else { jack_client_close(c); return 2; }
    jack_client_close(c);
    return 0;
}
