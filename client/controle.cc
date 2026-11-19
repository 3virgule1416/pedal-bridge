/* Module de contrôle de pedal_bridge : voir controle.h. */
#include "controle.h"
#include "pb_ctl.h"
#include "pb_shm.h"
#include "pipedal_ws.h"

#include <arpa/inet.h>
#include <jack/transport.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <cctype>
#include <cstdarg>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

namespace {

/* ------------------------------------------------------------------ OSC --- */

void put_str(std::string &m, const std::string &s)
{
    m += s;
    m.push_back('\0');
    while (m.size() % 4) m.push_back('\0');
}

void put_f(std::string &m, float f)
{
    uint32_t be;
    std::memcpy(&be, &f, 4);
    be = htonl(be);
    m.append((const char *)&be, 4);
}

struct OscArg { char type; int32_t i = 0; float f = 0; std::string s; };
struct OscMsg { std::string addr; std::vector<OscArg> args; };

bool get_str(const char *b, size_t n, size_t &p, std::string &out)
{
    size_t e = p;
    while (e < n && b[e]) e++;
    if (e >= n) return false;
    out.assign(b + p, e - p);
    p = (e + 4) & ~(size_t)3;
    return p <= n;
}

bool decode(const char *b, size_t n, OscMsg &m)
{
    size_t p = 0;
    std::string tags;
    if (!get_str(b, n, p, m.addr) || !get_str(b, n, p, tags) || tags.empty() || tags[0] != ',')
        return false;
    for (size_t k = 1; k < tags.size(); k++) {
        OscArg a;
        a.type = tags[k];
        if (a.type == 'i' || a.type == 'f') {
            if (p + 4 > n) return false;
            uint32_t be;
            std::memcpy(&be, b + p, 4);
            be = ntohl(be);
            if (a.type == 'i') std::memcpy(&a.i, &be, 4); else std::memcpy(&a.f, &be, 4);
            p += 4;
        } else if (a.type == 's') {
            if (!get_str(b, n, p, a.s)) return false;
        } else return false;
        m.args.push_back(a);
    }
    return true;
}

/* -------------------------------------------------------------- état ----- */

struct Loop {
    float len = 0;          // secondes
    int state = 0;
    bool normalized = false;
    bool dirty = false;     // enregistrement neuf pas encore normalisé
    uint32_t norm_d = 0;    // délai appliqué à la normalisation (pour information)
    uint32_t rec_d = 0;     // délai du trajet au moment de l'enregistrement
    bool keep_muted = false; // muette au dernier Stop : le reste au prochain Play
};

CtlConfig g_cfg;
std::thread g_thread;
std::atomic<bool> g_run{false};
int g_cmd = -1, g_osc = -1;
std::string g_reply_url;
std::vector<Loop> g_loops;
uint32_t g_sr = 48000;
bool g_sl_ok = false;        // SooperLooper a répondu au dernier ping

/* Fichier de contrôle partagé avec DLS Looper (pb_ctl.h). Ouvert par ce
 * thread ; le callback JACK y écrit le transport (ctl_rt_update). Jamais
 * démappé avant la fin du programme : le callback peut s'en servir. */
std::atomic<pb_ctl *> g_ctl{nullptr};
uint64_t g_ctl_me = 0;
void publish_states();

enum { SL_RECORD = 2, SL_PLAY = 4, SL_PAUSED = 14 };

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf("[contrôle] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

sockaddr_in local(int port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

void sl_send(const std::string &addr, const std::vector<std::string> &sargs,
             const std::vector<float> &fargs = {})
{
    std::string m;
    put_str(m, addr);
    std::string tags = ",";
    tags.append(sargs.size(), 's');
    tags.append(fargs.size(), 'f');
    put_str(m, tags);
    for (auto &s : sargs) put_str(m, s);
    for (float f : fargs) put_f(m, f);
    sockaddr_in a = local(g_cfg.sl_port);
    sendto(g_osc, m.data(), m.size(), 0, (sockaddr *)&a, sizeof a);
}

/* Retour d'état d'Hydrogen (OSC feedback) : il l'envoie à toute adresse qui
 * lui a parlé, sur la même prise que les réponses de SooperLooper. */
int g_h2_metro = -1;              // métronome d'Hydrogen : -1 inconnu, 0 éteint, 1 allumé
void h2_feedback(const OscMsg &m)
{
    if (m.addr == "/Hydrogen/TOGGLE_METRONOME" && !m.args.empty() && m.args[0].type == 'f')
        g_h2_metro = m.args[0].f > 0.5f ? 1 : 0;
}

/* Lit les réponses OSC en attente (au plus « wait » secondes) et appelle f. */
void sl_drain(double wait, const std::function<bool(const OscMsg &)> &f)
{
    const double end = now_s() + wait;
    for (;;) {
        double left = end - now_s();
        if (left < 0) left = 0;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(g_osc, &rs);
        timeval tv{(time_t)left, (suseconds_t)((left - (time_t)left) * 1e6)};
        if (select(g_osc + 1, &rs, nullptr, nullptr, &tv) <= 0) return;
        char buf[1024];
        ssize_t n = recv(g_osc, buf, sizeof buf, 0);
        OscMsg m;
        if (n <= 0 || !decode(buf, (size_t)n, m)) continue;
        h2_feedback(m);
        if (f(m)) return;
    }
}

/* Nombre de boucles de SooperLooper (ping). 0 si absent. */
int sl_count()
{
    sl_send("/ping", {g_reply_url, "/pong"});
    int count = 0;
    sl_drain(0.3, [&](const OscMsg &m) {
        if (m.addr == "/pong" && m.args.size() >= 3 && m.args[2].type == 'i') { count = m.args[2].i; return true; }
        return false;
    });
    return count;
}

/* --- État des normalisations, gardé dans un fichier ---------------------
 * Pour qu'un redémarrage du pont ne fasse pas décaler une 2e fois une boucle
 * déjà normalisée. Une ligne par boucle normalisée : « index longueur D ».
 * Au démarrage, une boucle est reconnue si sa longueur n'a pas changé. */
std::string work_dir();
bool mkdirs(const std::string &path);

void save_state()
{
    mkdirs(work_dir());
    std::ofstream f(work_dir() + "/etat.txt", std::ios::trunc);
    for (size_t i = 0; i < g_loops.size(); i++)
        if (g_loops[i].normalized && g_loops[i].len > 0)
            f << i << " " << g_loops[i].len << " " << g_loops[i].norm_d << "\n";
}

void restore_state()
{
    std::ifstream f(work_dir() + "/etat.txt");
    size_t i;
    float len;
    uint32_t d;
    int n = 0;
    while (f >> i >> len >> d)
        if (i < g_loops.size() && g_loops[i].len > 0 && std::fabs(g_loops[i].len - len) < 0.001f) {
            g_loops[i].normalized = true;
            g_loops[i].norm_d = d;
            n++;
        }
    if (n) say("%d boucle(s) déjà normalisée(s) reconnue(s) (etat.txt)", n);
}

/* Lit un paramètre de chaque boucle (len, state). */
void sl_refresh()
{
    const int n = sl_count();
    if (n <= 0) { g_sl_ok = false; publish_states(); return; }
    g_sl_ok = true;
    bool restore = false;
    if ((int)g_loops.size() != n) {
        g_loops.assign(n, Loop{});
        say("SooperLooper : %d boucles", n);
        restore = true;
    }
    bool changed = false;
    for (int i = 0; i < n; i++) {
        sl_send("/sl/" + std::to_string(i) + "/get", {"loop_len", g_reply_url, "/l"});
        sl_send("/sl/" + std::to_string(i) + "/get", {"state", g_reply_url, "/l"});
    }
    int seen = 0;
    sl_drain(0.3, [&](const OscMsg &m) {
        if (m.addr != "/l" || m.args.size() < 3) return false;
        const int i = m.args[0].i;
        if (i < 0 || i >= n) return false;
        Loop &L = g_loops[i];
        if (m.args[1].s == "loop_len") {
            const float len = m.args[2].f;
            if (len <= 0) { if (L.normalized) changed = true; L = Loop{}; }   // boucle vidée
            else L.len = len;
        } else if (m.args[1].s == "state") {
            L.state = (int)lrintf(m.args[2].f);
            if (L.state == SL_RECORD) {
                if (L.normalized) changed = true;
                L.dirty = true;
                L.normalized = false;
                /* Délai à corriger = celui du moment de l'enregistrement. */
                const uint32_t d = g_cfg.comp_frames ? g_cfg.comp_frames() : 0;
                if (d) L.rec_d = d;
            }
        }
        return ++seen >= 2 * n;
    });
    if (restore) restore_state();
    if (changed) save_state();
    publish_states();
}

bool sl_any_recording()
{
    for (auto &L : g_loops)
        if (L.state == SL_RECORD || L.state == 1 /* attente départ */ || L.state == 3 /* attente fin */)
            return true;
    return false;
}

/* ------------------------------------------------------------ fichiers --- */

bool mkdirs(const std::string &path)
{
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty()) { cur = "/"; continue; }
        cur += (cur.empty() || cur.back() == '/' ? "" : "/") + part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

long file_size(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

/* Attend que save_loop ait fini d'écrire (taille stable et assez grande). */
bool wait_file(const std::string &p, long min_size, double timeout)
{
    const double end = now_s() + timeout;
    long last = -2;
    double stable_since = now_s();
    while (now_s() < end) {
        const long s = file_size(p);
        if (s != last) { last = s; stable_since = now_s(); }
        else if (s >= min_size && now_s() - stable_since > 0.08) return true;
        usleep(10000);
    }
    return false;
}

/* Fait tourner l'audio d'un WAV : l'échantillon i prend la valeur de i + shift
 * (la boucle joue « shift » trames plus tôt). shift peut être négatif. */
bool rotate_wav(const std::string &src, const std::string &dst, long shift, std::string &err)
{
    std::ifstream in(src, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < 12 || raw.compare(0, 4, "RIFF") || raw.compare(8, 4, "WAVE")) { err = "pas un WAV"; return false; }
    size_t pos = 12, data_off = 0, data_len = 0;
    unsigned align = 0;
    while (pos + 8 <= raw.size()) {
        uint32_t size;
        std::memcpy(&size, raw.data() + pos + 4, 4);
        if (!raw.compare(pos, 4, "fmt ") && pos + 22 <= raw.size()) {
            uint16_t a;
            std::memcpy(&a, raw.data() + pos + 20, 2);
            align = a;
        }
        if (!raw.compare(pos, 4, "data")) { data_off = pos + 8; data_len = size; break; }
        pos += 8 + size + (size & 1);
    }
    if (!align || !data_off || data_off + data_len > raw.size()) { err = "WAV incomplet"; return false; }
    const long frames = (long)(data_len / align);
    if (frames <= 0) { err = "WAV vide"; return false; }
    long s = shift % frames;
    if (s < 0) s += frames;
    const size_t off = (size_t)s * align;
    std::string out = raw.substr(0, data_off);
    out += raw.substr(data_off + off, data_len - off);
    out += raw.substr(data_off, off);
    out += raw.substr(data_off + data_len);
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    o.write(out.data(), (std::streamsize)out.size());
    if (!o) { err = "écriture impossible"; return false; }
    return true;
}

bool save_loop_to(int i, const std::string &path, std::string &err)
{
    unlink(path.c_str());
    sl_send("/sl/" + std::to_string(i) + "/save_loop", {path, "float", "little", g_reply_url, "/err"});
    const long expected = (long)std::lround(g_loops[i].len * g_sr) * 8;   // stéréo float
    if (!wait_file(path, expected, 10.0)) { err = "save_loop n'a pas fini : " + path; return false; }
    return true;
}

bool load_loop_from(int i, const std::string &path, std::string &err)
{
    sl_send("/sl/" + std::to_string(i) + "/load_loop", {path, g_reply_url, "/err"});
    const double end = now_s() + 5.0;
    while (now_s() < end) {                    // attendre que la longueur apparaisse
        sl_send("/sl/" + std::to_string(i) + "/get", {"loop_len", g_reply_url, "/l"});
        float len = 0;
        sl_drain(0.1, [&](const OscMsg &m) {
            if (m.addr == "/l" && m.args.size() >= 3 && m.args[0].i == i && m.args[1].s == "loop_len") { len = m.args[2].f; return true; }
            return false;
        });
        if (len > 0) { g_loops[i].len = len; return true; }
    }
    err = "load_loop n'a pas chargé " + path;
    return false;
}

std::string work_dir() { return g_cfg.base_dir + "/travail"; }
std::string songs_dir() { return g_cfg.base_dir + "/chansons"; }

/* Normalise une boucle : décale son audio du délai qu'avait le trajet au moment
 * de son enregistrement (rec_d ; à défaut, D actuel).
 * Une boucle normalisée est DÉFINITIVE : au playback, la boucle et la batterie
 * passent par le même chemin de retour, donc elles restent alignées même si la
 * latence change ensuite. On ne la décale plus jamais. */
bool normalize(int i, uint32_t D, std::string &err)
{
    Loop &L = g_loops[i];
    if (L.normalized) return true;
    const long shift = L.rec_d ? (long)L.rec_d : (long)D;
    mkdirs(work_dir());
    const std::string a = work_dir() + "/boucle" + std::to_string(i + 1) + ".wav";
    const std::string b = work_dir() + "/boucle" + std::to_string(i + 1) + "-norm.wav";
    const double t0 = now_s();
    if (!save_loop_to(i, a, err) || !rotate_wav(a, b, shift, err) || !load_loop_from(i, b, err))
        return false;
    sl_send("/sl/" + std::to_string(i) + "/hit", {"pause_on"});
    unlink(a.c_str());
    unlink(b.c_str());
    L.normalized = true;
    L.dirty = false;
    L.norm_d = (uint32_t)shift;
    save_state();
    say("boucle %d normalisée (%+ld trames, %.0f ms de traitement)", i + 1, shift, 1000 * (now_s() - t0));
    return true;
}

/* -------------------------------------------------------------- tempo ---- */

std::string timebase(const std::string &msg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = local(g_cfg.timebase_port);
    timeval tv{1, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sendto(s, msg.data(), msg.size(), 0, (sockaddr *)&a, sizeof a);
    char buf[256];
    ssize_t n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
}

double get_tempo()
{
    const std::string r = timebase("?");                 // « tempo 120.00 BPM, ... »
    const size_t p = r.find("tempo ");
    return p == std::string::npos ? 0 : std::atof(r.c_str() + p + 6);
}

/* ------------------------------------------------------------ Hydrogen --- */
/* OSC d'Hydrogen (port 9000, à activer dans ~/.hydrogen/hydrogen.conf).
 * Vérifié sur la 1.2.0-beta : les patterns comptent à partir de 0 ;
 * SELECT_ONLY_NEXT_PATTERN choisit le pattern SANS démarrer (SELECT_AND_PLAY_PATTERN,
 * lui, démarre le transport : c'est le pont qui démarre, avec Play),
 * BPM change l'affichage, SONG_MODE_ACTIVATION 0 = mode Pattern,
 * SAVE_SONG_AS écrit le .h2song. Hydrogen n'annonce PAS le pattern
 * sélectionné : le client retient celui qu'il a demandé (g_pattern). */
int g_pattern = -1;               // pattern choisi par « groove », à partir de 0
std::vector<int> g_pat_sizes;     // taille de chaque pattern (ticks Hydrogen), du dernier .h2song lu

/* Gros morceau (toutes les chansons d'un kit dans un seul .h2song) : la
 * chanson choisie est une fenêtre de patterns [g_off, g_off + g_cnt). Groove,
 * Previous/Next et Playing sont comptés à partir de 1 DANS la fenêtre. Hors
 * gros morceau : g_off = 0, g_cnt = 0 (tout le morceau). */
int g_off = 0;
int g_cnt = 0;
int groove_count() { return g_cnt > 0 ? g_cnt : (int)g_pat_sizes.size(); }
int groove_rel() { return g_pattern >= 0 ? g_pattern - g_off + 1 : 0; }

/* Lit la taille des patterns d'un .h2song (ordre = numéro de pattern).
 * Hydrogen compte 48 ticks par noire : une mesure de 4/4 = 192 ticks. */
void read_pattern_sizes(const std::string &path)
{
    std::ifstream f(path);
    std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g_pat_sizes.clear();
    const size_t list = xml.find("<patternList>");
    const size_t end = xml.find("</patternList>");
    if (list == std::string::npos || end == std::string::npos) return;
    size_t p = list;
    while ((p = xml.find("<pattern>", p)) != std::string::npos && p < end) {
        const size_t s = xml.find("<size>", p), e = xml.find("</pattern>", p);
        g_pat_sizes.push_back(s != std::string::npos && s < e ? std::atoi(xml.c_str() + s + 6) : 0);
        p = e == std::string::npos ? end : e;
    }
    say("%zu pattern(s) lus dans %s", g_pat_sizes.size(), path.c_str());
}

/* Cycle de SooperLooper = longueur du groove (départ et fin d'enregistrement
 * au vrai début et à la vraie fin du groove), et nombre de temps par mesure
 * de pb_timebase (voyants, mesure 1). Mesures de 4 temps (192 ticks) ou de
 * 3 temps (144 ticks : valse). Retourne une phrase pour la réponse. */
int g_beats_cur = 0;              // temps par mesure envoyés à pb_timebase (0 = pas encore)

std::string apply_cycle(int pattern)
{
    int size = 192;                                   // inconnu : 1 mesure de 4/4
    const bool known = pattern >= 0 && pattern < (int)g_pat_sizes.size() && g_pat_sizes[pattern] > 0;
    if (known) size = g_pat_sizes[pattern];
    const int beats = size % 192 == 0 ? 4 : size % 144 == 0 ? 3 : 0;
    if (!beats)
        return "taille de pattern " + std::to_string(size) + " ticks : ni 4 ni 3 temps par mesure, cycle inchangé";
    std::string bpb;
    if (beats != g_beats_cur) {
        if (timebase("beats " + std::to_string(beats)).empty()) bpb = " ; erreur : pb_timebase ne répond pas";
        else { g_beats_cur = beats; bpb = " ; " + std::to_string(beats) + " temps par mesure"; }
    }
    const int bars = size / (48 * beats);
    sl_send("/set", {"eighth_per_cycle"}, {(float)(size / 24)});   // 24 ticks par croche
    return "cycle SooperLooper " + std::to_string(bars) + " mesure(s)" + (known ? "" : " (taille inconnue)") + bpb;
}

void h2_send(const std::string &addr, const std::string *s, const float *f)
{
    std::string m;
    put_str(m, addr);
    put_str(m, std::string(",") + (s ? "s" : "") + (f ? "f" : ""));
    if (s) put_str(m, *s);
    if (f) put_f(m, *f);
    sockaddr_in a = local(g_cfg.h2_port);
    sendto(g_osc, m.data(), m.size(), 0, (sockaddr *)&a, sizeof a);
}
void h2_float(const std::string &addr, float v) { h2_send(addr, nullptr, &v); }
void h2_string(const std::string &addr, const std::string &v) { h2_send(addr, &v, nullptr); }

/* Hydrogen écoute-t-il ? (port UDP ouvert : on ne peut pas lui « parler » sans effet) */
bool h2_listening()
{
    std::ifstream f("/proc/net/udp");
    char port_hex[8];
    std::snprintf(port_hex, sizeof port_hex, ":%04X", g_cfg.h2_port);
    std::string line;
    while (std::getline(f, line))
        if (line.find(port_hex) != std::string::npos && line.find(port_hex) < 20) return true;
    return false;
}

/* Métronome d'Hydrogen. Son OSC n'a qu'une bascule (TOGGLE_METRONOME, argument
 * ignoré) : on bascule seulement si l'état connu diffère. État inconnu : un
 * message d'une adresse nouvelle pour Hydrogen lui fait renvoyer tout son état. */
std::string set_metronome(bool on)
{
    if (!h2_listening()) return "erreur : l'OSC d'Hydrogen ne répond pas (port " + std::to_string(g_cfg.h2_port) + ")";
    if (g_h2_metro < 0) {
        h2_send("/Hydrogen/PB_PROBE", nullptr, nullptr);
        sl_drain(0.3, [](const OscMsg &) { return g_h2_metro >= 0; });
    }
    if (g_h2_metro < 0) return "erreur : état du métronome d'Hydrogen inconnu";
    if (g_h2_metro == (on ? 1 : 0)) return on ? "ok métronome déjà allumé" : "ok métronome déjà éteint";
    h2_send("/Hydrogen/TOGGLE_METRONOME", nullptr, nullptr);
    g_h2_metro = on ? 1 : 0;
    return on ? "ok métronome allumé" : "ok métronome éteint";
}

/* --- Affichage de PiPedal ------------------------------------------------
 * Next/Previous et Tap changent le groove et le tempo sans passer par les
 * réglages des plugins : le pont remet ensuite Groove et Tempo à la bonne
 * valeur par le websocket de PiPedal, comme un changement à la main. Le
 * plugin renvoie alors cette valeur : le pont la reconnaît (groove joué,
 * dernier tempo appliqué) et ne refait rien. PB_PIPEDAL_PORT=0 : désactivé. */
const char *URI_DRUM = "http://pedal-bridge.local/plugins/drum-";
const char *URI_LOOPER = "http://pedal-bridge.local/plugins/looper-";
int g_ui_groove = 0;              // à afficher (0 = rien)
int g_ui_tempo = 0;
int g_ui_part = 0, g_ui_var = -1; // listes Part (1..4) et Variation (0 None, 1.. A…) à afficher ; -1 : rien
int g_ui_intro = 0, g_ui_outro = 0; // listes Intro (1 Off, 2 Metronome, 3.. A…) et Outro (1.. A…) ; 0 : rien
std::string g_ui_song;              // chemin à montrer dans le sélecteur Song (Browse) ; vide : rien
int g_ui_tempo_late = 0;          // BPM +/- : affiché une fois le bouton relâché
double g_bpm_step_t = 0;          // dernier pas de BPM +/-
const char *g_ui_tempo_skip = nullptr;   // bouton qu'on tourne : ne pas lui renvoyer sa valeur
uint32_t g_last_tempo_c = 0;      // dernier tempo appliqué, centièmes de BPM

/* Valeurs envoyées à PiPedal dont le plugin va renvoyer l'écho (un peu plus
 * tard) : chaque écho attendu est ignoré une fois ; oubliés après 2 s. Sans
 * ça, un écho en retard ramènerait un ancien groove ou un ancien tempo. */
struct Echo { std::string what; int v; double t; };
std::vector<Echo> g_echoes;

void expect_echo(const std::string &what, int v) { g_echoes.push_back({what, v, now_s()}); }

bool take_echo(const std::string &what, int v)
{
    const double t = now_s();
    g_echoes.erase(std::remove_if(g_echoes.begin(), g_echoes.end(), [t](const Echo &e) { return t - e.t > 2.0; }),
                   g_echoes.end());
    for (auto it = g_echoes.begin(); it != g_echoes.end(); ++it)
        if (it->what == what && it->v == v) { g_echoes.erase(it); return true; }
    return false;
}

int pipedal_port()
{
    const char *e = std::getenv("PB_PIPEDAL_PORT");
    return e ? std::atoi(e) : 80;
}

void ui_flush()
{
    /* Pendant qu'on tient BPM +/-, ne pas toucher aux boutons Tempo : leur
     * retour arriverait en retard et ramènerait un ancien tempo. */
    if (g_ui_tempo_late && now_s() - g_bpm_step_t > 0.4) { g_ui_tempo = g_ui_tempo_late; g_ui_tempo_late = 0; }
    if (!g_ui_groove && !g_ui_tempo && !g_ui_part && g_ui_var < 0 && !g_ui_intro && !g_ui_outro && g_ui_song.empty()) return;
    std::vector<PipedalSet> v;
    if (g_ui_intro) v.push_back({URI_DRUM, "intro", (float)(g_ui_intro - 1)});
    if (g_ui_outro) v.push_back({URI_DRUM, "outro", (float)g_ui_outro});
    if (!g_ui_song.empty()) v.push_back({URI_DRUM, "", 0.0f, "http://pedal-bridge.local/plugins/drum#song", g_ui_song});
    g_ui_intro = g_ui_outro = 0;
    g_ui_song.clear();
    if (g_ui_part) { v.push_back({URI_DRUM, "part", (float)(g_ui_part - 1)}); expect_echo("part", g_ui_part); }
    if (g_ui_var >= 0) { v.push_back({URI_DRUM, "variation", (float)g_ui_var}); expect_echo("variation", g_ui_var); }
    g_ui_part = 0;
    g_ui_var = -1;
    if (g_ui_groove) { v.push_back({URI_DRUM, "groove", (float)g_ui_groove}); expect_echo("groove", g_ui_groove); }
    if (g_ui_tempo) {
        if (g_ui_tempo_skip != URI_DRUM) { v.push_back({URI_DRUM, "tempo", (float)g_ui_tempo}); expect_echo("drum-tempo", g_ui_tempo * 100); }
        if (g_ui_tempo_skip != URI_LOOPER) { v.push_back({URI_LOOPER, "tempo", (float)g_ui_tempo}); expect_echo("looper-tempo", g_ui_tempo * 100); }
    }
    g_ui_groove = g_ui_tempo = 0;
    g_ui_tempo_skip = nullptr;
    if (v.empty()) return;
    const int port = pipedal_port();
    if (port <= 0) return;
    std::string err;
    if (pipedal_set_controls(port, v, err) < 0) say("PiPedal : affichage non mis à jour (%s)", err.c_str());
}

std::string set_groove(const std::string &arg)
{
    const int n = std::atoi(arg.c_str());
    const int count = groove_count();
    if (n < 1 || n > 999 || (count > 0 && n > count))
        return "erreur : groove N, de 1 à " + std::to_string(count > 0 ? count : 999);
    if (!h2_listening()) return "erreur : l'OSC d'Hydrogen ne répond pas (port " + std::to_string(g_cfg.h2_port) + ")";
    const int abs = g_off + n - 1;                     // pattern dans le morceau d'Hydrogen
    h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
    h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", (float)abs);
    g_pattern = abs;
    if (n <= (int)PB_CTL_GROOVES) g_ui_groove = n;     // menu Groove : 1 à 160
    return "ok groove " + std::to_string(n) + " ; " + apply_cycle(g_pattern);
}

std::string set_tempo(const std::string &arg)
{
    const double bpm = std::atof(arg.c_str());
    if (bpm < 20 || bpm > 400) return "erreur : tempo entre 20 et 400";
    if (timebase(arg).empty()) return "erreur : pb_timebase ne répond pas";
    if (h2_listening()) h2_float("/Hydrogen/BPM", (float)bpm);    // affichage d'Hydrogen
    g_last_tempo_c = (uint32_t)std::lround(bpm * 100);
    if (bpm >= 40 && bpm <= 240 && std::fabs(bpm - std::round(bpm)) < 0.005)
        g_ui_tempo = (int)std::lround(bpm);               // bouton Tempo : BPM entiers
    return "ok tempo " + arg;
}

/* ----------------------------------------------------------- commandes --- */

void transport_stop_and_pause()
{
    jack_transport_stop(g_cfg.client);
    for (size_t i = 0; i < g_loops.size(); i++)
        if (g_loops[i].len > 0) sl_send("/sl/" + std::to_string(i) + "/hit", {"pause_on"});
}

/* Séquenceur de batterie : données (le code est plus bas, avant drum_poll). */
int g_intro_choice = 1, g_outro_choice = 1;   // listes Intro (1 Off, 2 Metronome, 3.. A…) et Outro (1.. A…)
double g_song_set_t = 0;                      // heure du dernier changement de chanson (séquenceur)
struct SongDef {
    std::vector<std::string> parts;                               // présentes, dans l'ordre
    std::map<std::string, std::vector<std::vector<int>>> vars;    // partie -> phrases (abs)
    std::map<std::string, std::vector<int>> fills;                // partie, "*", "Intro" -> abs
    std::vector<std::vector<int>> intro, outro;
    std::vector<int> transition;
    bool pool = false;                                            // pack de grooves : parties « virtuelles »
} g_song;

struct Seq {
    bool active = false;
    int part = 0;                         // index dans g_song.parts (partie affichée)
    std::map<int, int> var;               // partie -> variation choisie ; -1 : None (partie sautée)
    int loop_part = 0, next_part = 0;     // partie qui tourne ; celle qui vient
    std::vector<int> loop;                // phrase qui tourne
    size_t loop_pos = 0;
    std::deque<int> once;                 // à jouer une fois, dans l'ordre
    int once_kind = 0;                    // 1 intro, 7 outro, 8 fill/transition
    bool in_once = false;
    bool stop_after = false, metro_after = false;
    bool outro_only = false;              // Play demandé par End à l'arrêt : l'outro seule
    bool intro_only = false;              // Play demandé par Intro à l'arrêt : l'intro seule
    size_t resume_pos = 0;                // après un fill : où reprendre la phrase
    double item_start = 0, item_len = 0;  // élément en cours (temps du transport)
    int cur_abs = -1;
    double switch_at = -1;                // bascule demandée (temps absolu), -1 : aucune
    std::vector<int> next_loop;
} g_seq;
std::map<std::string, size_t> g_fill_turn;
void seq_setup(uint32_t part_ui, uint32_t var_ui);
void seq_start();
void seq_stopped();
void seq_display(uint32_t &part, uint32_t &var);
void browse_display(uint32_t &style, uint32_t &type);
void browse_check(const std::string &songs);

/* Arrête tout et normalise ce qui doit l'être. */
std::string stop_all()
{
    sl_refresh();
    if (!g_sl_ok) {                        // batterie seule : arrêter le transport suffit
        jack_transport_stop(g_cfg.client);
        seq_stopped();
        return "ok stop (SooperLooper absent : batterie seule)";
    }
    if (sl_any_recording()) return "erreur : un enregistrement est en cours, termine-le d'abord";
    /* Retenir les boucles muettes : la pause efface l'état muet, et Play
     * (trigger) remettrait le son. Boucle en pause : on garde ce qu'on savait. */
    for (Loop &L : g_loops) {
        if (L.state == 10 || L.state == 20) L.keep_muted = true;          // muette
        else if (L.state == 4 || L.state == 5 || L.state == 12) L.keep_muted = false;
    }
    transport_stop_and_pause();
    seq_stopped();
    usleep(30000);
    const uint32_t D = g_cfg.comp_frames ? g_cfg.comp_frames() : 0;
    int done = 0;
    std::string errs;
    for (size_t i = 0; i < g_loops.size(); i++) {
        Loop &L = g_loops[i];
        if (L.len <= 0) continue;
        if (!D) { errs += " latence inconnue (plugins absents ?)"; break; }
        if (!L.normalized) {
            std::string err;
            if (normalize((int)i, D, err)) done++;
            else errs += " boucle " + std::to_string(i + 1) + " : " + err;
        }
    }
    return errs.empty() ? "ok stop (" + std::to_string(done) + " boucle(s) normalisée(s))"
                        : "erreur :" + errs;
}

/* drums_only : batterie seule (End à l'arrêt, pour écouter l'outro) : les
 * boucles restent en pause. */
std::string play_from_start(bool drums_only = false)
{
    std::string r = stop_all();
    if (r.rfind("ok", 0) != 0) return r;
    jack_transport_locate(g_cfg.client, 0);
    usleep(30000);
    int n = 0;
    for (size_t i = 0; i < g_loops.size() && !drums_only; i++)
        if (g_loops[i].len > 0) {
            sl_send("/sl/" + std::to_string(i) + "/hit", {"trigger"});
            n++;
        }
    /* Hydrogen en mode Pattern (et le groove retenu) à chaque départ : au
     * démarrage de la session, il ignore la commande tant qu'aucun morceau
     * n'est chargé (« No song set yet ») et reste alors en mode Song. */
    if (h2_listening()) {
        h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
        if (g_seq.active) seq_start();               // intro / décompte, puis la partie
        else if (g_pattern >= 0) h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", (float)g_pattern);
    }
    usleep(40000);                         // laisser SooperLooper armer les boucles
    jack_transport_start(g_cfg.client);
    /* Boucles muettes au dernier Stop : SooperLooper efface un mute reçu avant
     * le vrai départ de la boucle (vérifié) ; on l'envoie au départ puis on le
     * répète pendant 40 ms (mute_on ne fait rien si elle l'est déjà). */
    for (int delay : {0, 5000, 10000, 25000}) {
        if (drums_only) break;
        if (delay) usleep(delay);
        for (size_t i = 0; i < g_loops.size(); i++)
            if (g_loops[i].len > 0 && g_loops[i].keep_muted) sl_send("/sl/" + std::to_string(i) + "/hit", {"mute_on"});
    }
    return "ok play (mesure 1, " + std::to_string(n) + " boucle(s))";
}


/* ------------------------------------------------- DLS Looper (pb_ctl) --- */

/* État SooperLooper -> état affiché par le plugin (enum pb_loop_state).
 * États de SooperLooper : 0 arrêt, 1 attente départ, 2 enregistrement,
 * 3 attente fin, 4 lecture, 5 overdub, 6 multiply, 7 insert, 8 replace,
 * 10 muet, 12 une fois, 14 pause, 20 arrêt muet. */
uint8_t map_state(const Loop &L)
{
    if (L.state == 1 || L.state == 3) return PB_LS_WAIT;
    if (L.state == SL_RECORD) return PB_LS_RECORD;
    if (L.len <= 0) return PB_LS_EMPTY;
    switch (L.state) {
    case 4: case 12:        return PB_LS_PLAY;
    case 5:                 return PB_LS_OVERDUB;
    case 10: case 20:       return PB_LS_MUTE;
    case 0: case SL_PAUSED: return L.keep_muted ? PB_LS_MUTE : PB_LS_PAUSE;   // muette au prochain Play
    default:                return PB_LS_OTHER;
    }
}

void publish_states()
{
    pb_ctl *k = g_ctl.load(std::memory_order_acquire);
    if (!k) return;
    for (uint32_t i = 0; i < PB_CTL_LOOPS; i++) {
        const uint8_t st = g_sl_ok && i < g_loops.size() ? map_state(g_loops[i]) : (uint8_t)PB_LS_OFFLINE;
        PB_STORE_RLX(&k->loop_state[i], st);
    }
    PB_STORE_RLX(&k->loop_count, g_sl_ok ? (uint32_t)g_loops.size() : 0u);
    PB_STORE_REL(&k->sl_ok, g_sl_ok ? 1u : 0u);
    {
        uint32_t pc, vc;
        seq_display(pc, vc);
        PB_STORE_RLX(&k->groove_cur, g_seq.active ? vc : (uint32_t)groove_rel());
        PB_STORE_RLX(&k->part_cur, pc);
        uint32_t st, ty;
        browse_display(st, ty);
        PB_STORE_RLX(&k->style_cur, st);
        PB_STORE_RLX(&k->type_cur, ty);
        PB_STORE_RLX(&k->var_cur, vc);
    }
    PB_STORE_RLX(&k->groove_count, (uint32_t)groove_count());
    PB_STORE_REL(&k->h2_ok, h2_listening() ? 1u : 0u);
}

/* Ce que le client a déjà vu du plugin propriétaire. */
uint64_t g_lp_owner = 0;
uint32_t g_lp_btn[PB_BTN_COUNT];
uint32_t g_lp_loop = 0;
uint32_t g_lp_tempo = 0;          // centièmes de BPM, dernier appliqué
uint32_t g_tempo_seen = 0;        // dernière valeur lue du plugin
double g_tempo_since = 0;         // depuis quand elle n'a pas bougé
int g_sel = 0;                     // boucle choisie (0 = boucle 1)
double g_ctl_next_try = 0;
bool g_refresh_now = false;

void select_loop(uint32_t v)
{
    if (v < 1 || v > PB_CTL_LOOPS) return;
    g_sel = (int)v - 1;
    sl_send("/set", {"selected_loop_num"}, {(float)g_sel});
}

bool transport_rolling()
{
    jack_position_t pos;
    return jack_transport_query(g_cfg.client, &pos) == JackTransportRolling;
}

std::string looper_button(int b)
{
    static const char *names[PB_BTN_COUNT] = {"record", "overdub", "undo", "redo", "mute", "undo_all", "play/stop"};
    if (b == PB_BTN_PLAYSTOP) return transport_rolling() ? stop_all() : play_from_start();
    if (!g_sl_ok) return "erreur : SooperLooper ne répond pas";
    /* Mute à l'arrêt : SooperLooper ne le garde pas sur une boucle en pause ;
     * le pont le retient et l'applique au prochain Play. */
    if (b == PB_BTN_MUTE && !transport_rolling() && g_sel >= 0 && g_sel < (int)g_loops.size()) {
        Loop &L = g_loops[g_sel];
        L.keep_muted = !L.keep_muted;
        publish_states();
        return std::string("ok boucle ") + std::to_string(g_sel + 1) +
               (L.keep_muted ? " muette au prochain Play" : " avec le son au prochain Play");
    }
    sl_send("/sl/" + std::to_string(g_sel) + "/hit", {names[b]});
    return std::string("ok ") + names[b] + " (boucle " + std::to_string(g_sel + 1) + ")";
}

/* ---------------------------------------------------- DLS Drum (pb_ctl) --- */

uint64_t g_dr_owner = 0;
uint32_t g_dr_btn[PB_DBTN_COUNT];
uint32_t g_dr_groove = 0;
uint32_t g_dr_part_ui = 0, g_dr_var_ui = 0;     // listes Part et Variation vues
uint32_t g_dr_intro_ui = 0, g_dr_outro_ui = 0;  // listes Intro et Outro vues
uint32_t g_dr_metro = 2;                        // 2 = à appliquer
uint32_t g_dr_tempo = 0, g_dr_tempo_seen = 0;   // centièmes de BPM
double g_dr_tempo_since = 0;
std::vector<double> g_taps;                     // instants des derniers appuis Tap
int g_bpm_delta = 0;                            // pas de BPM +/- pas encore appliqués
int g_bpm_target = 0;                           // tempo visé
double g_bpm_apply_t = 0;

/* BPM +/- : pas regroupés, appliqués au plus 10 fois par seconde (Hydrogen
 * déborde si on lui envoie le tempo trop souvent). */
void bpm_steps(pb_ctl *k)
{
    if (!g_bpm_delta || now_s() - g_bpm_apply_t < 0.1) return;
    /* Toujours à partir du tempo actuel (dernier appliqué par le pont : Tap,
     * bouton Tempo ou BPM +/-), pas d'une ancienne série. */
    const uint32_t c = g_last_tempo_c ? g_last_tempo_c : PB_LOAD_RLX(&k->bpm);
    g_bpm_target = (c ? (int)std::lround(c * 0.01) : 120) + g_bpm_delta;
    g_bpm_delta = 0;
    if (g_bpm_target < 40) g_bpm_target = 40;
    if (g_bpm_target > 240) g_bpm_target = 240;
    say("DLS Drum : BPM +/- -> %s", set_tempo(std::to_string(g_bpm_target)).c_str());
    g_ui_tempo = 0;                                   // affiché au relâchement
    g_ui_tempo_late = g_bpm_target;
    g_bpm_apply_t = g_bpm_step_t = now_s();
}

/* Groove précédent / suivant (à partir du groove retenu). */
std::string step_groove(int dir)
{
    const int count = groove_count() > 0 ? groove_count() : 16;
    int n = g_pattern < 0 ? 1 : groove_rel() + dir;           // à partir de 1, dans la chanson
    if (n < 1) n = 1;
    if (n > count) n = count;
    return set_groove(std::to_string(n));
}

/* Tap tempo : on tape au moins 3 coups ; le tempo change UNE fois, quand on
 * arrête de taper (pas de coup depuis 1 s, ou 1,6 intervalle aux tempos
 * lents), avec la moyenne des 8 derniers coups. La batterie ne saute donc pas
 * à chaque coup. */
double tap_period()
{
    return g_taps.size() < 2 ? 0 : (g_taps.back() - g_taps.front()) / (double)(g_taps.size() - 1);
}

std::string tap_tempo()
{
    g_taps.push_back(now_s());
    if (g_taps.size() > 8) g_taps.erase(g_taps.begin());
    return "ok tap " + std::to_string(g_taps.size());
}

/* Appelé à chaque passage : applique le tempo tapé quand on a fini. */
void tap_finish()
{
    if (g_taps.empty()) return;
    const double wait = std::max(1.0, 1.6 * tap_period());
    if (now_s() - g_taps.back() < wait) return;
    const size_t n = g_taps.size();
    const double period = tap_period();
    g_taps.clear();
    if (n < 3) { say("DLS Drum : tap ignoré (%zu coup(s), il en faut 3)", n); return; }
    const double bpm = 60.0 / period;
    if (bpm < 40 || bpm > 240) { say("DLS Drum : tap hors limites (%.1f BPM ; 40 à 240)", bpm); return; }
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", bpm);         // entier : comme le bouton Tempo
    say("DLS Drum : tap (%zu coups) -> %s", n, set_tempo(b).c_str());
}

std::string g_drum_song;           // morceau Hydrogen ouvert par le pont (chemin)
std::string open_drum_song(const std::string &path, int groove = 1);

/* Kit (liste fixe : numéro -> fichier .kit) et morceau (sélecteur de fichier,
 * lu sous verrou de séquence) choisis dans DLS Drum. Le morceau est « rendu »
 * sur le kit (version mise en cache, faite par scripts/rendu_kit.py), puis
 * ouvert seulement à l'arrêt (Hydrogen recharge tout et arrête la lecture). */
uint32_t g_dr_song_seq = 1;                     // impair : jamais lu
bool g_dr_song_preset_var = false;              // chanson du preset : garder sa variation
uint32_t g_dr_kit_num = 0xffffffffu;            // jamais lu
std::string g_dr_song_sel, g_dr_kit_sel;        // choix actuels dans DLS Drum
bool g_dr_song_pending = false;                 // à ouvrir au prochain arrêt
int g_dr_song_groove = 1;                       // groove de départ du morceau en attente
bool g_dr_song_preset = false;                  // prochaine lecture = choix du preset

/* Lit un chemin publié sous verrou de séquence ; true s'il y a du nouveau. */
bool read_seq_path(uint32_t *seq, const char *src, uint32_t &last, std::string &out)
{
    const uint32_t s1 = PB_LOAD_ACQ(seq);
    if (s1 == last || (s1 & 1u)) return false;
    char buf[PB_CTL_PATH];
    std::memcpy(buf, src, sizeof buf);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (PB_LOAD_ACQ(seq) != s1) return false;     // réécrit pendant la copie : au prochain passage
    last = s1;
    buf[sizeof buf - 1] = 0;
    out = buf;
    return true;
}

std::string home_dir() { const char *h = std::getenv("HOME"); return h ? h : "/tmp"; }

std::string scripts_dir()
{
    const char *sd = std::getenv("PB_SCRIPTS");
    return sd ? sd : home_dir() + "/pedal-bridge-build/src/scripts";
}

/* Kit n (1..N) de la liste : n-ième .kit de scripts/kits en ordre alphabétique
 * (même ordre que la liste écrite par gen_dls_ttl.py). "" si inconnu. */
std::string kit_file(uint32_t n)
{
    const std::string dir = scripts_dir() + "/kits";
    std::vector<std::string> kits;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string f = e->d_name;
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".kit") == 0) kits.push_back(f);
        }
        closedir(d);
    }
    std::sort(kits.begin(), kits.end());
    return n >= 1 && n <= kits.size() ? dir + "/" + kits[n - 1] : std::string();
}

std::string shell_quote(const std::string &s)
{
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}

long mtime(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_mtime : -1;
}

/* Morceau + kit -> fichier à ouvrir. Sans kit : le morceau tel quel. La
 * version pour le kit est refaite si elle manque ou si le morceau ou le .kit
 * est plus récent. En cas d'échec : le morceau tel quel (message). */
std::string render_song(const std::string &song, const std::string &kit)
{
    if (kit.empty() || file_size(kit) <= 0) return song;
    auto base = [](const std::string &f) { const size_t p = f.rfind('/'); return p == std::string::npos ? f : f.substr(p + 1); };
    std::string kname = base(kit);
    if (kname.size() > 4 && kname.compare(kname.size() - 4, 4, ".kit") == 0) kname.resize(kname.size() - 4);
    /* Même rangement que le dossier des morceaux (sous-dossiers de style) :
     * deux morceaux de même nom dans deux styles ne se mélangent pas. */
    const std::string marker = "/DLS Drums/";
    const size_t m = song.find(marker);
    const std::string rel = m == std::string::npos ? base(song) : song.substr(m + marker.size());
    const std::string out = home_dir() + "/dls-batterie/rendus/" + kname + "/" + rel;
    if (mtime(out) >= 0 && mtime(out) >= mtime(song) && mtime(out) >= mtime(kit)) return out;
    const std::string dir = out.substr(0, out.rfind('/'));
    mkdirs(dir);
    const std::string scripts = scripts_dir();
    const std::string cmd = "python3 " + shell_quote(scripts + "/rendu_kit.py") + " " + shell_quote(song) + " " +
                            shell_quote(kit) + " " + shell_quote(out) + " 2>&1";
    std::string msg;
    FILE *f = popen(cmd.c_str(), "r");
    int rc = -1;
    if (f) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) msg += line;
        rc = pclose(f);
    }
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (rc != 0 || file_size(out) <= 0) {
        say("DLS Drum : version pour le kit %s impossible (%s) ; morceau tel quel", kname.c_str(), msg.c_str());
        return song;
    }
    say("DLS Drum : %s", msg.c_str());
    return out;
}

/* Gros morceau du kit : ~/dls-batterie/gros/<kit>.h2song (+ .index.txt). */
std::map<std::string, std::pair<int, int>> g_index;   // chanson (chemin relatif) -> (premier, nombre)
std::string g_index_for;                              // gros morceau dont l'index est chargé

std::string kit_name(const std::string &kit)
{
    std::string n = kit.substr(kit.rfind('/') + 1);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".kit") == 0) n.resize(n.size() - 4);
    return n;
}

std::string big_song_path(const std::string &kit)
{
    return kit.empty() ? std::string() : home_dir() + "/dls-batterie/gros/" + kit_name(kit) + ".h2song";
}

/* Dossier des chansons et chemin relatif d'une chanson (« Rock/DLS Rock.h2song »). */
const char *SONGS_MARKER = "/DLS Drums/";
std::string songs_dir_of(const std::string &song)
{
    const size_t m = song.find(SONGS_MARKER);
    return m == std::string::npos ? std::string() : song.substr(0, m + std::strlen(SONGS_MARKER) - 1);
}
std::string song_rel(const std::string &song)
{
    const size_t m = song.find(SONGS_MARKER);
    return m == std::string::npos ? song : song.substr(m + std::strlen(SONGS_MARKER));
}

/* Date du plus récent .h2song du dossier (et de ses sous-dossiers). */
long newest_song(const std::string &dir)
{
    long best = -1;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string f = dir + "/" + n;
            struct stat st;
            if (stat(f.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) best = std::max(best, newest_song(f));
            else if (n.size() > 7 && n.compare(n.size() - 7, 7, ".h2song") == 0) best = std::max(best, (long)st.st_mtime);
        }
        closedir(d);
    }
    return best;
}

bool big_stale(const std::string &big, const std::string &kit, const std::string &songs)
{
    const long t = mtime(big);
    return t < 0 || mtime(big + ".index.txt") < 0 || t < mtime(kit) || t < newest_song(songs) ||
           t < mtime(scripts_dir() + "/gros_morceau.py");
}

bool build_big(const std::string &big, const std::string &kit, const std::string &songs)
{
    mkdirs(big.substr(0, big.rfind('/')));
    const std::string cmd = "python3 " + shell_quote(scripts_dir() + "/gros_morceau.py") + " " + shell_quote(songs) +
                            " " + shell_quote(kit) + " " + shell_quote(big) + " 2>&1";
    std::string msg;
    int rc = -1;
    if (FILE *f = popen(cmd.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) msg = line;      // la dernière ligne résume
        rc = pclose(f);
    }
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    say("DLS Drum : gros morceau %s : %s", kit_name(kit).c_str(), rc == 0 ? msg.c_str() : ("échec : " + msg).c_str());
    return rc == 0 && file_size(big) > 0;
}

/* Structure des parties de chaque chanson (gros_morceau.py, .parts.txt) :
 * chanson -> clé (part:Verse, fill:Verse, fill:*, intro, outro, transition)
 * -> numéros de groove dans la chanson (à partir de 1). */
std::map<std::string, std::map<std::string, std::vector<std::vector<int>>>> g_parts;
int g_silence_abs = -1;                              // pattern « Silence » (décompte du métronome)
int g_final_abs = -1;                                // pattern « Final hit » (fin sans outro)
std::string g_song_rel;                               // chanson en cours (chemin relatif)

void load_index(const std::string &big)
{
    g_index.clear();
    g_index_for = big;
    g_parts.clear();
    {
        std::ifstream pf(big + ".parts.txt");
        std::string l;
        while (std::getline(pf, l)) {
            const size_t a = l.find('\t'), b = l.find('\t', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::vector<std::vector<int>> vars;           // « 4+5,6+7 » : variations, phrases
            std::stringstream ss(l.substr(b + 1));
            std::string v;
            while (std::getline(ss, v, ',')) {
                std::vector<int> ph;
                std::stringstream sv(v);
                std::string n;
                while (std::getline(sv, n, '+')) if (!n.empty()) ph.push_back(std::atoi(n.c_str()));
                if (!ph.empty()) vars.push_back(ph);
            }
            g_parts[l.substr(0, a)][l.substr(a + 1, b - a - 1)] = vars;
        }
    }
    std::ifstream f(big + ".index.txt");
    std::string line;
    while (std::getline(f, line)) {
        const size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        if (line.substr(b + 1) == "@silence") { g_silence_abs = std::atoi(line.c_str()); continue; }
        if (line.substr(b + 1) == "@final") { g_final_abs = std::atoi(line.c_str()); continue; }
        g_index[line.substr(b + 1)] = {std::atoi(line.c_str()), std::atoi(line.c_str() + a + 1)};
    }
}

/* Choix (kit, chanson) avec le gros morceau. Retourne false si on doit
 * attendre l'arrêt (kit à charger pendant la lecture). */
bool apply_big_choice(pb_ctl *k)
{
    const std::string big = big_song_path(g_dr_kit_sel);
    const std::string songs = songs_dir_of(g_dr_song_sel);
    if (big.empty() || songs.empty()) return true;           // pas de gros morceau : géré ailleurs
    const bool stale = big_stale(big, g_dr_kit_sel, songs);
    const bool load = stale || big != g_drum_song;
    if (load && transport_rolling()) return false;          // Hydrogen arrête tout en ouvrant : au Stop
    if (stale && !build_big(big, g_dr_kit_sel, songs)) return true;
    if (load) say("DLS Drum : %s", open_drum_song(big, 0).c_str());
    if (g_index_for != big || stale) load_index(big);
    auto it = g_index.find(song_rel(g_dr_song_sel));
    if (it == g_index.end()) {
        /* Chanson rangée ailleurs depuis (scripts/ranger.py) : .alias.txt du
         * dossier, « ancien<TAB>nouveau » ; on suit au plus 5 renvois. */
        std::map<std::string, std::string> alias;
        std::ifstream af(songs + "/.alias.txt");
        std::string line;
        while (std::getline(af, line)) {
            const size_t t = line.find('\t');
            if (t != std::string::npos) alias[line.substr(0, t)] = line.substr(t + 1);
        }
        std::string rel = song_rel(g_dr_song_sel);
        for (int hop = 0; hop < 5 && it == g_index.end(); hop++) {
            const auto a = alias.find(rel);
            if (a == alias.end()) break;
            rel = a->second;
            it = g_index.find(rel);
        }
        if (it != g_index.end()) say("DLS Drum : %s rangé sous %s", song_rel(g_dr_song_sel).c_str(), rel.c_str());
    }
    if (it == g_index.end()) {
        /* Preset d'avant le rangement par style (« Funk.h2song » à la racine) :
         * chercher « <style>/Funk.h2song » ou « <style>/RP Bardet Funk.h2song ». */
        const std::string base = song_rel(g_dr_song_sel).substr(song_rel(g_dr_song_sel).rfind('/') + 1);
        for (auto j = g_index.begin(); j != g_index.end(); ++j) {
            const std::string &k = j->first;
            const std::string kb = k.substr(k.rfind('/') + 1);
            if (kb == base || kb == "RP Bardet " + base) { it = j; break; }
        }
        if (it != g_index.end()) say("DLS Drum : %s retrouvé : %s", base.c_str(), it->first.c_str());
    }
    if (it == g_index.end()) {
        say("DLS Drum : %s absent du gros morceau", song_rel(g_dr_song_sel).c_str());
        return true;
    }
    g_off = it->second.first;
    g_cnt = it->second.second;
    g_song_rel = it->first;
    browse_check(songs);
    /* Nouvelle chanson (groove 1 demandé) : premier groove de la première
     * partie jouable (couplet…), pas l'intro, comme au BeatBuddy. */
    if (g_dr_song_groove <= 1) {
        const auto ps = g_parts.find(g_song_rel);
        if (ps != g_parts.end())
            for (const char *p : {"part:Verse", "part:Pre-Chorus", "part:Chorus", "part:Bridge", "part:Groove"}) {
                const auto q = ps->second.find(p);
                if (q != ps->second.end() && !q->second.empty()) { g_dr_song_groove = q->second.front().front(); break; }
            }
    }
    seq_setup(PB_LOAD_RLX(&k->drum_part), g_dr_song_preset_var ? PB_LOAD_RLX(&k->drum_var) : UINT32_MAX);
    g_dr_song_preset_var = false;
    if (g_seq.active) {
        say("DLS Drum : %s (patterns %d à %d) ; %s %c", song_rel(g_dr_song_sel).c_str(), g_off + 1, g_off + g_cnt,
            g_song.parts[g_seq.part].c_str(), 'A' + std::min(g_seq.var[g_seq.part], 25));
    } else {
        say("DLS Drum : %s (patterns %d à %d) ; %s", song_rel(g_dr_song_sel).c_str(), g_off + 1, g_off + g_cnt,
            set_groove(std::to_string(std::min(std::max(g_dr_song_groove, 1), g_cnt))).c_str());
    }
    /* Le menu montre encore l'ancien groove jusqu'à ce que PiPedal applique
     * la mise à jour (websocket) : le considérer comme vu ; son écho est attendu. */
    g_dr_groove = PB_LOAD_RLX(&k->drum_groove);
    g_refresh_now = true;
    return true;
}

void drum_song_poll(pb_ctl *k)
{
    std::string path;
    bool changed = false;
    const uint32_t kn = PB_LOAD_RLX(&k->drum_kit_num);
    if (kn != g_dr_kit_num) g_dr_kit_num = kn, path = kn ? kit_file(kn) : std::string();
    else path = g_dr_kit_sel;
    if (path != g_dr_kit_sel) {
        g_dr_kit_sel = path;
        changed = true;
        /* Même style sur un autre kit : on garde le groove qui joue. */
        g_dr_song_groove = g_pattern >= 0 ? groove_rel() : (int)PB_LOAD_RLX(&k->drum_groove);
    }
    if (read_seq_path(&k->drum_song_seq, k->drum_song, g_dr_song_seq, path)) {
        /* Choix du preset : avec le groove du menu (sauvegardé avec lui).
         * Morceau choisi à la main : nouveau style, on repart au Groove 1. */
        if (g_dr_song_preset) { g_dr_song_groove = (int)PB_LOAD_RLX(&k->drum_groove); g_dr_song_preset_var = true; }
        else if (path != g_dr_song_sel) g_dr_song_groove = 1;
        g_dr_song_preset = false;
        if (path != g_dr_song_sel) { g_dr_song_sel = path; changed = true; }
    }
    if (changed && !g_dr_song_sel.empty()) g_dr_song_pending = true;
    /* Avec un kit et une chanson du dossier : gros morceau (changer de
     * chanson = sauter, même en jouant ; changer de kit = recharger, à l'arrêt). */
    if (g_dr_song_pending && !g_dr_kit_sel.empty() && !songs_dir_of(g_dr_song_sel).empty()) {
        if (apply_big_choice(k)) g_dr_song_pending = false;
        else if (changed) say("DLS Drum : nouveau kit chargé au prochain Stop");
        return;
    }
    if (g_dr_song_pending && !transport_rolling()) {
        g_dr_song_pending = false;
        const std::string eff = render_song(g_dr_song_sel, g_dr_kit_sel);
        if (eff == g_drum_song) return;              // déjà ouvert
        say("DLS Drum : %s", open_drum_song(eff, g_dr_song_groove).c_str());
        /* Le menu montre encore l'ancien groove jusqu'à ce que PiPedal applique
         * la mise à jour (websocket) : le considérer comme vu, sinon il serait
         * réappliqué ici même ; son écho (le nouveau groove) est attendu. */
        g_dr_groove = PB_LOAD_RLX(&k->drum_groove);
        g_refresh_now = true;
    }
}

/* --- Séquenceur de batterie (à la BeatBuddy / RC-600) ----------------------
 * Une chanson = intro (une fois), parties (Verse, Pre-Chorus, Chorus, Bridge,
 * ou Groove) qui tournent en boucle, chacune avec ses variations (phrases :
 * moitiés de 4 mesures enchaînées) et ses fills, transitions, outro (une fois,
 * puis tout s'arrête). Hydrogen redémarre un pattern choisi à son début
 * (vérifié) : le pont choisit chaque pattern ~10 ms avant son départ, d'après
 * la position du transport JACK (en temps). Fill, variation, partie et outro
 * partent à la mesure suivante. */

const char *PART_UI[] = {"Verse", "Pre-Chorus", "Chorus", "Bridge"};     // liste Part du plugin
int part_code(const std::string &p)       // pour Playing : 2 Verse … 6 Groove
{
    if (p == "Verse") return 2;
    if (p == "Pre-Chorus") return 3;
    if (p == "Chorus") return 4;
    if (p == "Bridge") return 5;
    return 6;
}

void build_song_def()
{
    g_song = SongDef();
    const auto sp = g_parts.find(g_song_rel);
    if (sp == g_parts.end()) return;
    auto absv = [](const std::vector<int> &v) { std::vector<int> o; for (int n : v) o.push_back(g_off + n - 1); return o; };
    for (const char *pn : {"Verse", "Pre-Chorus", "Chorus", "Bridge", "Groove"}) {
        const auto q = sp->second.find(std::string("part:") + pn);
        if (q == sp->second.end() || q->second.empty()) continue;
        g_song.parts.push_back(pn);
        for (const auto &ph : q->second) g_song.vars[pn].push_back(absv(ph));
    }
    for (const auto &kv : sp->second) {
        const std::string &k = kv.first;
        if (k.compare(0, 5, "fill:") == 0)
            for (const auto &ph : kv.second) for (int a : absv(ph)) g_song.fills[k.substr(5)].push_back(a);
        if (k == "intro") for (const auto &ph : kv.second) g_song.intro.push_back(absv(ph));
        if (k == "outro") for (const auto &ph : kv.second) g_song.outro.push_back(absv(ph));
        if (k == "transition") for (const auto &ph : kv.second) for (int a : absv(ph)) g_song.transition.push_back(a);
    }
    /* Pack de grooves (JJ…) : pas de Verse ni de Chorus à la source. Les 4
     * parties existent quand même et puisent toutes dans les mêmes grooves ;
     * au départ Verse = A, Pre-Chorus = B, Chorus = C, Bridge = D. */
    if (g_song.parts.size() == 1 && g_song.parts[0] == "Groove") {
        const auto pool = g_song.vars["Groove"];
        g_song.parts.clear();
        g_song.vars.clear();
        for (const char *pn : PART_UI) { g_song.parts.push_back(pn); g_song.vars[pn] = pool; }
        g_song.pool = true;
    }
}

/* Mémoire : variation choisie pour chaque partie de chaque chanson, gardée
 * sur le Pi (~/dls-batterie/choix.txt : « chanson<TAB>partie<TAB>A »), quel
 * que soit le preset. Écrite à chaque changement (hors temps réel). */
std::map<std::string, std::map<std::string, int>> g_memo;   // chanson -> partie -> variation ; "Intro", "Outro" -> choix
bool g_memo_read = false;
std::map<std::string, std::string> g_last_beat;   // Browse : "Style/Type" -> dernière chanson choisie
std::map<std::string, std::string> g_last_type;   // Browse : "Style" -> dernier "Style/Type"

std::string memo_path() { return home_dir() + "/dls-batterie/choix.txt"; }

void memo_load()
{
    if (g_memo_read) return;
    g_memo_read = true;
    std::ifstream f(memo_path());
    std::string l;
    while (std::getline(f, l)) {
        const size_t a = l.find('\t'), b = l.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos || b + 1 >= l.size()) continue;
        const std::string song = l.substr(0, a), key = l.substr(a + 1, b - a - 1), val = l.substr(b + 1);
        if (song == "@browse-type") { g_last_beat[key] = val; continue; }       // Browse : type -> beat
        if (song == "@browse-style") { g_last_type[key] = val; continue; }      // Browse : style -> type
        const char c = val[0];
        const int letter = c >= 'A' && c <= 'Z' && val.size() == 1 ? c - 'A' : -2;
        int v;
        if (key == "Intro") v = val == "Off" ? 1 : val == "Metronome" ? 2 : letter >= 0 ? letter + 3 : -2;
        else if (key == "Outro") v = letter >= 0 ? letter + 1 : -2;
        else v = c == '-' ? -1 : letter >= 0 ? letter : std::atoi(val.c_str()) - 1;
        if (v >= -1) g_memo[song][key] = v;
    }
}

/* Réécrit choix.txt (fichier temporaire puis renommage). */
void memo_write()
{
    const std::string path = memo_path(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        for (const auto &song : g_memo)
            for (const auto &pv : song.second) {
                const int v = pv.second;
                f << song.first << '\t' << pv.first << '\t';
                if (pv.first == "Intro") {
                    if (v == 1) f << "Off"; else if (v == 2) f << "Metronome"; else f << (char)('A' + std::min(v - 3, 25));
                } else if (pv.first == "Outro") {
                    f << (char)('A' + std::min(std::max(v - 1, 0), 25));
                } else if (v < 0) f << '-'; else if (v < 26) f << (char)('A' + v); else f << v + 1;
                f << '\n';
            }
        for (const auto &kv : g_last_beat) f << "@browse-type\t" << kv.first << '\t' << kv.second << '\n';
        for (const auto &kv : g_last_type) f << "@browse-style\t" << kv.first << '\t' << kv.second << '\n';
        if (!f) return;
    }
    std::rename(tmp.c_str(), path.c_str());
}

/* Intro ou Outro choisi pour la chanson en cours (key "Intro" / "Outro"). */
void memo_choice(const char *key, int v)
{
    if (g_song_rel.empty() || v < 1) return;
    memo_load();
    auto &song = g_memo[g_song_rel];
    const auto q = song.find(key);
    if (q != song.end() && q->second == v) return;
    song[key] = v;
    memo_write();
}

void memo_save(int part)
{
    if (part < 0 || part >= (int)g_song.parts.size() || g_song_rel.empty()) return;
    auto &song = g_memo[g_song_rel];
    const auto q = song.find(g_song.parts[part]);
    if (q != song.end() && q->second == g_seq.var[part]) return;
    song[g_song.parts[part]] = g_seq.var[part];
    memo_write();
}

/* Partie de la liste du plugin (1 Verse … 4 Bridge) -> index dans la chanson
 * (la plus proche dans l'ordre si elle n'existe pas). */
int part_from_ui(uint32_t ui)
{
    if (g_song.parts.empty()) return 0;
    const std::string want = ui >= 1 && ui <= 4 ? PART_UI[ui - 1] : "Verse";
    for (size_t i = 0; i < g_song.parts.size(); i++) if (g_song.parts[i] == want) return (int)i;
    const char *order[] = {"Verse", "Pre-Chorus", "Chorus", "Bridge"};
    int wi = 0;
    for (int i = 0; i < 4; i++) if (want == order[i]) wi = i;
    for (int d = 1; d < 4; d++)
        for (int dir : {-1, 1}) {
            const int j = wi + dir * d;
            if (j < 0 || j > 3) continue;
            for (size_t i = 0; i < g_song.parts.size(); i++) if (g_song.parts[i] == order[j]) return (int)i;
        }
    return 0;                                         // ex. « Groove » seulement
}

const std::vector<int> &phrase_of(int part)
{
    static const std::vector<int> none;
    if (part < 0 || part >= (int)g_song.parts.size()) return none;
    const auto &vs = g_song.vars[g_song.parts[part]];
    if (vs.empty()) return none;
    int v = g_seq.var[part];
    if (v < 0) return none;                           // None : partie sautée
    if (v >= (int)vs.size()) v = (int)vs.size() - 1;
    return vs[v];
}

std::string var_name(int v) { return v < 0 ? "None" : v < 26 ? std::string(1, (char)('A' + v)) : std::to_string(v + 1); }

/* Partie active suivante (dir +1) ou précédente (-1), en revenant au début
 * après la dernière ; -1 s'il n'y en a pas d'autre que « from ». */
int next_on(int from, int dir)
{
    const int n = (int)g_song.parts.size();
    for (int k = 1; k < n; k++) {
        const int j = ((from + dir * k) % n + n) % n;
        if (g_seq.var[j] >= 0) return j;
    }
    return -1;
}

double size_beats(int abs) { return abs >= 0 && abs < (int)g_pat_sizes.size() ? g_pat_sizes[abs] / 48.0 : 4.0; }

int phrase_ticks(const std::vector<int> &ph)
{
    int t = 0;
    for (int a : ph) t += a >= 0 && a < (int)g_pat_sizes.size() ? g_pat_sizes[a] : 192;
    return t;
}

/* Position du transport en temps (noires) depuis la mesure 1 ; false à l'arrêt. */
bool transport_beats(double &now_b, double &bpb, double &beat_s)
{
    jack_position_t pos;
    if (jack_transport_query(g_cfg.client, &pos) != JackTransportRolling) return false;
    if (!(pos.valid & JackPositionBBT) || pos.beats_per_minute <= 0) return false;
    bpb = pos.beats_per_bar;
    beat_s = 60.0 / pos.beats_per_minute;
    now_b = (pos.bar - 1) * bpb + (pos.beat - 1) + pos.tick / pos.ticks_per_beat;
    return true;
}

void seq_select(int abs)
{
    if (abs < 0) return;
    h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", (float)abs);
    g_seq.cur_abs = abs;
}

/* Cycle de SooperLooper = longueur de la phrase (4/4 ou 3/4 entières). */
void seq_cycle(const std::vector<int> &ph)
{
    if (ph.empty()) return;
    apply_cycle(ph[0]);                                // temps par mesure de pb_timebase
    const int ticks = phrase_ticks(ph);
    if (ticks > 0 && (ticks % 192 == 0 || ticks % 144 == 0)) sl_send("/set", {"eighth_per_cycle"}, {(float)(ticks / 24)});
}

/* Prochaine barre (temps absolu), assez loin pour y arriver à temps. */
bool next_bar(double &at, double &bpb)
{
    double now_b, beat_s;
    if (!transport_beats(now_b, bpb, beat_s)) return false;
    at = std::ceil(now_b / bpb + 1e-9) * bpb;
    if ((at - now_b) * beat_s < 0.03) at += bpb;
    return true;
}

/* Fills de la partie pi (sinon les fills communs, sinon n'importe lesquels) ;
 * used : la clé de la liste prise. nullptr s'il n'y en a pas. */
const std::vector<int> *fill_list(int pi, std::string &used)
{
    if (pi < 0) pi = g_seq.part;
    const std::string part = g_seq.in_once && g_seq.once_kind == 1 ? "Intro" :
                             (pi >= (int)g_song.parts.size() ? "" : g_song.parts[pi]);
    for (const std::string &key : {part, std::string("*")}) {
        const auto it = g_song.fills.find(key);
        if (it != g_song.fills.end() && !it->second.empty()) { used = key; return &it->second; }
    }
    for (const auto &kv : g_song.fills) if (!kv.second.empty()) { used = kv.first; return &kv.second; }
    return nullptr;
}

int fill_for_part(int pi = -1)
{
    std::string used;                                  // liste de fills prise (son tour est partagé)
    const std::vector<int> *f = fill_list(pi, used);
    if (!f) return -1;
    size_t &turn = g_fill_turn[g_song_rel + "/" + used];
    return (*f)[turn++ % f->size()];
}

/* Nouvelle chanson : partie de la liste, variations remises à A (sauf la
 * variation de la liste pour cette partie). Choisit le premier pattern. */
void seq_setup(uint32_t part_ui, uint32_t var_ui)
{
    const Seq keep = g_seq;                            // pour enchaîner en jouant
    build_song_def();
    g_seq = Seq();
    g_seq.active = !g_song.parts.empty();
    if (!g_seq.active) return;
    g_seq.part = part_from_ui(part_ui);
    /* Variation de chaque partie : mémoire, sinon celle du preset (partie
     * affichée), sinon A (pack de grooves : A, B, C, D). */
    memo_load();
    const auto mem = g_memo.find(g_song_rel);
    for (int i = 0; i < (int)g_song.parts.size(); i++) {
        const int n = (int)g_song.vars[g_song.parts[i]].size();
        int v = g_song.pool ? i : 0;
        if (i == g_seq.part && var_ui != UINT32_MAX) v = (int)var_ui - 1;     // 0 : None
        if (mem != g_memo.end()) {
            const auto q = mem->second.find(g_song.parts[i]);
            if (q != mem->second.end()) v = q->second;
        }
        g_seq.var[i] = std::min(std::max(v, -1), std::max(n - 1, 0));
    }
    if (g_seq.var[g_seq.part] < 0) {                  // partie affichée sautée : la suivante
        const int j = next_on(g_seq.part, +1);
        if (j >= 0) g_seq.part = j;
        else g_seq.var[g_seq.part] = 0;               // toutes à None : on garde celle-ci
    }
    /* Intro et Outro retenus pour cette chanson (montrés dans PiPedal) ;
     * jamais réglés : on garde ceux en cours (preset). Pendant 1,5 s, un
     * changement de ces listes venu du preset ne les écrase pas. */
    g_song_set_t = now_s();
    if (mem != g_memo.end()) {
        const auto qi = mem->second.find("Intro"), qo = mem->second.find("Outro");
        if (qi != mem->second.end()) { g_intro_choice = qi->second; g_ui_intro = qi->second; }
        if (qo != mem->second.end()) { g_outro_choice = qo->second; g_ui_outro = qo->second; }
    }
    g_seq.loop_part = g_seq.part;
    g_seq.loop = phrase_of(g_seq.part);
    double at, bpb;
    if (keep.active && next_bar(at, bpb)) {           // en jouant : le pattern en cours finit sa mesure,
        g_seq.next_loop = g_seq.loop;                  // la nouvelle chanson part à la suivante
        g_seq.next_part = g_seq.part;
        g_seq.loop = {keep.cur_abs};
        g_seq.cur_abs = keep.cur_abs;
        g_seq.item_start = keep.item_start;
        g_seq.item_len = keep.item_len;
        g_seq.metro_after = keep.metro_after;
        g_seq.switch_at = at;
        return;
    }
    seq_select(g_seq.loop.empty() ? -1 : g_seq.loop[0]);
    g_pattern = g_seq.cur_abs;
    seq_cycle(g_seq.loop);
}

void seq_display(uint32_t &part, uint32_t &var)
{
    part = 0;
    var = 0;
    if (!g_seq.active) return;
    if (g_seq.in_once) part = (uint32_t)g_seq.once_kind;
    else if (!g_song.parts.empty()) part = (uint32_t)part_code(g_song.parts[g_seq.loop_part]);
    var = (uint32_t)(g_seq.var[g_seq.loop_part] + 1);
}

/* --- Demandes (boutons, listes) --- */
std::string seq_fill()
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    if (g_seq.switch_at >= 0) return "déjà une bascule prévue";
    const int f = fill_for_part();
    if (f < 0) return "pas de fill dans cette chanson";
    double at, bpb;
    if (!next_bar(at, bpb)) return "à l'arrêt : pas de fill";
    const double len = size_beats(f);
    if (len < bpb) at += bpb - len;                   // un fill court finit avec la mesure
    g_seq.once.assign(1, f);
    g_seq.once_kind = 8;
    g_seq.switch_at = at;
    return "ok fill " + std::to_string(f - g_off + 1);
}

std::string seq_change(int part, int var, bool transition)
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    if (part < 0 || part >= (int)g_song.parts.size()) return "erreur : partie inconnue";
    const auto &vs = g_song.vars[g_song.parts[part]];
    if (var < -1) var = -1;
    if (var >= (int)vs.size()) var = (int)vs.size() - 1;
    if (var < 0 && g_seq.var[part] >= 0 && next_on(part, +1) < 0)
        return "erreur : il faut au moins une partie";
    const int old_part = g_seq.loop_part;
    const bool same = part == g_seq.loop_part && var == g_seq.var[part] && g_seq.switch_at < 0;
    g_seq.part = part;
    g_seq.var[part] = var;
    memo_save(part);
    if (var < 0) {                                     // None : partie sautée, la musique ne change pas
        if (g_seq.switch_at >= 0 && g_seq.next_part == part) {
            g_seq.switch_at = -1;
            g_seq.next_loop.clear();
            g_seq.once.clear();
        }
        return "ok " + g_song.parts[part] + " : None (sautée par Part ◀ ▶)";
    }
    const std::vector<int> ph = phrase_of(part);
    double at, bpb;
    if (!next_bar(at, bpb)) {                          // à l'arrêt : tout de suite
        g_seq.loop = ph;
        g_seq.loop_part = part;
        g_seq.loop_pos = 0;
        seq_select(ph.empty() ? -1 : ph[0]);
        g_pattern = g_seq.cur_abs;
        seq_cycle(ph);
        return "ok " + g_song.parts[part] + " " + var_name(var);
    }
    if (same) return "déjà là";
    g_seq.next_loop = ph;
    g_seq.next_part = part;
    g_seq.once.clear();
    if (transition) {                                  // transition, sinon un fill de la partie quittée
        int f = -1;
        if (!g_song.transition.empty()) {
            size_t &turn = g_fill_turn[g_song_rel + "/transition"];
            f = g_song.transition[turn++ % g_song.transition.size()];
        } else {
            f = fill_for_part(old_part);   // (old_part : la partie qui joue)
        }
        if (f >= 0) {
            g_seq.once.push_back(f);
            g_seq.once_kind = 8;
            if (size_beats(f) < bpb) at += bpb - size_beats(f);   // un fill court finit avec la mesure
        }
    }
    g_seq.switch_at = at;
    return "ok vers " + g_song.parts[part] + " " + var_name(var);
}

/* Fin d'une chanson sans outro : un fill de la partie qui joue, puis le coup
 * final (grosse caisse + crash sur le 1). false : rien pour la faire. */
bool ending_fill_hit(double &at, double bpb)
{
    if (g_final_abs < 0) return false;
    g_seq.once.clear();
    /* Liste Outro : A = 1er fill de la partie, B = 2e… (toujours la même fin). */
    std::string used;
    const std::vector<int> *fl = fill_list(g_seq.loop_part, used);
    const int f = fl ? (*fl)[(size_t)std::max(g_outro_choice - 1, 0) % fl->size()] : -1;
    if (f >= 0) {
        g_seq.once.push_back(f);
        if (size_beats(f) < bpb) at += bpb - size_beats(f);   // un fill court finit avec la mesure
    }
    g_seq.once.push_back(g_final_abs);
    g_seq.once_kind = 7;
    return true;
}

std::string seq_outro()
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    double at, bpb;
    if (!next_bar(at, bpb)) {                          // à l'arrêt : écouter l'outro choisie, batterie seule
        if (g_song.outro.empty() && g_final_abs < 0) return "pas d'outro dans cette chanson";
        g_seq.outro_only = true;
        const std::string r = play_from_start(true);
        g_seq.outro_only = false;
        if (r.rfind("ok", 0) != 0) return r;
        return g_song.outro.empty() ? "ok fin " + var_name(g_outro_choice - 1) + " : fill + coup final (batterie)"
                                    : "ok outro " + var_name(g_outro_choice - 1) + " seule (batterie)";
    }
    g_seq.once.clear();
    if (!g_song.outro.empty()) {
        const int o = std::min(std::max(g_outro_choice - 1, 0), (int)g_song.outro.size() - 1);
        for (int a : g_song.outro[o]) g_seq.once.push_back(a);
        g_seq.once_kind = 7;
    }
    bool hit = false;
    if (g_song.outro.empty()) hit = ending_fill_hit(at, bpb);
    g_seq.stop_after = true;
    g_seq.switch_at = at;
    return !g_song.outro.empty() ? "ok outro" : hit ? "ok fin " + var_name(g_outro_choice - 1) + " : fill + coup final"
                                                 : "ok fin (arrêt à la barre)";
}

/* Bouton Intro. En jouant : l'intro choisie à la mesure suivante, puis la
 * partie reprend (comme après un fill). À l'arrêt : l'intro seule (ou la
 * mesure de décompte), batterie seule, pour la choisir. */
std::string seq_intro()
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    const int ic = g_intro_choice;
    if (ic <= 1) return "Intro : Off";
    if (ic >= 3 && g_song.intro.empty()) return "pas d'intro dans cette chanson";
    double at, bpb;
    if (!next_bar(at, bpb)) {
        g_seq.intro_only = true;
        const std::string r = play_from_start(true);
        g_seq.intro_only = false;
        return r.rfind("ok", 0) == 0 ? std::string(ic == 2 ? "ok décompte" : "ok intro seule (batterie)") : r;
    }
    if (ic == 2) return "décompte : seulement au Play";
    if (g_seq.switch_at >= 0) return "déjà une bascule prévue";
    g_seq.once.clear();
    for (int a : g_song.intro[std::min(ic - 3, (int)g_song.intro.size() - 1)]) g_seq.once.push_back(a);
    g_seq.once_kind = 1;
    g_seq.switch_at = at;
    return "ok intro";
}

/* Au Play (transport à la mesure 1) : intro ou décompte, puis la partie. */
void seq_start()
{
    if (!g_seq.active) return;
    g_seq.once.clear();
    g_seq.switch_at = -1;
    g_seq.stop_after = false;
    g_seq.resume_pos = 0;
    if (g_seq.var[g_seq.part] < 0) {                  // partie affichée sautée : la suivante
        const int j = next_on(g_seq.part, +1);
        if (j >= 0) g_seq.part = j;
    }
    g_seq.loop_part = g_seq.part;
    g_seq.loop = phrase_of(g_seq.part);
    g_seq.loop_pos = 0;
    const int ic = g_intro_choice;
    if (g_seq.outro_only && !g_song.outro.empty()) {   // End à l'arrêt : l'outro seule, puis arrêt
        const int o = std::min(std::max(g_outro_choice - 1, 0), (int)g_song.outro.size() - 1);
        for (int a : g_song.outro[o]) g_seq.once.push_back(a);
        g_seq.once_kind = 7;
        g_seq.stop_after = true;
    } else if (g_seq.outro_only) {                     // pas d'outro : fill + coup final
        double at0 = 0;
        ending_fill_hit(at0, 4.0);                     // à la mesure 1 : le fill part au début
        g_seq.stop_after = true;
    } else if (ic == 2 && g_silence_abs >= 0) {        // décompte : 1 mesure de clic
        g_seq.once.push_back(g_silence_abs);
        g_seq.once_kind = 1;
        if (g_h2_metro != 1) { set_metronome(true); g_seq.metro_after = true; }
    } else if (ic >= 3 && !g_song.intro.empty()) {
        for (int a : g_song.intro[std::min(ic - 3, (int)g_song.intro.size() - 1)]) g_seq.once.push_back(a);
        g_seq.once_kind = 1;
    }
    if (g_seq.intro_only) g_seq.stop_after = true;  // Intro à l'arrêt : l'intro seule, puis arrêt
    int first;
    if (!g_seq.once.empty()) { first = g_seq.once.front(); g_seq.once.pop_front(); g_seq.in_once = true; }
    else { first = g_seq.loop.empty() ? -1 : g_seq.loop[0]; g_seq.in_once = false; }
    seq_select(first);
    g_seq.item_start = 0;
    g_seq.item_len = size_beats(first);
    seq_cycle(g_seq.loop);
}

void seq_stopped()
{
    if (!g_seq.active) return;
    if (g_seq.metro_after) { set_metronome(false); g_seq.metro_after = false; }
    g_seq.once.clear();
    g_seq.switch_at = -1;
    g_seq.in_once = g_seq.stop_after = false;
    g_seq.resume_pos = 0;
    g_seq.next_loop.clear();
    if (!phrase_of(g_seq.part).empty()) {             // partie affichée (sinon : celle qui jouait)
        g_seq.loop = phrase_of(g_seq.part);
        g_seq.loop_part = g_seq.part;
    }
    g_seq.loop_pos = 0;
    seq_select(g_seq.loop.empty() ? -1 : g_seq.loop[0]);
}

/* Appelé à chaque passage (~5 ms). */
void seq_tick()
{
    if (!g_seq.active) return;
    double now_b, bpb, beat_s;
    if (!transport_beats(now_b, bpb, beat_s)) return;
    const double early = 0.010 / beat_s;
    double t;
    const bool asked = g_seq.switch_at >= 0;
    if (asked) {
        if (now_b < g_seq.switch_at - early) return;
        t = g_seq.switch_at;
        g_seq.switch_at = -1;
    } else {
        t = g_seq.item_start + g_seq.item_len;
        if (now_b < t - early) return;
    }
    int abs;
    if (!g_seq.once.empty()) {
        abs = g_seq.once.front();
        g_seq.once.pop_front();
        if (!g_seq.in_once && !g_seq.loop.empty()) {   // fill qui coupe la phrase : reprise au même
            const double item_end = g_seq.item_start + g_seq.item_len;   // élément, ou au suivant s'il le finit
            g_seq.resume_pos = t + size_beats(abs) >= item_end - 0.01 ? (g_seq.loop_pos + 1) % g_seq.loop.size()
                                                                       : g_seq.loop_pos;
        }
        g_seq.in_once = true;
    } else if (g_seq.stop_after) {                     // fin jouée (ou pas d'outro) : tout s'arrête
        g_seq.stop_after = false;
        say("DLS Drum : %s", stop_all().c_str());
        g_refresh_now = true;
        return;
    } else {
        if (g_seq.in_once && g_seq.metro_after) { set_metronome(false); g_seq.metro_after = false; }
        const bool was_once = g_seq.in_once;
        g_seq.in_once = false;
        if (!g_seq.next_loop.empty()) {               // autre partie ou variation
            g_seq.loop = g_seq.next_loop;
            g_seq.loop_part = g_seq.next_part;
            g_seq.next_loop.clear();
            g_seq.loop_pos = 0;
            seq_cycle(g_seq.loop);
        } else if (was_once) {                        // après intro, décompte ou fill
            g_seq.loop_pos = g_seq.loop.empty() ? 0 : g_seq.resume_pos % g_seq.loop.size();
        } else if (!g_seq.loop.empty()) {             // phrase : élément suivant
            g_seq.loop_pos = (g_seq.loop_pos + 1) % g_seq.loop.size();
        }
        g_seq.resume_pos = 0;
        if (g_seq.loop.empty()) return;
        abs = g_seq.loop[g_seq.loop_pos];
    }
    if (abs != g_seq.cur_abs) {                       // même pattern : Hydrogen le boucle lui-même
        seq_select(abs);
        say("DLS Drum : pattern %d (mesure %d temps %.2f%s)", abs - g_off + 1, (int)(now_b / bpb) + 1,
            std::fmod(now_b, bpb) + 1, g_seq.in_once ? ", une fois" : "");
    }
    g_pattern = abs;
    g_seq.item_start = t;
    g_seq.item_len = size_beats(abs);
    g_refresh_now = true;
}

/* Intro ▶ : intro suivante de la liste (Metronome, A, B…, sans Off), montrée
 * dans PiPedal, puis jouée comme par le bouton Intro. Outro ▶ : pareil avec
 * l'outro (en jouant, la chanson finit avec elle). */
std::string seq_intro_next()
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    const int last = 2 + (int)g_song.intro.size();     // 2 Metronome, 3.. A…
    g_intro_choice = g_intro_choice < 2 || g_intro_choice >= last ? 2 : g_intro_choice + 1;
    g_ui_intro = g_intro_choice;
    memo_choice("Intro", g_intro_choice);
    const std::string name = g_intro_choice == 2 ? "Metronome" : var_name(g_intro_choice - 3);
    return "Intro " + name + " : " + seq_intro();
}

std::string seq_outro_next()
{
    if (!g_seq.active) return "erreur : pas de structure pour cette chanson";
    if (g_song.outro.empty()) {                        // pas d'outro : les fins fill + coup final
        std::string used;
        const std::vector<int> *fl = fill_list(g_seq.loop_part, used);
        const int n = fl ? std::min((int)fl->size(), 26) : 0;   // liste Outro : A à Z
        if (n > 0) {
            g_outro_choice = g_outro_choice < 1 || g_outro_choice >= n ? 1 : g_outro_choice + 1;
            g_ui_outro = g_outro_choice;
            memo_choice("Outro", g_outro_choice);
        }
        return seq_outro();
    }
    g_outro_choice = g_outro_choice < 1 || g_outro_choice >= (int)g_song.outro.size() ? 1 : g_outro_choice + 1;
    g_ui_outro = g_outro_choice;
    memo_choice("Outro", g_outro_choice);
    return "Outro " + var_name(g_outro_choice - 1) + " : " + seq_outro();
}

/* --- Browse : Style / Type / Beat au pied ---------------------------------
 * Les chansons du gros morceau (« Style/Type/Beat.h2song », ordre du sélecteur).
 * level 0 Style, 1 Type, 2 Beat ; dir -1 / +1, en revenant au début. Chaque
 * type retient son dernier beat, chaque style son dernier type. */

std::string path_part(const std::string &rel, int level)  // "Style", "Style/Type"
{
    size_t p = rel.find('/');
    if (level == 0 || p == std::string::npos) return rel.substr(0, p);
    const size_t q = rel.find('/', p + 1);
    return q == std::string::npos ? rel.substr(0, p) : rel.substr(0, q);
}

/* Liste Style/Type des sorties Style et Type : celle du plugin INSTALLÉ
 * (browse.txt de son paquet), pour ne jamais montrer un mauvais nom ; un
 * dossier qu'elle n'a pas encore s'affiche « - ». */
const char *PB_INSTALLED = "/usr/local/lib/lv2/pedal-bridge.lv2";

std::vector<std::string> read_types(const std::string &file)
{
    std::vector<std::string> v;
    std::ifstream f(file);
    std::string l;
    while (std::getline(f, l)) if (l.find('/') != std::string::npos) v.push_back(l);
    return v;
}

/* Dossiers Style/Type du dossier des chansons, triés comme le pont (octets). */
std::vector<std::string> folder_types(const std::string &songs)
{
    std::vector<std::string> v;
    DIR *d = opendir(songs.c_str());
    if (!d) return v;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string st = e->d_name, sp = songs + "/" + st;
        struct stat sb;
        if (stat(sp.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        DIR *d2 = opendir(sp.c_str());
        if (!d2) continue;
        while (dirent *e2 = readdir(d2)) {
            if (e2->d_name[0] == '.') continue;
            const std::string tp = sp + "/" + e2->d_name;
            if (stat(tp.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) v.push_back(st + "/" + e2->d_name);
        }
        closedir(d2);
    }
    closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

/* Au premier choix de chanson après le démarrage : si les dossiers Style/Type
 * ne sont plus ceux du plugin installé, refaire drum.ttl et browse.txt dans le
 * paquet compilé et dire comment l'installer (sudo : à la main). */
void browse_check(const std::string &songs)
{
    static bool done = false;
    if (done || songs.empty()) return;
    done = true;
    const std::vector<std::string> now = folder_types(songs);
    if (now.empty()) return;
    const std::vector<std::string> inst = read_types(std::string(PB_INSTALLED) + "/browse.txt");
    if (now == inst) return;
    std::string plus, minus;
    for (const auto &t : now) if (std::find(inst.begin(), inst.end(), t) == inst.end()) plus += " " + t + ",";
    for (const auto &t : inst) if (std::find(now.begin(), now.end(), t) == now.end()) minus += " " + t + ",";
    const std::string bundle = home_dir() + "/pedal-bridge-build/build/lv2/pedal-bridge.lv2";
    if (read_types(bundle + "/browse.txt") != now) {
        {
            std::ofstream f(bundle + "/browse.txt.tmp");
            for (const auto &t : now) f << t << '\n';
        }
        std::rename((bundle + "/browse.txt.tmp").c_str(), (bundle + "/browse.txt").c_str());
        const std::string cmd = "python3 " + shell_quote(scripts_dir() + "/gen_dls_ttl.py") + " --types " +
                                shell_quote(bundle + "/browse.txt") + " --out " + shell_quote(bundle) + " 2>&1";
        const int rc = std::system(cmd.c_str());
        if (rc != 0) { say("DLS Drum : Style/Type changés, mais drum.ttl n'a pas pu être refait (%d)", rc); return; }
    }
    say("DLS Drum : dossiers Style/Type changés (nouveaux :%s retirés :%s) ; drum.ttl refait dans %s. "
        "Pour les voir dans PiPedal : sudo install -m 644 '%s/drum.ttl' '%s/browse.txt' %s/ "
        "puis sudo systemctl restart pipedald (coupe le son)",
        plus.empty() ? " aucun" : plus.c_str(), minus.empty() ? " aucun" : minus.c_str(), bundle.c_str(),
        bundle.c_str(), bundle.c_str(), PB_INSTALLED);
}

void browse_display(uint32_t &style, uint32_t &type)
{
    static std::vector<std::string> types;
    static bool read = false;
    if (!read) {
        read = true;
        types = read_types(std::string(PB_INSTALLED) + "/browse.txt");
        if (types.empty()) types = read_types(scripts_dir() + "/browse.txt");   // ancienne installation
    }
    style = type = 0;
    if (g_song_rel.empty()) return;
    const std::string st = path_part(g_song_rel, 0), ty = path_part(g_song_rel, 1);
    std::vector<std::string> styles;
    for (size_t i = 0; i < types.size(); i++) {
        const std::string s = types[i].substr(0, types[i].find('/'));
        if (std::find(styles.begin(), styles.end(), s) == styles.end()) styles.push_back(s);
        if (types[i] == ty) type = (uint32_t)i + 1;
    }
    for (size_t i = 0; i < styles.size(); i++) if (styles[i] == st) style = (uint32_t)i + 1;
}

void browse_remember(const std::string &rel)
{
    memo_load();
    const std::string ty = path_part(rel, 1), st = path_part(rel, 0);
    if (g_last_beat[ty] == rel && g_last_type[st] == ty) return;
    g_last_beat[ty] = rel;
    g_last_type[st] = ty;
    memo_write();
}

std::string browse(pb_ctl *k, int level, int dir)
{
    if (g_index.empty() || g_song_rel.empty()) return "pas de liste de chansons (kit et chanson à choisir)";
    std::vector<std::string> songs;
    for (const auto &kv : g_index) songs.push_back(kv.first);
    browse_remember(g_song_rel);
    const std::string cur = g_song_rel;
    std::string target;
    if (level == 2) {                                  // beats du même type
        std::vector<std::string> same;
        for (const auto &r : songs) if (path_part(r, 1) == path_part(cur, 1)) same.push_back(r);
        const auto it = std::find(same.begin(), same.end(), cur);
        const int n = (int)same.size(), i = it == same.end() ? 0 : (int)(it - same.begin());
        target = same[((i + dir) % n + n) % n];
    } else {
        std::vector<std::string> keys;                 // styles, ou types du style
        for (const auto &r : songs) {
            if (level == 1 && path_part(r, 0) != path_part(cur, 0)) continue;
            const std::string key = path_part(r, level);
            if (keys.empty() || keys.back() != key) keys.push_back(key);
        }
        const auto it = std::find(keys.begin(), keys.end(), path_part(cur, level));
        const int n = (int)keys.size(), i = it == keys.end() ? 0 : (int)(it - keys.begin());
        std::string key = keys[((i + dir) % n + n) % n];
        if (level == 0) {                              // style -> son dernier type
            const auto t = g_last_type.find(key);
            if (t != g_last_type.end()) key = t->second;
        }
        const auto b = g_last_beat.find(key);          // type -> son dernier beat, sinon le premier
        if (b != g_last_beat.end()) target = b->second;
        else for (const auto &r : songs) if (r.compare(0, key.size() + 1, key + "/") == 0 || r == key) { target = r; break; }
    }
    if (target.empty() || target == cur) return "rien d'autre ici";
    const std::string dir_ = songs_dir_of(g_dr_song_sel);
    if (dir_.empty()) return "erreur : dossier des chansons inconnu";
    g_dr_song_sel = dir_ + "/" + target;
    g_dr_song_groove = 1;
    if (!apply_big_choice(k)) return "erreur : changement impossible maintenant";
    browse_remember(target);
    g_ui_song = g_dr_song_sel;                         // le sélecteur Song de PiPedal suit
    return "ok " + target.substr(0, target.size() - (target.size() > 7 && target.compare(target.size() - 7, 7, ".h2song") == 0 ? 7 : 0));
}

void drum_poll(pb_ctl *k)
{
    const uint64_t owner = PB_LOAD_ACQ(&k->drum_owner);
    if (owner != g_dr_owner) {             // autre instance : se recaler sans rien exécuter
        g_dr_owner = owner;
        for (int b = 0; b < PB_DBTN_COUNT; b++) g_dr_btn[b] = PB_LOAD_ACQ(pb_drum_btn(k, b));
        g_dr_tempo = g_dr_tempo_seen = PB_LOAD_RLX(&k->drum_tempo);
        g_dr_groove = 0;                   // groove du preset : choisi au prochain passage (sans démarrer)
        g_taps.clear();
        g_dr_metro = 2;                    // interrupteur = état voulu : appliqué au prochain passage
        g_dr_song_seq = 1;                 // kit et morceau du preset : relus (ouverts s'ils changent)
        g_dr_kit_num = 0xffffffffu;
        g_dr_song_preset = true;
        say("%s", owner ? "DLS Drum actif" : "DLS Drum retiré");
        return;
    }
    if (!owner) return;
    drum_song_poll(k);
    const uint32_t groove = PB_LOAD_RLX(&k->drum_groove);
    if (g_seq.active) {
        const uint32_t pu = PB_LOAD_RLX(&k->drum_part), vu = PB_LOAD_RLX(&k->drum_var);
        const uint32_t iu = PB_LOAD_RLX(&k->drum_intro), ou = PB_LOAD_RLX(&k->drum_outro);
        /* Juste après un changement de chanson, un Intro / Outro du preset ne
         * remplace pas celui retenu pour la chanson : on le remontre. */
        const bool fresh = now_s() - g_song_set_t < 1.5;
        const auto mem = g_memo.find(g_song_rel);
        if (iu != g_dr_intro_ui) {
            g_dr_intro_ui = iu;
            if (fresh && mem != g_memo.end() && mem->second.count("Intro")) {
                if ((int)iu != g_intro_choice) g_ui_intro = g_intro_choice;
            } else {
                g_intro_choice = (int)iu;
                if (!fresh) memo_choice("Intro", g_intro_choice);
            }
        }
        if (ou != g_dr_outro_ui) {
            g_dr_outro_ui = ou;
            if (fresh && mem != g_memo.end() && mem->second.count("Outro")) {
                if ((int)ou != g_outro_choice) g_ui_outro = g_outro_choice;
            } else {
                g_outro_choice = (int)ou;
                if (!fresh) memo_choice("Outro", g_outro_choice);
            }
        }
        if (pu != g_dr_part_ui || vu != g_dr_var_ui) {
            const bool pchg = pu != g_dr_part_ui;
            g_dr_part_ui = pu;
            g_dr_var_ui = vu;
            if (!take_echo(pchg ? "part" : "variation", (int)(pchg ? pu : vu))) {
                const int np = part_from_ui(pu);
                say("DLS Drum : %s", seq_change(np, pchg && np != g_seq.part ? g_seq.var[np] : (int)vu - 1,
                                                pchg && !PB_LOAD_RLX(&k->drum_nofill)).c_str());
            }
        }
        g_dr_groove = groove;
    } else if (groove != g_dr_groove) {
        g_dr_groove = groove;
        if (!take_echo("groove", (int)groove) && (int)groove != groove_rel()) {   // sinon : écho, ou déjà joué
            say("DLS Drum : %s", set_groove(std::to_string(groove)).c_str());
            g_refresh_now = true;
        }
    }
    const uint32_t metro = PB_LOAD_RLX(&k->drum_metro);
    if (metro != g_dr_metro) {
        g_dr_metro = metro;
        say("DLS Drum : %s", set_metronome(metro != 0).c_str());
    }
    const uint32_t tempo = PB_LOAD_RLX(&k->drum_tempo);
    if (tempo != g_dr_tempo_seen) { g_dr_tempo_seen = tempo; g_dr_tempo_since = now_s(); }
    if (tempo != g_dr_tempo && now_s() - g_dr_tempo_since >= 0.3) {
        g_dr_tempo = tempo;
        if (!take_echo("drum-tempo", (int)tempo) && tempo != g_last_tempo_c) {   // sinon : écho, ou déjà le tempo
            char t[32];
            std::snprintf(t, sizeof t, "%.2f", tempo * 0.01);
            say("DLS Drum : tempo %s -> %s", t, set_tempo(t).c_str());
            g_ui_tempo_skip = URI_DRUM;              // il vient de ce bouton (on le tourne peut-être encore)
        }
    }
    for (int b = 0; b < PB_DBTN_COUNT; b++) {
        const uint32_t c = PB_LOAD_ACQ(pb_drum_btn(k, b));
        uint32_t d = c - g_dr_btn[b];
        g_dr_btn[b] = c;
        if (b == PB_DBTN_BPM_UP || b == PB_DBTN_BPM_DOWN) {   // pas répétés quand on tient
            if (d > 100) d = 100;                             // garde-fou
            g_bpm_delta += b == PB_DBTN_BPM_UP ? (int)d : -(int)d;
            if (d) g_bpm_step_t = now_s();
            continue;
        }
        if (d > 4) d = 4;                  // garde-fou
        for (uint32_t j = 0; j < d; j++) {
            std::string r;
            switch (b) {
            case PB_DBTN_PLAYSTOP: r = transport_rolling() ? stop_all() : play_from_start(); break;
            case PB_DBTN_PREV:     r = g_seq.active ? seq_change(g_seq.part, g_seq.var[g_seq.part] - 1, false) : step_groove(-1); break;
            case PB_DBTN_NEXT:     r = g_seq.active ? seq_change(g_seq.part, g_seq.var[g_seq.part] + 1, false) : step_groove(+1); break;
            case PB_DBTN_TAP:      r = tap_tempo(); break;
            case PB_DBTN_FILL:     r = seq_fill(); break;
            case PB_DBTN_PART_PREV:
            case PB_DBTN_PART_NEXT: {                  // parties actives (pas None), en revenant au début
                const int j = g_seq.active ? next_on(g_seq.part, b == PB_DBTN_PART_NEXT ? +1 : -1) : -1;
                r = j >= 0 ? seq_change(j, g_seq.var[j], !PB_LOAD_RLX(&k->drum_nofill)) : "pas d'autre partie";
                break;
            }
            case PB_DBTN_OUTRO:    r = seq_outro(); break;
            case PB_DBTN_INTRO:    r = seq_intro(); break;
            case PB_DBTN_INTRO_NEXT: r = seq_intro_next(); break;
            case PB_DBTN_OUTRO_NEXT: r = seq_outro_next(); break;
            case PB_DBTN_STYLE_PREV: r = browse(k, 0, -1); break;
            case PB_DBTN_STYLE_NEXT: r = browse(k, 0, +1); break;
            case PB_DBTN_TYPE_PREV:  r = browse(k, 1, -1); break;
            case PB_DBTN_TYPE_NEXT:  r = browse(k, 1, +1); break;
            case PB_DBTN_BEAT_PREV:  r = browse(k, 2, -1); break;
            case PB_DBTN_BEAT_NEXT:  r = browse(k, 2, +1); break;
            }
            say("DLS Drum : %s", r.c_str());
            g_refresh_now = true;
        }
    }
    bpm_steps(k);
    tap_finish();
    seq_tick();
    /* Listes Part et Variation de PiPedal : suivre le séquenceur (websocket). */
    if (g_seq.active) {
        const uint32_t pu = g_song.parts.empty() ? 0u : [&]() -> uint32_t {
            for (uint32_t i = 0; i < 4; i++) if (g_song.parts[g_seq.part] == PART_UI[i]) return i + 1;
            return g_dr_part_ui;                       // « Groove » : la liste ne bouge pas
        }();
        const uint32_t vu = (uint32_t)g_seq.var[g_seq.part] + 1;
        if (pu != g_dr_part_ui || vu != g_dr_var_ui) { g_ui_part = (int)pu; g_ui_var = (int)vu; }
    }
}

/* Morceau courant d'Hydrogen au démarrage (lastSongFilename de son fichier
 * de réglages) : taille de ses patterns, pour régler le cycle de SooperLooper
 * dès le premier changement de groove. */
/* Morceau ouvert par le pont (batterie, load) : retenu dans un fichier, car
 * Hydrogen n'écrit son lastSongFilename qu'en quittant ; après un redémarrage
 * du pont, c'est ce fichier qui dit quel morceau joue vraiment. */
std::string drum_song_file() { return work_dir() + "/morceau-batterie.txt"; }

void remember_drum_song(const std::string &path)
{
    mkdirs(work_dir());
    std::ofstream(drum_song_file(), std::ios::trunc) << path << '\n';
}

void read_h2_current_song()
{
    {
        std::ifstream m(drum_song_file());
        std::string path;
        /* Valable seulement si Hydrogen tournait déjà quand le pont l'a ouvert :
         * un Hydrogen relancé depuis (redémarrage du Pi, de la session) a un
         * autre morceau, il faut le rouvrir. Date de démarrage d'un processus =
         * date de son dossier /proc/<pid>. */
        if (std::getline(m, path) && file_size(path) > 0) {
            read_pattern_sizes(path);
            long h2_start = -1;
            if (DIR *d = opendir("/proc")) {
                while (dirent *e = readdir(d)) {
                    if (!std::isdigit((unsigned char)e->d_name[0])) continue;
                    std::ifstream c(std::string("/proc/") + e->d_name + "/comm");
                    std::string comm;
                    std::getline(c, comm);
                    if (comm == "hydrogen" || comm == "h2cli")
                        h2_start = std::max(h2_start, mtime(std::string("/proc/") + e->d_name));
                }
                closedir(d);
            }
            if (h2_start >= 0 && h2_start <= mtime(drum_song_file())) g_drum_song = path;
            else say("Hydrogen relancé depuis le dernier morceau ouvert : il sera rouvert");
            return;
        }
    }
    const char *home = std::getenv("HOME");
    if (!home) return;
    std::ifstream f(std::string(home) + "/.hydrogen/hydrogen.conf");
    std::string conf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string tag = "<lastSongFilename>";
    const size_t a = conf.find(tag), b = conf.find("</lastSongFilename>");
    if (a == std::string::npos || b == std::string::npos || b < a) return;
    const std::string path = conf.substr(a + tag.size(), b - a - tag.size());
    if (file_size(path) > 0) read_pattern_sizes(path);
}

/* Lit les commandes de DLS Looper et DLS Drum ; appelé ~200 fois par seconde. */
void looper_poll()
{
    pb_ctl *k = g_ctl.load(std::memory_order_acquire);
    if (!k) {
        if (now_s() < g_ctl_next_try) return;
        g_ctl_next_try = now_s() + 1.0;
        char err[256];
        if (pb_ctl_open(&k, 0, err, sizeof err) != 0) return;   // pas encore créé par un plugin
        g_ctl_me = ((uint64_t)getpid() << 32) | 1u;
        g_ctl.store(k, std::memory_order_release);
        say("fichier de contrôle ouvert (DLS Looper / DLS Drum)");
        publish_states();
    }
    drum_poll(k);
    ui_flush();
    const uint64_t owner = PB_LOAD_ACQ(&k->looper_owner);
    if (owner != g_lp_owner) {             // autre instance : se recaler sans rien exécuter
        g_lp_owner = owner;
        for (int b = 0; b < PB_BTN_COUNT; b++) g_lp_btn[b] = PB_LOAD_ACQ(&k->looper_btn[b]);
        g_lp_tempo = g_tempo_seen = PB_LOAD_RLX(&k->looper_tempo);
        g_lp_loop = PB_LOAD_RLX(&k->looper_loop);
        if (owner) { select_loop(g_lp_loop); say("DLS Looper actif (boucle %u)", g_lp_loop); }
        else say("DLS Looper retiré");
        return;
    }
    if (!owner) return;
    const uint32_t loop = PB_LOAD_RLX(&k->looper_loop);
    if (loop != g_lp_loop) {
        g_lp_loop = loop;
        select_loop(loop);
        say("DLS Looper : boucle %u", loop);
    }
    /* Tempo : appliqué quand le bouton ne bouge plus depuis 0,3 s (tourner le
     * bouton envoie une valeur par cran ; Hydrogen déborde sinon). */
    const uint32_t tempo = PB_LOAD_RLX(&k->looper_tempo);
    if (tempo != g_tempo_seen) { g_tempo_seen = tempo; g_tempo_since = now_s(); }
    if (tempo != g_lp_tempo && now_s() - g_tempo_since >= 0.3) {
        g_lp_tempo = tempo;
        if (!take_echo("looper-tempo", (int)tempo) && tempo != g_last_tempo_c) {
            char t[32];
            std::snprintf(t, sizeof t, "%.2f", tempo * 0.01);
            say("DLS Looper : tempo %s -> %s", t, set_tempo(t).c_str());
            g_ui_tempo_skip = URI_LOOPER;
        }
    }
    for (int b = 0; b < PB_BTN_COUNT; b++) {
        const uint32_t c = PB_LOAD_ACQ(&k->looper_btn[b]);
        uint32_t d = c - g_lp_btn[b];
        g_lp_btn[b] = c;
        if (d > 4) d = 4;                  // garde-fou
        for (uint32_t j = 0; j < d; j++) {
            say("DLS Looper : %s", looper_button(b).c_str());
            g_refresh_now = true;
        }
    }
}

bool valid_name(const std::string &s)
{
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    return true;
}

std::string save_song(const std::string &name)
{
    if (!valid_name(name)) return "erreur : nom invalide (lettres, chiffres, - et _ seulement)";
    std::string r = stop_all();
    if (r.rfind("ok", 0) != 0) return r;
    const std::string dir = songs_dir() + "/" + name;
    if (!mkdirs(dir)) return "erreur : impossible de créer " + dir;
    const uint32_t D = g_cfg.comp_frames ? g_cfg.comp_frames() : 0;
    std::ofstream info(dir + "/chanson.txt", std::ios::trunc);
    const double tempo = get_tempo();
    info << "tempo=" << tempo << "\n" << "compensation=" << D << "\n" << "frequence=" << g_sr << "\n";
    int n = 0;
    for (size_t i = 0; i < g_loops.size(); i++) {
        const std::string f = dir + "/boucle" + std::to_string(i + 1) + ".wav";
        if (g_loops[i].len <= 0) { unlink(f.c_str()); continue; }
        std::string err;
        if (!save_loop_to((int)i, f, err)) return "erreur : " + err;
        info << "boucle" << (i + 1) << "=" << g_loops[i].len << "\n";
        n++;
    }
    /* Morceau Hydrogen (patterns, kit) + mode + pattern retenu. */
    std::string h2 = "morceau Hydrogen non sauvé (OSC d'Hydrogen absent)";
    if (h2_listening()) {
        const std::string f = dir + "/" + name + ".h2song";
        unlink(f.c_str());
        h2_string("/Hydrogen/SAVE_SONG_AS", f);
        if (wait_file(f, 1000, 10.0)) {
            info << "hydrogen=" << name << ".h2song\n" << "mode=pattern\n" << "pattern=" << g_pattern << "\n";
            read_pattern_sizes(f);
            h2 = "morceau Hydrogen sauvé" + (g_pattern >= 0 ? ", groove " + std::to_string(g_pattern + 1)
                                                             : std::string(", groove inconnu (choisis-le avec groove N)"));
        } else h2 = "erreur : Hydrogen n'a pas écrit " + f;
    }
    say("chanson « %s » sauvegardée : %d boucle(s), tempo %.2f ; %s", name.c_str(), n, tempo, h2.c_str());
    return "ok sauvegardé « " + name + " » : " + std::to_string(n) + " boucle(s), tempo " +
           std::to_string((int)std::lround(tempo)) + " ; " + h2;
}

std::string load_song(const std::string &name)
{
    if (!valid_name(name)) return "erreur : nom invalide";
    const std::string dir = songs_dir() + "/" + name;
    std::ifstream info(dir + "/chanson.txt");
    if (!info) return "erreur : chanson « " + name + " » introuvable";
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(info, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    sl_refresh();
    if (!g_sl_ok) return "erreur : SooperLooper ne répond pas (lancé ? port OSC " + std::to_string(g_cfg.sl_port) + ")";
    if (sl_any_recording()) return "erreur : un enregistrement est en cours";
    transport_stop_and_pause();
    const uint32_t Dsaved = (uint32_t)std::atol(kv["compensation"].c_str());
    int n = 0;
    for (size_t i = 0; i < g_loops.size(); i++) {
        const std::string f = dir + "/boucle" + std::to_string(i + 1) + ".wav";
        Loop &L = g_loops[i];
        if (file_size(f) <= 0) {                     // pas dans la chanson : vider
            if (L.len > 0) sl_send("/sl/" + std::to_string(i) + "/hit", {"undo_all"});
            L = Loop{};
            continue;
        }
        /* Les boucles sauvées sont normalisées : aucun décalage au chargement,
         * même si la latence a changé depuis (voir normalize()). */
        std::string err;
        if (!load_loop_from((int)i, f, err)) return "erreur : " + err;
        sl_send("/sl/" + std::to_string(i) + "/hit", {"pause_on"});
        L.normalized = true;
        L.dirty = false;
        L.norm_d = Dsaved;
        n++;
    }
    save_state();
    const double tempo = std::atof(kv["tempo"].c_str());
    if (tempo >= 20) timebase(std::to_string(tempo));
    /* Morceau Hydrogen : ouvrir, mode Pattern, pattern retenu, affichage du tempo. */
    std::string h2 = "pas de morceau Hydrogen dans cette chanson";
    if (!kv["hydrogen"].empty()) {
        const std::string f = dir + "/" + kv["hydrogen"];
        if (!h2_listening()) h2 = "morceau Hydrogen non chargé (OSC d'Hydrogen absent)";
        else if (file_size(f) <= 0) h2 = "erreur : " + f + " introuvable";
        else {
            h2_string("/Hydrogen/OPEN_SONG", f);
            read_pattern_sizes(f);
            remember_drum_song(f);
            g_drum_song = f;
            usleep(1500000);                         // Hydrogen ne confirme pas : on lui laisse le temps
            h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
            g_off = g_cnt = 0;
            g_pattern = std::atoi(kv["pattern"].c_str());
            if (kv["pattern"].empty()) g_pattern = -1;
            if (g_pattern >= 0) h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", (float)g_pattern);
            if (tempo >= 20) h2_float("/Hydrogen/BPM", (float)tempo);
            h2 = "morceau Hydrogen chargé" + (g_pattern >= 0 ? ", groove " + std::to_string(g_pattern + 1) : std::string());
            h2 += ", " + apply_cycle(g_pattern);
        }
    }
    jack_transport_locate(g_cfg.client, 0);
    say("chanson « %s » chargée : %d boucle(s), tempo %.2f ; %s", name.c_str(), n, tempo, h2.c_str());
    return "ok chargé « " + name + " » : " + std::to_string(n) + " boucle(s), tempo " +
           std::to_string((int)std::lround(tempo)) + " ; " + h2 + ". Prêt : play.";
}

/* Ouvre un morceau Hydrogen (.h2song, chemin absolu) : mode Pattern,
 * groove 1, cycle et temps par mesure selon ce groove. À l'arrêt seulement. */
/* Temps processeur cumulé (ticks) des processus « hydrogen » (ou « h2cli », sans fenêtre). */
long hydrogen_cpu_ticks()
{
    long total = 0;
    if (DIR *d = opendir("/proc")) {
        while (dirent *e = readdir(d)) {
            if (!std::isdigit((unsigned char)e->d_name[0])) continue;
            std::ifstream f(std::string("/proc/") + e->d_name + "/stat");
            std::string st((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const size_t a = st.find('('), b = st.rfind(')');
            if (a == std::string::npos || b == std::string::npos) continue;
            const std::string comm = st.substr(a + 1, b - a - 1);
            if (comm != "hydrogen" && comm != "h2cli") continue;           // avec ou sans fenêtre
            std::istringstream rest(st.substr(b + 2));
            std::string tok;
            long utime = 0, stime = 0;
            for (int i = 3; i <= 15 && rest >> tok; i++) {          // champs 14 et 15 : utime, stime
                if (i == 14) utime = std::atol(tok.c_str());
                if (i == 15) stime = std::atol(tok.c_str());
            }
            total += utime + stime;
        }
        closedir(d);
    }
    return total;
}

/* Attend qu'Hydrogen soit calme : moins de 25 % d'un cœur sur 0,3 s. */
bool wait_hydrogen_idle(double deadline)
{
    const long hz = sysconf(_SC_CLK_TCK);
    long prev = hydrogen_cpu_ticks();
    double tprev = now_s();
    while (now_s() < deadline) {
        usleep(300000);
        const long c = hydrogen_cpu_ticks();
        const double t = now_s();
        const double load = (double)(c - prev) / hz / (t - tprev);
        prev = c;
        tprev = t;
        if (load < 0.25) return true;
    }
    return false;
}

std::string open_drum_song(const std::string &path, int groove)
{
    if (path.empty() || path[0] != '/') return "erreur : batterie CHEMIN (chemin absolu d'un .h2song)";
    if (file_size(path) <= 0) return "erreur : " + path + " introuvable";
    if (!h2_listening()) return "erreur : l'OSC d'Hydrogen ne répond pas (port " + std::to_string(g_cfg.h2_port) + ")";
    if (transport_rolling()) return "erreur : arrête la lecture d'abord (Play/Stop)";
    /* Fin du chargement : Hydrogen renvoie tout son état (volumes, mute…) à ses
     * clients quand le morceau est chargé (mesuré : 3,9 s pour un gros morceau
     * de 28 Mo). On s'inscrit d'abord (un message d'une adresse nouvelle
     * déclenche aussi cet envoi) et on vide ce qui arrive, puis on ouvre et on
     * attend le premier retour ; au pire, un délai selon la taille. */
    h2_send("/Hydrogen/PB_PROBE", nullptr, nullptr);
    sl_drain(0.2, [](const OscMsg &) { return false; });
    const double t0 = now_s();
    h2_string("/Hydrogen/OPEN_SONG", path);
    read_pattern_sizes(path);
    remember_drum_song(path);
    g_drum_song = path;
    g_off = g_cnt = 0;
    const double limit = 3.0 + file_size(path) / 1e7;   // 28 Mo : 5,8 s au plus
    bool done = false;
    while (!done && now_s() - t0 < limit)
        sl_drain(limit - (now_s() - t0), [&](const OscMsg &m) {
            return done = m.addr.compare(0, 10, "/Hydrogen/") == 0;
        });
    usleep(100000);                                   // la fin de la rafale
    say("DLS Drum : morceau chargé en %.1f s%s", now_s() - t0, done ? "" : " (sans retour d'Hydrogen)");
    /* Hydrogen ignore le choix d'un pattern qu'il croit déjà choisi
     * (setSelectedPatternNumber : même numéro -> rien), mais le chargement a
     * remis le premier pattern en lecture. Après un changement de kit, le pont
     * redemande souvent le même numéro : il serait ignoré et les premières
     * notes viendraient du mauvais pattern. On force donc 1 puis 0. */
    /* Hydrogen reste occupé après son retour d'état (mesuré, gros morceau :
     * 100 % d'un cœur pendant 4,5 s, retour d'état à 3,9 s) et, en finissant,
     * remet le premier pattern : un choix fait avant serait perdu. On attend
     * qu'il soit calme (temps processeur), au plus 8 s après l'ouverture. */
    const double ti = now_s();
    const bool idle = wait_hydrogen_idle(t0 + 8.0);
    h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
    h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", 1.0f);
    h2_float("/Hydrogen/SELECT_ONLY_NEXT_PATTERN", 0.0f);
    g_pattern = -1;
    say("DLS Drum : Hydrogen calme %.1f s après son retour d'état%s (ouverture : %.1f s en tout)",
        now_s() - ti, idle ? "" : " (délai dépassé)", now_s() - t0);
    const std::string r = groove > 0 ? set_groove(std::to_string(groove)) : std::string("groove non choisi");
    jack_transport_locate(g_cfg.client, 0);
    return "ok morceau " + path + " : " + std::to_string(g_pat_sizes.size()) + " pattern(s) ; " + r;
}

std::string list_songs()
{
    std::string r = "ok chansons :";
    DIR *d = opendir(songs_dir().c_str());
    if (!d) return r + " (aucune)";
    int n = 0;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.' && file_size(songs_dir() + "/" + e->d_name + "/chanson.txt") > 0) {
            r += std::string(" ") + e->d_name;
            n++;
        }
    closedir(d);
    return n ? r : r + " (aucune)";
}

std::string status()
{
    sl_refresh();
    if (!g_sl_ok) return "erreur : SooperLooper ne répond pas (lancé ? port OSC " + std::to_string(g_cfg.sl_port) + ")";
    std::string r = "ok compensation " + std::to_string(g_cfg.comp_frames ? g_cfg.comp_frames() : 0) +
                    " trames ;";
    for (size_t i = 0; i < g_loops.size(); i++) {
        const Loop &L = g_loops[i];
        if (L.len <= 0) continue;
        char b[160];
        std::snprintf(b, sizeof b, " boucle %zu : %.2f s, état %d, %s ;", i + 1, L.len, L.state,
                      L.normalized ? "normalisée" : "à normaliser");
        r += b;
    }
    return r;
}

std::string handle(const std::string &line)
{
    std::istringstream ss(line);
    std::string cmd, arg;
    ss >> cmd;
    std::getline(ss >> std::ws, arg);               // tout le reste (chemins avec espaces)
    while (!arg.empty() && std::isspace((unsigned char)arg.back())) arg.pop_back();
    if (cmd == "play") return play_from_start();
    if (cmd == "stop") return stop_all();
    if (cmd == "save") return save_song(arg);
    if (cmd == "load") return load_song(arg);
    if (cmd == "list") return list_songs();
    if (cmd == "status") return status();
    if (cmd == "groove") return set_groove(arg);
    if (cmd == "batterie") return open_drum_song(arg);
    if (cmd == "tempo") return set_tempo(arg);
    return "erreur : commande inconnue (play, stop, save NOM, load NOM, list, status, groove N, tempo BPM)";
}

void thread_main()
{
    double next_poll = 0;
    read_h2_current_song();
    while (g_run.load()) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(g_cmd, &rs);
        timeval tv{0, 5000};               // 5 ms : boutons de DLS Looper
        if (select(g_cmd + 1, &rs, nullptr, nullptr, &tv) > 0) {
            char buf[512];
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(g_cmd, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
            if (n > 0) {
                buf[n] = 0;
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                say("commande : %s", line.c_str());
                const std::string rep = handle(line);
                say("%s", rep.c_str());
                sendto(g_cmd, rep.data(), rep.size(), 0, (sockaddr *)&from, fl);
            }
        }
        looper_poll();
        /* Surveiller les boucles : un enregistrement neuf est à normaliser. */
        if (now_s() >= next_poll || g_refresh_now) {
            g_refresh_now = false;
            sl_refresh();
            next_poll = now_s() + 0.2;
        }
    }
}

} // namespace

/* Callback JACK (temps réel) : transport et mesure vers le fichier de contrôle.
 * jack_transport_query est utilisable dans le callback ; le reste : atomiques. */
void ctl_rt_update(jack_client_t *c)
{
    pb_ctl *k = g_ctl.load(std::memory_order_acquire);
    if (!k) return;
    jack_position_t pos;
    const jack_transport_state_t ts = jack_transport_query(c, &pos);
    PB_STORE_RLX(&k->rolling, ts == JackTransportRolling ? 1u : 0u);
    if ((pos.valid & JackPositionBBT) && pos.ticks_per_beat > 0) {
        double f = pos.tick / pos.ticks_per_beat;
        if (f < 0) f = 0;
        if (f > 0.99999) f = 0.99999;
        PB_STORE_RLX(&k->bar, (uint32_t)pos.bar);
        PB_STORE_RLX(&k->beat, (uint32_t)pos.beat);
        PB_STORE_RLX(&k->beats_per_bar, (uint32_t)pos.beats_per_bar);
        PB_STORE_RLX(&k->beat_frac, (uint32_t)(f * 65536.0));
        PB_STORE_RLX(&k->bpm, (uint32_t)std::lround(pos.beats_per_minute * 100.0));
    } else {
        PB_STORE_RLX(&k->bar, 0u);
        PB_STORE_RLX(&k->beat, 0u);
        PB_STORE_RLX(&k->bpm, 0u);
    }
    PB_STORE_RLX(&k->client_owner, g_ctl_me);
    PB_STORE_REL(&k->client_heartbeat, PB_LOAD_RLX(&k->client_heartbeat) + 1u);
}

bool ctl_start(const CtlConfig &cfg)
{
    g_cfg = cfg;
    g_sr = jack_get_sample_rate(cfg.client);
    g_cmd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = local(cfg.cmd_port);
    if (bind(g_cmd, (sockaddr *)&a, sizeof a) != 0) {
        std::fprintf(stderr, "Contrôle : port UDP %d déjà pris\n", cfg.cmd_port);
        close(g_cmd);
        return false;
    }
    g_osc = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in o = local(0);
    bind(g_osc, (sockaddr *)&o, sizeof o);
    socklen_t ol = sizeof o;
    getsockname(g_osc, (sockaddr *)&o, &ol);
    g_reply_url = "osc.udp://127.0.0.1:" + std::to_string(ntohs(o.sin_port)) + "/";
    g_run.store(true);
    g_thread = std::thread(thread_main);
    say("commandes sur UDP 127.0.0.1:%d (play, stop, save, load, list, status)", cfg.cmd_port);
    return true;
}

void ctl_stop()
{
    if (!g_run.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    close(g_cmd);
    close(g_osc);
}

int ctl_send_command(int cmd_port, const std::string &cmd, double timeout_s)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = local(cmd_port);
    timeval tv{(time_t)timeout_s, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sendto(s, cmd.data(), cmd.size(), 0, (sockaddr *)&a, sizeof a);
    char buf[2048];
    ssize_t n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) { std::fprintf(stderr, "pedal_bridge ne répond pas (port %d)\n", cmd_port); return 1; }
    buf[n] = 0;
    std::printf("%s\n", buf);
    return std::strncmp(buf, "ok", 2) == 0 ? 0 : 1;
}
