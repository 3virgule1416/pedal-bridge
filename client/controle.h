/*
 * Module de contrôle de pedal_bridge : play/stop globaux (batterie + boucles),
 * normalisation des boucles, sauvegarde et rechargement de chansons.
 *
 * Tourne dans son propre thread (jamais dans le callback audio). Reçoit des
 * commandes texte par UDP sur 127.0.0.1:<port> et répond en texte :
 *
 *   play            tout repart de la mesure 1, batterie et boucles ensemble
 *   stop            tout s'arrête ; les nouvelles boucles sont normalisées
 *   save NOM        sauvegarde la chanson (boucles normalisées + tempo)
 *   load NOM        recharge une chanson (transport arrêté à la mesure 1)
 *   list            liste les chansons sauvegardées
 *   status          état des boucles
 *   groove N        pattern N d'Hydrogen (à partir de 1), retenu pour save
 *   tempo BPM       tempo (pb_timebase) + affichage d'Hydrogen
 *
 * Normaliser une boucle : SooperLooper compense la latence en décalant la
 * lecture, et oublie ce décalage quand la boucle repart par « trigger »
 * (mesuré : 22,7 ms de retard). On décale donc l'audio lui-même de
 * D = latence d'entrée + de sortie (save_loop, rotation, load_loop). Ensuite,
 * trigger tombe juste, y compris pour une boucle rechargée plus tard.
 * Seul un enregistrement neuf (record) a besoin d'être normalisé : un overdub
 * sur une boucle normalisée se place correctement (mesuré avec pb_lag).
 */
#ifndef PB_CONTROLE_H
#define PB_CONTROLE_H

#include <jack/jack.h>

#include <cstdint>
#include <functional>
#include <string>

struct CtlConfig {
    jack_client_t *client = nullptr;
    int sl_port = 9951;                       // OSC de SooperLooper
    int cmd_port = 9982;                      // commandes texte (UDP, 127.0.0.1)
    int timebase_port = 9980;                 // pb_timebase (tempo)
    int h2_port = 9000;                       // OSC d'Hydrogen
    std::string base_dir;                     // ~/pedal-bridge
    std::function<uint32_t()> comp_frames;    // D en trames JACK (0 = inconnu)
};

/* Démarre le thread de contrôle. Retourne false si le port est déjà pris. */
bool ctl_start(const CtlConfig &cfg);
void ctl_stop();

/* Mode client : envoie une commande à un pedal_bridge déjà lancé et affiche
 * la réponse. Retourne 0 si la réponse commence par « ok ». */
int ctl_send_command(int cmd_port, const std::string &cmd, double timeout_s);

#endif
