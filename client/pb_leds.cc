/*
 * pb_leds : DEL de la page PEDAL du MIDI Captain (mode PiPedal).
 *
 * Programme à part, lancé au démarrage du Pi (service système) : il suit
 * PiPedal par son websocket et colore les touches de la page PEDAL, même si
 * le pont (pedal_bridge) ne tourne pas. Reprend les règles de
 * captain-manager/pipedal_leds.py, avec en plus :
 *   1. chaque effet dans sa couleur d'icône de PiPedal (sinon vert) ;
 *   2. un CC lié à un interrupteur d'un effet (pas seulement le bypass) : la
 *      DEL montre l'état de cet interrupteur ;
 *   4. un bref éclair de toutes les DEL quand un preset est chargé ;
 *   5. reconnexion à PiPedal et repeinture si PiPedal redémarre ou si le
 *      Captain est rebranché.
 *
 * Touche N = CC N (1 à 10) ; snapshots 1 à 5 sur les touches 6 à 10 (ils
 * passent devant). Allumé : les 3 DEL, couleur vive ; éteint : une DEL, 5 %.
 *
 * Un seul responsable des DEL : pb_leds peint seulement si le Captain est sur
 * la page PEDAL (page publiée par le pont dans /dev/shm/pedal-bridge-control)
 * ou si le pont ne donne plus signe de vie ; sinon c'est le pont qui peint.
 */
#include "pb_ctl.h"
#include "pb_shm.h"
#include "pipedal_ws.h"

#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_sig(int) { g_stop = 1; }

double now_s()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

/* ------------------------------------------------------------- JSON --- */
struct J {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J *get(const char *k) const
    {
        if (t != OBJ) return nullptr;
        for (const auto &kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double num(const char *k, double d = 0) const
    {
        const J *v = get(k);
        return v && v->t == NUM ? v->n : v && v->t == BOOL ? (v->b ? 1 : 0) : d;
    }
    std::string str(const char *k) const
    {
        const J *v = get(k);
        return v && v->t == STR ? v->s : std::string();
    }
};

struct Parser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool str(std::string &out)
    {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'u': out += '?'; p += 4; break;   // assez pour nos besoins
                default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(J &v)
    {
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = J::OBJ;
            p++;
            ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                p++;
                J x;
                if (!val(x)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.t = J::ARR;
            p++;
            ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                J x;
                if (!val(x)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = J::STR; return str(v.s); }
        if (!std::strncmp(p, "true", 4)) { v.t = J::BOOL; v.b = true; p += 4; return true; }
        if (!std::strncmp(p, "false", 5)) { v.t = J::BOOL; p += 5; return true; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return true; }
        char *end;
        v.n = std::strtod(p, &end);
        if (end == p) return false;
        v.t = J::NUM;
        p = end;
        return true;
    }
};

bool parse(const std::string &text, J &out)
{
    Parser ps{text.data(), text.data() + text.size()};
    return ps.val(out);
}

/* ----------------------------------------------------------- Couleurs --- */
using RGB = std::array<int, 3>;
const std::map<std::string, RGB> COLORS = {      // palette « Material 500 » de PiPedal
    {"default", {255, 255, 255}}, {"red", {244, 67, 54}}, {"pink", {233, 30, 99}},
    {"purple", {156, 39, 176}}, {"deep-purple", {103, 58, 183}}, {"indigo", {63, 81, 181}},
    {"blue", {33, 150, 243}}, {"light-blue", {3, 169, 244}}, {"cyan", {0, 188, 212}},
    {"teal", {0, 150, 136}}, {"green", {76, 175, 80}}, {"light-green", {139, 195, 74}},
    {"lime", {205, 220, 57}}, {"yellow", {255, 235, 59}}, {"amber", {255, 193, 7}},
    {"orange", {255, 152, 0}}, {"deep-orange", {255, 87, 34}}, {"brown", {121, 85, 72}},
    {"grey", {158, 158, 158}}, {"blue-grey", {96, 125, 139}},
};
const RGB EFFECT_DEFAULT = {0, 255, 0};          // effet sans couleur choisie
const double DIM = 0.05;                         // éteint : 5 % de la couleur, une DEL

RGB color_of(const std::string &key, RGB fallback)
{
    const auto it = COLORS.find(key);
    return it == COLORS.end() ? fallback : it->second;
}

/* ------------------------------------------------------- MIDI (Captain) --- */
snd_seq_t *g_seq = nullptr;
int g_out = -1, g_in = -1;
int g_cap_c = -1, g_cap_p = -1;

bool find_captain()
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(g_seq, ci) >= 0) {
        if (std::strncmp(snd_seq_client_info_get_name(ci), "Feather RP2040", 14) != 0) continue;
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(g_seq, pi) >= 0) {
            const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_READ;
            if ((snd_seq_port_info_get_capability(pi) & need) == need) {
                const int p = snd_seq_port_info_get_port(pi);
                const bool fresh = c != g_cap_c || p != g_cap_p;
                g_cap_c = c;
                g_cap_p = p;
                if (fresh) snd_seq_connect_from(g_seq, g_in, c, p);   // ses boutons : repeindre après un appui
                return fresh;
            }
        }
    }
    g_cap_c = g_cap_p = -1;
    return false;
}

/* Une touche (1 à 10) : couleur sur les 3 DEL (full) ou sur une seule. */
void set_key(int key, RGB c, bool full)
{
    if (g_cap_c < 0) return;
    std::vector<uint8_t> m = {0xF0, 0x00, 0x04, 0x10, (uint8_t)key};
    for (int led = 0; led < 3; led++)
        for (int k = 0; k < 3; k++) m.push_back((uint8_t)((full || led == 0 ? c[k] : 0) / 2));
    m.push_back(0xF7);
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_sysex(&ev, m.size(), m.data());
    snd_seq_ev_set_source(&ev, g_out);
    snd_seq_ev_set_dest(&ev, g_cap_c, g_cap_p);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(g_seq, &ev);
}

/* ----------------------------------------------------- État de PiPedal --- */
struct Lit {
    int key;
    long long id;                                  // instanceId de l'effet
    std::string symbol;                            // "__bypass" ou interrupteur
    RGB color;
};
std::vector<Lit> g_lit;                            // touches liées à un effet
std::map<long long, bool> g_enabled;               // effet actif
std::map<std::pair<long long, std::string>, double> g_values;   // valeurs des réglages liés
std::vector<std::unique_ptr<RGB>> g_snaps;         // couleurs des snapshots (nullptr : vide)
int g_selected = -1;
long long g_preset = -1;                           // preset choisi (pour l'éclair)
bool g_dirty = true;

void load_pedalboard(const J &pb)
{
    g_lit.clear();
    g_enabled.clear();
    g_values.clear();
    if (const J *items = pb.get("items"))
        for (const J &it : items->a) {
            const long long id = (long long)it.num("instanceId", -1);
            g_enabled[id] = it.num("isEnabled", 1) != 0;
            const RGB col = color_of(it.str("iconColor"), EFFECT_DEFAULT);
            if (const J *cv = it.get("controlValues"))
                for (const J &v : cv->a) g_values[{id, v.str("key")}] = v.num("value");
            if (const J *mb = it.get("midiBindings"))
                for (const J &b : mb->a) {
                    if ((int)b.num("bindingType") != 2) continue;        // CC seulement
                    const int cc = (int)b.num("control", -1);
                    if (cc < 1 || cc > 10) continue;
                    g_lit.push_back({cc, id, b.str("symbol"), col});
                }
        }
    g_snaps.clear();
    if (const J *sn = pb.get("snapshots"))
        for (const J &s : sn->a)
            g_snaps.push_back(s.t == J::OBJ ? std::make_unique<RGB>(color_of(s.str("color"), {255, 255, 255})) : nullptr);
    g_selected = (int)pb.num("selectedSnapshot", -1);
    g_dirty = true;
    say("pb_leds : preset « %s », %zu touche(s) liée(s), %zu snapshot(s)", pb.str("name").c_str(), g_lit.size(),
        g_snaps.size());
}

void paint()
{
    std::map<int, std::pair<RGB, bool>> keys;      // touche -> (couleur, allumée)
    for (const Lit &l : g_lit) {
        bool on;
        if (l.symbol == "__bypass") on = g_enabled[l.id];
        else {
            const auto it = g_values.find({l.id, l.symbol});
            on = it != g_values.end() && it->second > 0.5;
        }
        keys[l.key] = {l.color, on};
    }
    for (size_t i = 0; i < g_snaps.size() && i < 5; i++)
        if (g_snaps[i]) keys[6 + (int)i] = {*g_snaps[i], (int)i == g_selected};
    const bool window = access("/dev/shm/pb-demarrage", F_OK) == 0;   // pb_demarrage : rangée du bas à lui
    for (int k = 1; k <= 10; k++) {
        if (k >= 6 && window) continue;
        const auto it = keys.find(k);
        if (it == keys.end()) { set_key(k, {0, 0, 0}, true); continue; }
        const RGB c = it->second.first;
        if (it->second.second) set_key(k, c, true);
        else set_key(k, {(int)(c[0] * DIM), (int)(c[1] * DIM), (int)(c[2] * DIM)}, false);
    }
}

void flash()                                       // éclair au chargement d'un preset
{
    for (int k = 1; k <= 10; k++) set_key(k, {255, 255, 255}, true);
    usleep(120000);
}

/* Messages de PiPedal : [ {"message": nom, "reply": n?}, corps ]. */
bool handle(const std::string &raw, bool may_paint)
{
    J d;
    if (!parse(raw, d) || d.t != J::ARR || d.a.empty() || d.a[0].t != J::OBJ) return false;
    const std::string name = d.a[0].str("message");
    const J *body = d.a.size() > 1 ? &d.a[1] : nullptr;
    if (!body || name == "onVuUpdate") return false;
    if (name == "currentPedalboard") load_pedalboard(*body);
    else if (name == "onPedalboardChanged") { if (const J *pb = body->get("pedalboard")) load_pedalboard(*pb); }
    else if (name == "onItemEnabledChanged") {
        g_enabled[(long long)body->num("instanceId", -1)] = body->num("enabled") != 0;
        g_dirty = true;
    } else if (name == "onControlChanged") {
        const std::pair<long long, std::string> k{(long long)body->num("instanceId", -1), body->str("symbol")};
        const double v = body->num("value");
        for (const Lit &l : g_lit)
            if (l.id == k.first && l.symbol == k.second && g_values[k] != v) { g_values[k] = v; g_dirty = true; }
    } else if (name == "onSelectedSnapshotChanged") {
        if (body->t == J::NUM) { g_selected = (int)body->n; g_dirty = true; }
    } else if (name == "getPresets" || name == "onPresetsChanged") {
        const J *pr = name == "getPresets" ? body : body->get("presets");
        if (pr) {
            const long long sel = (long long)pr->num("selectedInstanceId", -1);
            if (g_preset >= 0 && sel != g_preset && may_paint) flash();
            g_preset = sel;
        }
    }
    return true;
}

/* --------------------------------------- Qui peint (pont ou pb_leds) --- */
pb_ctl *g_ctl = nullptr;
uint32_t g_hb = 0;
double g_hb_t = 0;
double g_ctl_try = 0;

/* true : la page PEDAL est affichée, ou le pont ne donne plus signe de vie. */
bool may_paint()
{
    if (!g_ctl && now_s() >= g_ctl_try) {
        g_ctl_try = now_s() + 2.0;
        char err[160];
        pb_ctl_open(&g_ctl, 0, err, sizeof err);
    }
    if (!g_ctl) return true;
    const uint32_t hb = PB_LOAD_RLX(&g_ctl->client_heartbeat);
    if (hb != g_hb) { g_hb = hb; g_hb_t = now_s(); }
    if (now_s() - g_hb_t > 1.5) return true;       // pont absent : le Captain est sur PEDAL (pb_garde)
    const uint32_t page = PB_LOAD_RLX(&g_ctl->cap_page);
    return page == 0 || page == 1;                 // inconnue, ou page 0
}

} // namespace

int main()
{
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    if (snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        std::fprintf(stderr, "pb_leds : ALSA indisponible\n");
        return 1;
    }
    snd_seq_set_client_name(g_seq, "pb_leds");
    g_out = snd_seq_create_simple_port(g_seq, "del", SND_SEQ_PORT_CAP_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    g_in = snd_seq_create_simple_port(g_seq, "boutons", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                      SND_SEQ_PORT_TYPE_APPLICATION);
    const char *pe = std::getenv("PB_PIPEDAL_PORT");
    const int port = pe ? std::atoi(pe) : 80;
    say("pb_leds : DEL de la page PEDAL du Captain (PiPedal, port %d)", port);

    int fd = -1;
    double next_try = 0, next_scan = 0;
    bool painting = false;
    while (!g_stop) {
        if (fd < 0 && now_s() >= next_try) {
            std::string err;
            fd = pipedal_ws_open(port, err);
            if (fd < 0) next_try = now_s() + 3.0;
            else {
                say("pb_leds : connecté à PiPedal");
                pipedal_ws_send(fd, "[{\"message\":\"hello\",\"replyTo\":1}]");
                pipedal_ws_send(fd, "[{\"message\":\"currentPedalboard\",\"replyTo\":2}]");
                pipedal_ws_send(fd, "[{\"message\":\"getPresets\",\"replyTo\":3}]");
            }
        }
        if (now_s() >= next_scan) {
            next_scan = now_s() + 2.0;
            if (find_captain()) g_dirty = true;     // (re)branché : repeindre
        }
        const bool may = may_paint();
        if (may && !painting) g_dirty = true;       // retour sur la page PEDAL : repeindre
        painting = may;

        pollfd pf{fd, POLLIN, 0};
        if (fd >= 0 && poll(&pf, 1, 50) > 0) {
            std::string msg;
            const int op = pipedal_ws_recv(fd, msg);
            if (op < 0 || op == 8) {
                say("pb_leds : connexion à PiPedal perdue, nouvel essai dans 3 s");
                close(fd);
                fd = -1;
                next_try = now_s() + 3.0;
            } else if (op == 1) handle(msg, may);
        } else if (fd < 0) usleep(50000);

        /* Boutons de la page PEDAL : le Captain remet la DEL appuyée en noir. */
        snd_seq_event_t *ev = nullptr;
        while (snd_seq_event_input(g_seq, &ev) >= 0 && ev) {
            if (ev->type == SND_SEQ_EVENT_CONTROLLER && ev->data.control.channel == 0) g_dirty = true;
            ev = nullptr;
        }
        static bool window_seen = false;           // fin de la fenêtre de démarrage : repeindre la rangée du bas
        const bool window = access("/dev/shm/pb-demarrage", F_OK) == 0;
        if (window_seen && !window) g_dirty = true;
        window_seen = window;
        if (g_dirty && may) { paint(); g_dirty = false; }
    }
    if (fd >= 0) close(fd);
    snd_seq_close(g_seq);
    return 0;
}
