/*
 * pb_ctl : lit le fichier de contrôle /dev/shm/pedal-bridge-control et joue
 * le rôle de DLS Looper (pour les tests et le dépannage).
 *
 *   pb_ctl show                 état publié par le client (une ligne)
 *   pb_ctl create               crée le fichier (banc de test seulement : en
 *                               vrai, c'est le plugin qui le crée)
 *   pb_ctl take                 devient le « plugin » propriétaire
 *   pb_ctl press NOM            appui : record overdub undo redo mute effacer play
 *   pb_ctl loop N               boucle choisie (1-8)
 *   pb_ctl tempo BPM            réglage Tempo
 *   pb_ctl drum-take            devient le « DLS Drum » propriétaire
 *   pb_ctl drum-press NOM       appui : play prev next tap bpm+ bpm-
 *   pb_ctl drum-groove N        menu Groove (1-160)
 *   pb_ctl drum-tempo BPM       réglage Tempo de DLS Drum
 *   pb_ctl drum-metro 0|1       interrupteur Metronome de DLS Drum
 *
 * Dossier : $PB_SHM_DIR (ou /dev/shm). Avec un vrai DLS Looper chargé, press
 * incrémente le compteur à sa place (pour le dépannage seulement).
 */
#define _GNU_SOURCE
#include "pb_ctl.h"
#include "pb_shm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *STATES[PB_LS_COUNT] = {
    "hors-ligne", "vide", "attente", "enregistre", "overdub", "lecture", "muet", "pause", "autre"
};
static const char *BTN[PB_BTN_COUNT] = { "record", "overdub", "undo", "redo", "mute", "effacer", "play" };
static const char *DBTN[PB_DBTN_COUNT] = { "play", "prev", "next", "tap", "bpm+", "bpm-" };

static int usage(void)
{
    fprintf(stderr, "usage : pb_ctl show | create | take | press NOM | loop N | tempo BPM\n"
                    "        pb_ctl drum-take | drum-press NOM | drum-groove N | drum-tempo BPM | drum-metro 0|1\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    pb_ctl *c;
    char err[256];
    if (pb_ctl_open(&c, !strcmp(cmd, "create"), err, sizeof err) != 0) {
        fprintf(stderr, "pb_ctl : %s\n", err);
        return 1;
    }
    if (!strcmp(cmd, "create") || !strcmp(cmd, "take")) {
        PB_STORE_REL(&c->looper_owner, pb_new_owner_id());
        printf("ok propriétaire\n");
    } else if (!strcmp(cmd, "show")) {
        /* Le client est actif si son heartbeat bouge. */
        const uint32_t h0 = PB_LOAD_ACQ(&c->client_heartbeat);
        usleep(50000);
        const int alive = PB_LOAD_ACQ(&c->client_heartbeat) != h0;
        printf("pont=%d sl=%u lecture=%u mesure=%u temps=%u bpm=%.2f boucle=%u reglage_tempo=%.2f",
               alive, PB_LOAD_RLX(&c->sl_ok), PB_LOAD_RLX(&c->rolling), PB_LOAD_RLX(&c->bar),
               PB_LOAD_RLX(&c->beat), PB_LOAD_RLX(&c->bpm) * 0.01, PB_LOAD_RLX(&c->looper_loop),
               PB_LOAD_RLX(&c->looper_tempo) * 0.01);
        const uint32_t n = PB_LOAD_RLX(&c->loop_count);
        for (uint32_t i = 0; i < PB_CTL_LOOPS && i < (n ? n : 1); i++) {
            uint32_t s = PB_LOAD_RLX(&c->loop_state[i]);
            printf(" b%u=%s", i + 1, s < PB_LS_COUNT ? STATES[s] : "?");
        }
        printf(" proprietaire=%s h2=%u groove=%u/%u drum=%s\n", PB_LOAD_RLX(&c->looper_owner) ? "oui" : "non",
               PB_LOAD_RLX(&c->h2_ok), PB_LOAD_RLX(&c->groove_cur), PB_LOAD_RLX(&c->groove_count),
               PB_LOAD_RLX(&c->drum_owner) ? "oui" : "non");
    } else if (!strcmp(cmd, "drum-take")) {
        PB_STORE_REL(&c->drum_owner, pb_new_owner_id());
        printf("ok propriétaire drum\n");
    } else if (!strcmp(cmd, "drum-press") && argc > 2) {
        int b = -1;
        for (int i = 0; i < PB_DBTN_COUNT; i++) if (!strcmp(argv[2], DBTN[i])) b = i;
        if (b < 0) { pb_ctl_close(c); return usage(); }
        PB_STORE_REL(&c->drum_btn[b], PB_LOAD_RLX(&c->drum_btn[b]) + 1u);
        printf("ok drum %s\n", DBTN[b]);
    } else if (!strcmp(cmd, "drum-groove") && argc > 2) {
        const int v = atoi(argv[2]);
        if (v < 1 || v > (int)PB_CTL_GROOVES) { pb_ctl_close(c); return usage(); }
        PB_STORE_RLX(&c->drum_groove, (uint32_t)v);
        printf("ok groove %d\n", v);
    } else if (!strcmp(cmd, "drum-tempo") && argc > 2) {
        const double v = atof(argv[2]);
        if (v < 40 || v > 240) { pb_ctl_close(c); return usage(); }
        PB_STORE_RLX(&c->drum_tempo, (uint32_t)lround(v * 100));
        printf("ok tempo drum %.2f\n", v);
    } else if (!strcmp(cmd, "drum-metro") && argc > 2) {
        PB_STORE_RLX(&c->drum_metro, atoi(argv[2]) ? 1u : 0u);
        printf("ok métronome %d\n", atoi(argv[2]) ? 1 : 0);
    } else if (!strcmp(cmd, "press") && argc > 2) {
        int b = -1;
        for (int i = 0; i < PB_BTN_COUNT; i++) if (!strcmp(argv[2], BTN[i])) b = i;
        if (b < 0) { pb_ctl_close(c); return usage(); }
        PB_STORE_REL(&c->looper_btn[b], PB_LOAD_RLX(&c->looper_btn[b]) + 1u);
        printf("ok %s\n", BTN[b]);
    } else if (!strcmp(cmd, "loop") && argc > 2) {
        const int v = atoi(argv[2]);
        if (v < 1 || v > (int)PB_CTL_LOOPS) { pb_ctl_close(c); return usage(); }
        PB_STORE_RLX(&c->looper_loop, (uint32_t)v);
        printf("ok boucle %d\n", v);
    } else if (!strcmp(cmd, "tempo") && argc > 2) {
        const double v = atof(argv[2]);
        if (v < 40 || v > 240) { pb_ctl_close(c); return usage(); }
        PB_STORE_RLX(&c->looper_tempo, (uint32_t)lround(v * 100));
        printf("ok tempo %.2f\n", v);
    } else {
        pb_ctl_close(c);
        return usage();
    }
    pb_ctl_close(c);
    return 0;
}
