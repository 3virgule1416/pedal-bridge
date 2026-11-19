/*
 * MIDI du pont (ALSA séquenceur) : écoute le MIDI Captain et le double switch
 * de l'Axe I/O One EN PARALLÈLE de PiPedal (ALSA laisse plusieurs programmes
 * s'abonner au même appareil), et peut envoyer au Captain des CC (changement
 * de page : CC 20 sur le canal 16) et du SysEx (couleurs des DEL).
 *
 * Hors temps réel seulement (fil de contrôle). Appareils retrouvés par leur
 * nom ; débranchés puis rebranchés, ils sont repris (essai toutes les 2 s).
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class MidiSrc { Captain, Axe };

struct MidiEv {
    MidiSrc src;
    int ch;          // canal 1..16
    int cc;          // numéro de CC
    int value;       // 0..127
};

/* Ouvre le client ALSA « pedal_bridge ». false (message dans err) si ALSA
 * n'est pas disponible ; les appareils absents sont attendus. */
bool midi_open(std::string &err);
void midi_close();

/* Descripteur à surveiller (select) ; -1 si fermé. */
int midi_fd();

/* Lit les événements en attente (CC seulement) et reprend les appareils
 * rebranchés. Appeler souvent (boucle du fil de contrôle). */
void midi_poll(const std::function<void(const MidiEv &)> &on_cc);

/* Vers le Captain (rien si absent). */
void midi_captain_cc(int ch, int cc, int value);
void midi_captain_sysex(const std::vector<uint8_t> &data);   // F0 … F7 compris

/* État des appareils, pour le journal : « Captain oui, Axe oui ». */
std::string midi_status();
