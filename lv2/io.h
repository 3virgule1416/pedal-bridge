/*
 * Audio du pont pour les plugins DLS Looper / DLS Drum : envoi et retour sur
 * un lien fixe, en mono ou en stéréo. Même comportement que DLS Send et
 * DLS Return (mode Add) : voir send.c et return.c.
 *
 * Ouverture hors temps réel (instantiate) ; *_run() : seulement atomiques,
 * calcul et copies (temps réel).
 */
#ifndef PB_IO_H
#define PB_IO_H

#include "pb_shm.h"

typedef struct {
    pb_link  link;
    uint64_t me;
    uint32_t rate;
    int      need_take;
    pb_watch peer;          /* le lecteur (client JACK) */
} pb_send_io;

typedef struct {
    pb_link  link;
    uint64_t me;
    uint32_t rate;
    int      need_take;
    int      state;         /* attente / lecture */
    pb_watch peer;          /* l'écrivain (client JACK) */
    float    gain_db, gain_cur, gain_target;
    float    env;
    float    last[2];
} pb_return_io;

/* Hors temps réel. Retourne 0 si le lien est ouvert (sinon : pas d'audio du
 * pont, le plugin laisse passer le signal). */
int  pb_send_io_open(pb_send_io *s, uint32_t link_no, uint32_t rate);
void pb_send_io_activate(pb_send_io *s);
void pb_send_io_deactivate(pb_send_io *s);
void pb_send_io_close(pb_send_io *s);

int  pb_return_io_open(pb_return_io *r, uint32_t link_no, uint32_t rate);
void pb_return_io_activate(pb_return_io *r);
void pb_return_io_deactivate(pb_return_io *r);
void pb_return_io_close(pb_return_io *r);

/* Temps réel. Copie l'entrée dans le tampon d'envoi si un lecteur est là.
 * Mono : in_r == in_l (le même son des deux côtés). Retourne 1 si connecté. */
int pb_send_io_run(pb_send_io *s, const float *in_l, const float *in_r, uint32_t n);

/* Temps réel. AJOUTE le retour aux sorties (qui contiennent déjà le signal).
 * nch = 1 : out[0] reçoit (G + D) / 2. Retourne 1 si connecté. */
int pb_return_io_run(pb_return_io *r, float *const out[2], int nch, uint32_t n, float gain_db);

#endif
