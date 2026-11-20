/*
 * pb_garde : gardien du double switch et du Captain.
 *
 * Tant que pedal_bridge tourne, il prend le double switch de l'Axe à PiPedal
 * (connexion MIDI Axe -> PiPedal débranchée) et fait changer de page le
 * Captain. S'il s'arrête sans les rendre (plantage, kill -9), pb_garde le
 * voit dans la seconde et remet tout en mode PiPedal normal :
 *   - rebranche l'Axe à PiPedal (A et B changent de preset comme avant) ;
 *   - remet le Captain sur la page 0 (CC 20 = 0, canal 16), une fois.
 * Il ne touche à rien tant que le pont tourne.
 *
 *   pb_garde            (lancé par scripts/session.sh)
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Un processus « pedal_bridge » du même utilisateur tourne-t-il ? */
static int bridge_alive(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 1;                                  /* dans le doute : ne rien faire */
    const uid_t me = getuid();
    int alive = 0;
    struct dirent *e;
    while (!alive && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        if (strcmp(comm, "pedal_bridge") != 0) continue;
        snprintf(path, sizeof path, "/proc/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_uid == me) alive = 1;
    }
    closedir(d);
    return alive;
}

/* Port d'un client ALSA dont le nom commence par name, avec ces capacités. */
static int find(snd_seq_t *seq, const char *name, unsigned need, int *client, int *port)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        if (strncmp(snd_seq_client_info_get_name(ci), name, strlen(name)) != 0) continue;
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0)
            if ((snd_seq_port_info_get_capability(pi) & need) == need) {
                *client = c;
                *port = snd_seq_port_info_get_port(pi);
                return 1;
            }
    }
    return 0;
}

int main(void)
{
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { fprintf(stderr, "pb_garde : ALSA indisponible\n"); return 1; }
    snd_seq_set_client_name(seq, "pb_garde");
    const int out = snd_seq_create_simple_port(seq, "garde", SND_SEQ_PORT_CAP_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    printf("pb_garde : en veille (pont absent -> double switch à PiPedal, Captain page 0)\n");
    fflush(stdout);
    int was_alive = bridge_alive(), page_sent = !was_alive;   /* au départ sans pont : la page n'est pas à nous */
    while (!g_stop) {
        sleep(1);
        const int alive = bridge_alive();
        if (alive) { was_alive = 1; page_sent = 0; continue; }
        const unsigned rd = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
        const unsigned wr = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
        int ac, ap, pc, pp, cc, cp;
        /* Axe -> PiPedal : rebrancher si la connexion manque. */
        if (find(seq, "AXE IO One", rd, &ac, &ap) && find(seq, "PiPedal", wr, &pc, &pp)) {
            snd_seq_port_subscribe_t *sub;
            snd_seq_port_subscribe_alloca(&sub);
            snd_seq_addr_t s = {(unsigned char)ac, (unsigned char)ap}, d = {(unsigned char)pc, (unsigned char)pp};
            snd_seq_port_subscribe_set_sender(sub, &s);
            snd_seq_port_subscribe_set_dest(sub, &d);
            if (snd_seq_get_port_subscription(seq, sub) != 0 && snd_seq_subscribe_port(seq, sub) == 0) {
                printf("pb_garde : pont absent, double switch rendu à PiPedal\n");
                fflush(stdout);
            }
        }
        /* Captain sur la page 0, une fois après la disparition du pont. */
        if (was_alive && !page_sent && find(seq, "Feather RP2040", wr, &cc, &cp)) {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_controller(&ev, 15, 20, 0);
            snd_seq_ev_set_source(&ev, out);
            snd_seq_ev_set_dest(&ev, cc, cp);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
            page_sent = 1;
            printf("pb_garde : pont absent, Captain remis sur la page 0\n");
            fflush(stdout);
        }
    }
    snd_seq_close(seq);
    return 0;
}
