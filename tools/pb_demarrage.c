/*
 * pb_demarrage : fenêtre de choix au démarrage du Pi (mode spectacle).
 *
 * Dès que le MIDI Captain est là, la DEL du bouton 10 clignote en orange
 * pendant 8 s. Un appui sur le bouton 10 (page PEDAL : CC 10, canal 1) choisit
 * le **mode spectacle** : bouton 10 en vert, code de sortie 1 (la session —
 * JACK, Hydrogen, SooperLooper, le pont — n'est pas lancée : PiPedal seul).
 * Sans appui : code 0 (session complète). Captain absent après 30 s : code 0.
 *
 * Pendant la fenêtre, /dev/shm/pb-demarrage existe : pb_leds ne touche pas au
 * bouton 10.
 *
 *   pb_demarrage [secondes, défaut 8]
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *FLAG = "/dev/shm/pb-demarrage";
static snd_seq_t *seq;
static int out_port, in_port, cap_c = -1, cap_p = -1;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int find_captain(void)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        if (strncmp(snd_seq_client_info_get_name(ci), "Feather RP2040", 14) != 0) continue;
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_READ;
            if ((snd_seq_port_info_get_capability(pi) & need) == need) {
                cap_c = c;
                cap_p = snd_seq_port_info_get_port(pi);
                return 1;
            }
        }
    }
    return 0;
}

static void send_ev(snd_seq_event_t *ev)
{
    snd_seq_ev_set_source(ev, out_port);
    snd_seq_ev_set_dest(ev, cap_c, cap_p);
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output_direct(seq, ev);
}

static void key10(int r, int g, int b)            /* 3 DEL du bouton 10, RVB 8 bits */
{
    unsigned char m[15] = {0xF0, 0x00, 0x04, 0x10, 10};
    for (int i = 0; i < 3; i++) { m[5 + 3 * i] = r / 2; m[6 + 3 * i] = g / 2; m[7 + 3 * i] = b / 2; }
    m[14] = 0xF7;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_sysex(&ev, sizeof m, m);
    send_ev(&ev);
}

int main(int argc, char **argv)
{
    const double window = argc > 1 ? atof(argv[1]) : 8.0;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return 0;
    snd_seq_set_client_name(seq, "pb_demarrage");
    out_port = snd_seq_create_simple_port(seq, "del", SND_SEQ_PORT_CAP_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    in_port = snd_seq_create_simple_port(seq, "boutons", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                         SND_SEQ_PORT_TYPE_APPLICATION);
    const double t0 = now_s();
    while (!find_captain()) {
        if (now_s() - t0 > 30) { printf("pb_demarrage : Captain absent, démarrage normal\n"); return 0; }
        usleep(500000);
    }
    usleep(1500000);                                   /* le Captain finit de démarrer */
    snd_seq_connect_from(seq, in_port, cap_c, cap_p);
    FILE *f = fopen(FLAG, "w");
    if (f) fclose(f);
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, 15, 20, 0);        /* page PEDAL (canal 16, CC 20 = 0) */
    send_ev(&ev);
    printf("pb_demarrage : bouton 10 pendant %.0f s = mode spectacle\n", window);
    fflush(stdout);
    int show = 0, lit = -1;
    const double start = now_s();
    while (!show && now_s() - start < window) {
        const int on = (int)((now_s() - start) * 4) % 2 == 0;   /* clignote 2 fois par seconde */
        if (on != lit) { key10(on ? 255 : 0, on ? 110 : 0, 0); lit = on; }
        snd_seq_event_t *in = NULL;
        while (snd_seq_event_input(seq, &in) >= 0 && in) {
            if (in->type == SND_SEQ_EVENT_CONTROLLER && in->data.control.channel == 0 &&
                in->data.control.param == 10 && in->data.control.value > 0)
                show = 1;
            in = NULL;
        }
        usleep(20000);
    }
    if (show) {
        key10(0, 255, 0);
        printf("pb_demarrage : MODE SPECTACLE (PiPedal seul, sans batterie ni looper)\n");
        fflush(stdout);
        sleep(2);
        key10(0, 0, 0);                                /* puis éteint : pb_leds remet sa couleur */
    } else {
        key10(0, 0, 0);
        printf("pb_demarrage : démarrage normal (session complète)\n");
    }
    unlink(FLAG);
    snd_seq_close(seq);
    return show ? 1 : 0;
}
