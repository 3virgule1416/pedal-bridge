/* Module de contrôle de pedal_bridge : voir controle.h. */
#include "controle.h"
#include "pb_ctl.h"
#include "pb_shm.h"

#include <arpa/inet.h>
#include <jack/transport.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <atomic>
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
        if (n > 0 && decode(buf, (size_t)n, m) && f(m)) return;
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
 * Vérifié sur la 1.2.0-beta : SELECT_AND_PLAY_PATTERN compte à partir de 0,
 * BPM change l'affichage, SONG_MODE_ACTIVATION 0 = mode Pattern,
 * SAVE_SONG_AS écrit le .h2song. Hydrogen n'annonce PAS le pattern
 * sélectionné : le client retient celui qu'il a demandé (g_pattern). */
int g_pattern = -1;               // pattern choisi par « groove », à partir de 0
std::vector<int> g_pat_sizes;     // taille de chaque pattern (ticks Hydrogen), du dernier .h2song lu

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
 * au vrai début et à la vraie fin du groove). Seulement des mesures de 4/4
 * entières pour l'instant. Retourne une phrase pour la réponse. */
std::string apply_cycle(int pattern)
{
    int size = 192;                                   // inconnu : 1 mesure de 4/4
    const bool known = pattern >= 0 && pattern < (int)g_pat_sizes.size() && g_pat_sizes[pattern] > 0;
    if (known) size = g_pat_sizes[pattern];
    if (size % 192 != 0)
        return "taille de pattern " + std::to_string(size) + " ticks : mesure autre que 4/4, cycle inchangé";
    const int bars = size / 192;
    sl_send("/set", {"eighth_per_cycle"}, {(float)(size / 24)});   // 24 ticks par croche
    return "cycle SooperLooper " + std::to_string(bars) + " mesure(s)" + (known ? "" : " (taille inconnue)");
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

std::string set_groove(const std::string &arg)
{
    const int n = std::atoi(arg.c_str());
    if (n < 1 || n > 999) return "erreur : groove N, N à partir de 1 (1er pattern d'Hydrogen)";
    if (!h2_listening()) return "erreur : l'OSC d'Hydrogen ne répond pas (port " + std::to_string(g_cfg.h2_port) + ")";
    h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
    h2_float("/Hydrogen/SELECT_AND_PLAY_PATTERN", (float)(n - 1));
    g_pattern = n - 1;
    return "ok groove " + std::to_string(n) + " ; " + apply_cycle(g_pattern);
}

std::string set_tempo(const std::string &arg)
{
    const double bpm = std::atof(arg.c_str());
    if (bpm < 20 || bpm > 400) return "erreur : tempo entre 20 et 400";
    if (timebase(arg).empty()) return "erreur : pb_timebase ne répond pas";
    if (h2_listening()) h2_float("/Hydrogen/BPM", (float)bpm);    // affichage d'Hydrogen
    return "ok tempo " + arg;
}

/* ----------------------------------------------------------- commandes --- */

void transport_stop_and_pause()
{
    jack_transport_stop(g_cfg.client);
    for (size_t i = 0; i < g_loops.size(); i++)
        if (g_loops[i].len > 0) sl_send("/sl/" + std::to_string(i) + "/hit", {"pause_on"});
}

/* Arrête tout et normalise ce qui doit l'être. */
std::string stop_all()
{
    sl_refresh();
    if (!g_sl_ok) return "erreur : SooperLooper ne répond pas (lancé ? port OSC " + std::to_string(g_cfg.sl_port) + ")";
    if (sl_any_recording()) return "erreur : un enregistrement est en cours, termine-le d'abord";
    transport_stop_and_pause();
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

std::string play_from_start()
{
    std::string r = stop_all();
    if (r.rfind("ok", 0) != 0) return r;
    jack_transport_locate(g_cfg.client, 0);
    usleep(30000);
    int n = 0;
    for (size_t i = 0; i < g_loops.size(); i++)
        if (g_loops[i].len > 0) { sl_send("/sl/" + std::to_string(i) + "/hit", {"trigger"}); n++; }
    /* Hydrogen en mode Pattern (et le groove retenu) à chaque départ : au
     * démarrage de la session, il ignore la commande tant qu'aucun morceau
     * n'est chargé (« No song set yet ») et reste alors en mode Song. */
    if (h2_listening()) {
        h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
        if (g_pattern >= 0) h2_float("/Hydrogen/SELECT_AND_PLAY_PATTERN", (float)g_pattern);
    }
    usleep(40000);                         // laisser SooperLooper armer les boucles
    jack_transport_start(g_cfg.client);
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
    case 0: case SL_PAUSED: return PB_LS_PAUSE;
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
    sl_send("/sl/" + std::to_string(g_sel) + "/hit", {names[b]});
    return std::string("ok ") + names[b] + " (boucle " + std::to_string(g_sel + 1) + ")";
}

/* Lit les commandes de DLS Looper ; appelé ~200 fois par seconde. */
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
        say("fichier de contrôle ouvert (DLS Looper)");
        publish_states();
    }
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
        char t[32];
        std::snprintf(t, sizeof t, "%.2f", tempo * 0.01);
        say("DLS Looper : tempo %s -> %s", t, set_tempo(t).c_str());
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
            usleep(1500000);                         // Hydrogen ne confirme pas : on lui laisse le temps
            h2_float("/Hydrogen/SONG_MODE_ACTIVATION", 0.0f);
            g_pattern = std::atoi(kv["pattern"].c_str());
            if (kv["pattern"].empty()) g_pattern = -1;
            if (g_pattern >= 0) h2_float("/Hydrogen/SELECT_AND_PLAY_PATTERN", (float)g_pattern);
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
    ss >> cmd >> arg;
    if (cmd == "play") return play_from_start();
    if (cmd == "stop") return stop_all();
    if (cmd == "save") return save_song(arg);
    if (cmd == "load") return load_song(arg);
    if (cmd == "list") return list_songs();
    if (cmd == "status") return status();
    if (cmd == "groove") return set_groove(arg);
    if (cmd == "tempo") return set_tempo(arg);
    return "erreur : commande inconnue (play, stop, save NOM, load NOM, list, status, groove N, tempo BPM)";
}

void thread_main()
{
    double next_poll = 0;
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
