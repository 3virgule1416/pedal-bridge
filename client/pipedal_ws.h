/*
 * Petit client websocket de PiPedal : change la valeur d'un réglage de plugin
 * comme le fait sa page web (la page et le plugin suivent ; PiPedal marque le
 * preset modifié, comme pour un changement à la main).
 *
 * Hors temps réel seulement (fil de contrôle) : connexion TCP, bloquant, avec
 * délai d'attente court. Protocole lu dans les sources de PiPedal 2.0 :
 *   ws://127.0.0.1:80/pipedal
 *   [{"replyTo":1,"message":"currentPedalboard"}]  -> instanceId de chaque plugin
 *   [{"message":"setControl"},{"clientId":0,"instanceId":N,"symbol":"S","value":V}]
 */
#pragma once
#include <string>
#include <vector>

struct PipedalSet {
    std::string uri_prefix;   // ex. "http://pedal-bridge.local/plugins/drum-" (mono et stéréo)
    std::string symbol;       // symbole du port, ex. "groove"
    float value;
    std::string property = {}; // sinon : propriété fichier (patch), ex. ".../drum#song"…
    std::string path = {};     // … et son chemin
};

/* Applique chaque réglage à toutes les instances dont l'URI commence par
 * uri_prefix dans le pedalboard courant. Retourne le nombre de réglages
 * envoyés, ou -1 (message dans err). */
int pipedal_set_controls(int port, const std::vector<PipedalSet> &sets, std::string &err);

/* Connexion qui reste ouverte (pb_leds) : ouvre le websocket (descripteur,
 * ou -1 et err), envoie un message texte, lit un message entier (opcode :
 * 1 texte, 8 fermeture ; -1 connexion perdue). Fermer avec close(fd). */
int pipedal_ws_open(int port, std::string &err);
bool pipedal_ws_send(int fd, const std::string &text);
int pipedal_ws_recv(int fd, std::string &out);
