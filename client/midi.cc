#include "midi.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <chrono>
#include <cstring>

namespace {

snd_seq_t *g_seq = nullptr;
int g_in = -1, g_out = -1;            // nos ports : entrée (écoute), sortie (vers le Captain)
int g_fd = -1;
struct Dev {
    const char *name;                 // début du nom du client ALSA
    int client = -1, port = -1;       // -1 : absent
};
Dev g_captain{"Feather RP2040"}, g_axe{"AXE IO One"};
Dev g_pipedal{"PiPedal"};             // port d'entrée MIDI de PiPedal (« PiPedal:in »)
bool g_take_axe = false;              // le pont garde le double switch pour lui
double g_next_scan = 0;

double now_s()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* Cherche un client dont le nom commence par dev.name ; s'y abonne. */
void attach(Dev &dev, bool to_device)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    int found_c = -1, found_p = -1;
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(g_seq, ci) >= 0) {
        if (std::strncmp(snd_seq_client_info_get_name(ci), dev.name, std::strlen(dev.name)) != 0) continue;
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(g_seq, pi) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pi);
            const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((caps & need) == need) { found_c = c; found_p = snd_seq_port_info_get_port(pi); break; }
        }
        if (found_c >= 0) break;
    }
    if (found_c == dev.client && found_p == dev.port) return;          // rien de neuf
    dev.client = found_c;
    dev.port = found_p;
    if (found_c < 0) return;
    snd_seq_connect_from(g_seq, g_in, found_c, found_p);              // l'appareil -> nous
    if (to_device) snd_seq_connect_to(g_seq, g_out, found_c, found_p); // nous -> l'appareil
}

/* Port d'entrée de PiPedal : client « PiPedal », port en écriture. */
void find_pipedal()
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    g_pipedal.client = g_pipedal.port = -1;
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(g_seq, ci) >= 0) {
        if (std::strcmp(snd_seq_client_info_get_name(ci), g_pipedal.name) != 0) continue;
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(g_seq, pi) >= 0) {
            const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
            if ((snd_seq_port_info_get_capability(pi) & need) == need) {
                g_pipedal.client = c;
                g_pipedal.port = snd_seq_port_info_get_port(pi);
                return;
            }
        }
    }
}

/* Connexion Axe -> PiPedal (celle que PiPedal fait lui-même) : présente ? */
bool axe_linked()
{
    if (g_axe.client < 0 || g_pipedal.client < 0) return false;
    snd_seq_port_subscribe_t *sub;
    snd_seq_port_subscribe_alloca(&sub);
    snd_seq_addr_t s{(unsigned char)g_axe.client, (unsigned char)g_axe.port};
    snd_seq_addr_t d{(unsigned char)g_pipedal.client, (unsigned char)g_pipedal.port};
    snd_seq_port_subscribe_set_sender(sub, &s);
    snd_seq_port_subscribe_set_dest(sub, &d);
    return snd_seq_get_port_subscription(g_seq, sub) == 0;
}

void axe_link(bool on)
{
    if (g_axe.client < 0 || g_pipedal.client < 0 || axe_linked() == on) return;
    snd_seq_port_subscribe_t *sub;
    snd_seq_port_subscribe_alloca(&sub);
    snd_seq_addr_t s{(unsigned char)g_axe.client, (unsigned char)g_axe.port};
    snd_seq_addr_t d{(unsigned char)g_pipedal.client, (unsigned char)g_pipedal.port};
    snd_seq_port_subscribe_set_sender(sub, &s);
    snd_seq_port_subscribe_set_dest(sub, &d);
    if (on) snd_seq_subscribe_port(g_seq, sub);
    else snd_seq_unsubscribe_port(g_seq, sub);
}

void scan()
{
    attach(g_captain, true);
    attach(g_axe, false);
    find_pipedal();
    if (g_take_axe) axe_link(false);                   // PiPedal l'a peut-être refaite (redémarrage, rebranchement)
}

} // namespace

bool midi_open(std::string &err)
{
    if (g_seq) return true;
    if (snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        g_seq = nullptr;
        err = "ALSA séquenceur indisponible";
        return false;
    }
    snd_seq_set_client_name(g_seq, "pedal_bridge");
    g_in = snd_seq_create_simple_port(g_seq, "pedales", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    g_out = snd_seq_create_simple_port(g_seq, "captain", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (g_in < 0 || g_out < 0) { err = "ports ALSA"; midi_close(); return false; }
    pollfd pf{};
    if (snd_seq_poll_descriptors(g_seq, &pf, 1, POLLIN) == 1) g_fd = pf.fd;
    scan();
    g_next_scan = now_s() + 2.0;
    return true;
}

void midi_close()
{
    if (g_seq && g_captain.client >= 0) {              // le Captain revient sur la page 0 (PiPedal)
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_controller(&ev, 15, 20, 0);   // canal 16, CC 20 = page 0
        snd_seq_ev_set_source(&ev, g_out);
        snd_seq_ev_set_dest(&ev, g_captain.client, g_captain.port);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(g_seq, &ev);
    }
    if (g_seq && g_take_axe) { find_pipedal(); axe_link(true); }   // A et B retournent à PiPedal
    g_take_axe = false;
    if (g_seq) snd_seq_close(g_seq);
    g_seq = nullptr;
    g_in = g_out = g_fd = -1;
    g_captain.client = g_axe.client = -1;
}

int midi_fd() { return g_fd; }

void midi_poll(const std::function<void(const MidiEv &)> &on_cc)
{
    if (!g_seq) return;
    if (now_s() >= g_next_scan) { scan(); g_next_scan = now_s() + 2.0; }
    snd_seq_event_t *ev = nullptr;
    while (snd_seq_event_input(g_seq, &ev) >= 0 && ev) {
        if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
            const int c = ev->source.client;
            if (c == g_captain.client || c == g_axe.client)
                on_cc({c == g_captain.client ? MidiSrc::Captain : MidiSrc::Axe, ev->data.control.channel + 1,
                       (int)ev->data.control.param, ev->data.control.value});
        } else if (ev->type == SND_SEQ_EVENT_CLIENT_EXIT || ev->type == SND_SEQ_EVENT_PORT_EXIT) {
            g_next_scan = 0;                                          // un appareil est parti : rescanner
        }
        ev = nullptr;
    }
}

static void send(snd_seq_event_t &ev)
{
    if (!g_seq || g_captain.client < 0) return;
    snd_seq_ev_set_source(&ev, g_out);
    snd_seq_ev_set_dest(&ev, g_captain.client, g_captain.port);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(g_seq, &ev);
}

void midi_take_axe(bool take)
{
    g_take_axe = take;
    if (!g_seq) return;
    find_pipedal();
    axe_link(!take);
}

void midi_pipedal_cc(int ch, int cc, int value)
{
    if (!g_seq || g_pipedal.client < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, ch - 1, cc, value);
    snd_seq_ev_set_source(&ev, g_out);
    snd_seq_ev_set_dest(&ev, g_pipedal.client, g_pipedal.port);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(g_seq, &ev);
}

void midi_captain_cc(int ch, int cc, int value)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, ch - 1, cc, value);
    send(ev);
}

void midi_captain_sysex(const std::vector<uint8_t> &data)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_sysex(&ev, data.size(), const_cast<uint8_t *>(data.data()));
    send(ev);
}

std::string midi_status()
{
    if (!g_seq) return "MIDI fermé";
    return std::string("Captain ") + (g_captain.client >= 0 ? "oui" : "non") + ", Axe " +
           (g_axe.client >= 0 ? "oui" : "non") + ", PiPedal " + (g_pipedal.client >= 0 ? "oui" : "non") +
           (g_take_axe ? " ; double switch au pont" : "");
}
