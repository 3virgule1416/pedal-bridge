/*
 * pedal_bridge : client JACK du pont PiPedal <-> JACK.
 *
 * Pour chaque lien N demandé :
 *   - sendN_L / sendN_R   (sorties JACK) : lit /dev/shm/pedal-bridge-LN-send
 *                                          (écrit par le plugin Bridge Send)
 *   - returnN_L/returnN_R (entrées JACK) : écrit /dev/shm/pedal-bridge-LN-return
 *                                          (lu par le plugin Bridge Return)
 *
 * PiPedal (ALSA) et JACK ont chacun leur horloge. Un rééchantillonneur
 * zita VResampler par sens absorbe la dérive : son ratio est corrigé par une
 * boucle PI qui garde le remplissage du tampon près d'une cible.
 *
 * Temps réel : le callback JACK ne fait ni allocation, ni verrou, ni appel
 * système. Ouverture des tampons, (re)construction des rééchantillonneurs et
 * affichage se font dans le thread principal.
 */
#include <jack/jack.h>
#include <zita-resampler/vresampler.h>

#include "pb_shm.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace {

constexpr uint32_t MAXF = 8192;      // taille de période JACK maximale gérée
constexpr unsigned HLEN = 32;        // demi-longueur du filtre zita (qualité)
constexpr double   RR_MAX = 0.005;   // correction de ratio maximale (±0,5 %)

/* ---------------------------------------------------------------------------
 * Boucle PI sur le remplissage.
 * Erreur e (secondes) = (remplissage filtré - cible) / Fp.
 * Correction c = Kp·e + Ki·∫e dt, ratio relatif = 1 - c.
 * Avec Kp = 2·ζ·ω, Ki = ω², ω = 2π·0,02 Hz, ζ = 1 : réglage lent et sans
 * dépassement ; l'intégrale finit par valoir la dérive entre les horloges.
 * ------------------------------------------------------------------------- */
struct Ctl {
    double target = 512;
    double filt = 0, integ = 0;
    double kp = 2.0 * 2.0 * M_PI * 0.02;
    double ki = (2.0 * M_PI * 0.02) * (2.0 * M_PI * 0.02);

    void reset(double fill) { filt = fill; }   // l'intégrale (la dérive) est gardée

    double update(double fill, double dt, double fp)
    {
        const double a = 1.0 - std::exp(-dt / 0.3);   // passe-bas 0,3 s : lisse la gigue
        filt += a * (fill - filt);
        const double e = (filt - target) / fp;
        integ += ki * e * dt;
        if (integ > RR_MAX) integ = RR_MAX;
        if (integ < -RR_MAX) integ = -RR_MAX;
        double c = kp * e + integ;
        if (c > 2 * RR_MAX) c = 2 * RR_MAX;
        if (c < -2 * RR_MAX) c = -2 * RR_MAX;
        return 1.0 - c;
    }
};

enum { ST_WAIT = 0, ST_PLAY = 1 };

struct Path {
    uint32_t link_no = 0;
    int      dir = PB_DIR_SEND;
    uint64_t me = 0;

    pb_link  link{};
    std::atomic<bool> enabled{false};   // le callback ne touche à rien si faux
    bool     opened = false;
    bool     warned = false;
    double   fp = 0;                    // fréquence côté PiPedal
    std::atomic<uint32_t> target{512};  // cible de remplissage (trames à Fp)
    std::atomic<uint32_t> latency{0};   // latence déclarée à JACK (trames JACK)

    VResampler rs;
    Ctl      ctl;
    pb_watch watch{};
    int      state = ST_WAIT;
    float    env = 0, last[2] = {0, 0};

    jack_port_t *port[2] = {nullptr, nullptr};
    std::vector<float> scratch;

    /* Statistiques (écrites par le callback, lues par le thread principal). */
    std::atomic<double>   st_fill{0}, st_rr{1}, st_drift{0};
    std::atomic<uint64_t> st_under{0}, st_over{0}, st_starts{0};
    std::atomic<int>      st_state{0}, st_peer{0}, st_owner{1};
};

jack_client_t        *g_client = nullptr;
uint32_t              g_fj = 48000;
std::atomic<uint32_t> g_period{256};
std::atomic<uint64_t> g_cycles{0}, g_xruns{0};
std::atomic<bool>     g_shutdown{false};
volatile sig_atomic_t g_stop = 0;
std::vector<Path *>   g_paths;
double                g_target_ms = 0;   // 0 = automatique
double                g_pp_lat_ms = -1;  // latence propre de PiPedal (aller + retour), -1 = lue dans sa config
bool                  g_pp_lat_fixed = false;  // donnée en option : ne pas relire la config
int                   g_sl_port = 0;     // port OSC de SooperLooper à tenir à jour (0 = non)

/* Latence ALSA de PiPedal : bufferSize × numberOfBuffers / sampleRate, lue dans
 * /var/pipedal/AudioConfig.json (la valeur affichée dans son interface). */
double read_pipedal_latency_ms()
{
    FILE *f = std::fopen("/var/pipedal/AudioConfig.json", "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    auto num = [&](const char *key) -> double {
        const char *p = std::strstr(buf, key);
        if (!p) return 0;
        p = std::strchr(p, ':');
        return p ? std::atof(p + 1) : 0;
    };
    const double bs = num("\"bufferSize\""), nb = num("\"numberOfBuffers\""), sr = num("\"sampleRate\"");
    return (bs > 0 && nb > 0 && sr > 0) ? 1000.0 * bs * nb / sr : 0;
}

void prime(VResampler &rs)
{
    rs.reset();
    rs.inp_count = rs.inpsize() / 2 - 1;
    rs.inp_data = nullptr;
    rs.out_count = 1u << 30;
    rs.out_data = nullptr;
    rs.process();
}

/* --- PiPedal -> JACK ------------------------------------------------------ */
void process_send(Path &p, uint32_t n)
{
    float *o[2] = { (float *)jack_port_get_buffer(p.port[0], n),
                    (float *)jack_port_get_buffer(p.port[1], n) };
    if (!p.enabled.load(std::memory_order_acquire) || n > MAXF) {
        std::memset(o[0], 0, n * sizeof(float));
        std::memset(o[1], 0, n * sizeof(float));
        return;
    }
    pb_link *l = &p.link;
    const bool owner = pb_reader_claim(l, p.me);
    p.st_owner.store(owner, std::memory_order_relaxed);
    bool tail = false;
    uint32_t produced = 0;
    float env_end = 0.0f;

    if (owner) {
        pb_reader_beat(l, n);
        pb_watch_update(&p.watch, PB_LOAD_ACQ(&l->hdr->writer_heartbeat), n);
        const bool alive = p.watch.alive;
        p.st_peer.store(alive, std::memory_order_relaxed);
        uint32_t fill = pb_read_available(l);
        const uint32_t target = p.target.load(std::memory_order_relaxed);
        p.ctl.target = target;

        if (p.state == ST_WAIT && alive && fill >= target) {
            pb_reader_resync(l, target);     // jeter le vieux son en trop
            fill = target;
            prime(p.rs);
            p.ctl.reset(fill);
            p.state = ST_PLAY;
            p.env = 0.0f;
            p.st_starts.fetch_add(1, std::memory_order_relaxed);
        }
        if (p.state == ST_PLAY) {
            const double rr = p.ctl.update(fill, (double)n / g_fj, p.fp);
            p.rs.set_rratio(rr);
            p.st_rr.store(rr, std::memory_order_relaxed);
            p.st_drift.store(p.ctl.integ, std::memory_order_relaxed);   // + : PiPedal plus rapide

            pb_span sp;
            pb_read_reserve(l, PB_CAPACITY, &sp);
            p.rs.out_data = p.scratch.data();
            p.rs.out_count = n;
            p.rs.inp_data = sp.p1;
            p.rs.inp_count = sp.n1;
            p.rs.process();
            uint32_t used = sp.n1 - p.rs.inp_count;
            if (p.rs.out_count && sp.n2) {
                p.rs.inp_data = sp.p2;
                p.rs.inp_count = sp.n2;
                p.rs.process();
                used += sp.n2 - p.rs.inp_count;
            }
            pb_read_commit(l, used);
            produced = n - p.rs.out_count;
            if (produced < n) {
                pb_count_underrun(l, n - produced);
                p.st_under.fetch_add(n - produced, std::memory_order_relaxed);
                p.state = ST_WAIT;
                tail = true;
            } else {
                env_end = 1.0f;
            }
        }
        p.st_fill.store(p.ctl.filt, std::memory_order_relaxed);
    } else if (p.state == ST_PLAY) {
        p.state = ST_WAIT;
        tail = true;
    }
    p.st_state.store(p.state, std::memory_order_relaxed);

    /* Sortie : fondu sur ce qui a été produit, puis queue vers zéro. */
    const float *s = p.scratch.data();
    float e = p.env;
    const float de = produced ? (env_end - p.env) / (float)produced : 0.0f;
    for (uint32_t i = 0; i < produced; i++, e += de) {
        o[0][i] = s[2 * i] * e;
        o[1][i] = s[2 * i + 1] * e;
        p.last[0] = o[0][i];
        p.last[1] = o[1][i];
    }
    const uint32_t rest = n - produced;
    for (int c = 0; c < 2; c++) {
        if (tail && rest) {
            float t = p.last[c];
            const float dt = t / (float)rest;
            for (uint32_t i = produced; i < n; i++) { t -= dt; o[c][i] = t; }
        } else {
            for (uint32_t i = produced; i < n; i++) o[c][i] = 0.0f;
        }
    }
    p.env = env_end;
    if (p.state != ST_PLAY) p.last[0] = p.last[1] = 0.0f;
}

/* --- JACK -> PiPedal ------------------------------------------------------ */
void process_return(Path &p, uint32_t n)
{
    if (!p.enabled.load(std::memory_order_acquire) || n > MAXF) return;
    const float *in[2] = { (const float *)jack_port_get_buffer(p.port[0], n),
                           (const float *)jack_port_get_buffer(p.port[1], n) };
    pb_link *l = &p.link;
    const bool owner = pb_writer_claim(l, p.me);
    p.st_owner.store(owner, std::memory_order_relaxed);
    if (!owner) { p.state = ST_WAIT; p.st_state.store(ST_WAIT); return; }

    pb_writer_beat(l, n);
    pb_watch_update(&p.watch, PB_LOAD_ACQ(&l->hdr->reader_heartbeat), n);
    p.st_peer.store(p.watch.alive, std::memory_order_relaxed);
    if (!p.watch.alive) {               // pas de Return actif : on n'écrit rien
        p.state = ST_WAIT;
        p.st_state.store(ST_WAIT, std::memory_order_relaxed);
        return;
    }
    const uint32_t target = p.target.load(std::memory_order_relaxed);
    p.ctl.target = target;
    pb_writer_set_target(l, target);

    const uint64_t w = PB_LOAD_RLX(&l->hdr->write_idx);
    const uint64_t r = PB_LOAD_ACQ(&l->hdr->read_idx);
    const uint32_t fill = (w - r) > PB_CAPACITY ? PB_CAPACITY : (uint32_t)(w - r);
    if (p.state == ST_WAIT) {
        prime(p.rs);
        p.ctl.reset(target);
        p.state = ST_PLAY;
        p.st_starts.fetch_add(1, std::memory_order_relaxed);
    }
    const double rr = p.ctl.update(fill, (double)n / g_fj, p.fp);
    p.rs.set_rratio(rr);
    p.st_rr.store(rr, std::memory_order_relaxed);
    p.st_drift.store(-p.ctl.integ, std::memory_order_relaxed);   // + : PiPedal plus rapide
    p.st_fill.store(p.ctl.filt, std::memory_order_relaxed);
    p.st_state.store(ST_PLAY, std::memory_order_relaxed);

    float *s = p.scratch.data();
    for (uint32_t i = 0; i < n; i++) { s[2 * i] = in[0][i]; s[2 * i + 1] = in[1][i]; }

    pb_span sp;
    pb_write_reserve(l, PB_CAPACITY, &sp);
    p.rs.inp_data = s;
    p.rs.inp_count = n;
    p.rs.out_data = sp.p1;
    p.rs.out_count = sp.n1;
    p.rs.process();
    uint32_t produced = sp.n1 - p.rs.out_count;
    if (p.rs.inp_count && sp.n2) {
        p.rs.out_data = sp.p2;
        p.rs.out_count = sp.n2;
        p.rs.process();
        produced += sp.n2 - p.rs.out_count;
    }
    pb_write_commit(l, produced);
    if (p.rs.inp_count) {               // tampon plein : le reste est perdu
        pb_count_overrun(l, p.rs.inp_count);
        p.st_over.fetch_add(p.rs.inp_count, std::memory_order_relaxed);
    }
}

int on_process(jack_nframes_t n, void *)
{
    for (Path *p : g_paths) {
        if (p->dir == PB_DIR_SEND) process_send(*p, n);
        else process_return(*p, n);
    }
    g_cycles.fetch_add(1, std::memory_order_release);
    return 0;
}

int on_bufsize(jack_nframes_t n, void *) { g_period.store(n); return 0; }

/* Latence déclarée à JACK, pour que SooperLooper (autoset_latency) compense :
 *   sendN   : latence de capture  = entrée de PiPedal + trajet aller du pont ;
 *   returnN : latence de lecture  = trajet retour du pont + sortie de PiPedal.
 * Appelé par JACK dans un thread de notification (pas le thread audio). */
void on_latency(jack_latency_callback_mode_t mode, void *)
{
    for (Path *p : g_paths) {
        const bool mine = (mode == JackCaptureLatency && p->dir == PB_DIR_SEND) ||
                          (mode == JackPlaybackLatency && p->dir == PB_DIR_RETURN);
        if (!mine) continue;
        jack_latency_range_t r;
        r.min = r.max = p->latency.load();
        for (int c = 0; c < 2; c++) jack_port_set_latency_range(p->port[c], mode, &r);
    }
}

/* Trajet du pont dans un sens : réserve + filtre du rééchantillonneur
 * (mesuré par pb_fakehost : 2 × (256 + 32) trames = 12,0 ms aller-retour).
 * On y ajoute la moitié de la latence propre de PiPedal. */
uint32_t compute_latency(const Path &p)
{
    const double bridge = (p.target.load() + HLEN) * (double)g_fj / p.fp;
    const double pp = g_pp_lat_ms > 0 ? g_pp_lat_ms * 0.5 * g_fj / 1000.0 : 0;
    return (uint32_t)std::lround(bridge + pp);
}

void update_latency(Path &p)
{
    const uint32_t l = compute_latency(p);
    if (l == p.latency.load()) return;
    p.latency.store(l);
    jack_recompute_total_latencies(g_client);
    std::printf("Lien %u %s : latence déclarée à JACK %u trames (%.1f ms)\n", p.link_no,
                p.dir == PB_DIR_SEND ? "envoi " : "retour", l, 1000.0 * l / g_fj);
    std::fflush(stdout);
}

/* --- Compensation de latence de SooperLooper (thread principal) -----------
 * SooperLooper 1.7.8 ne lit pas la latence des ports JACK. On lui donne celle
 * du premier lien : input_latency = envoi, output_latency = retour, pour toutes
 * les boucles (/sl/-1/set). Renvoyé à chaque changement et toutes les 5 s, au
 * cas où SooperLooper aurait redémarré (il oublie alors ces réglages). */
size_t osc_pad(char *p, const char *s)
{
    const size_t n = std::strlen(s), m = (n + 4) & ~(size_t)3;
    std::memset(p, 0, m);
    std::memcpy(p, s, n);
    return m;
}

void osc_set_all(int port, const char *param, float value)
{
    static int s = -1;
    if (s < 0) s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    char msg[128];
    size_t n = osc_pad(msg, "/sl/-1/set");
    n += osc_pad(msg + n, ",sf");
    n += osc_pad(msg + n, param);
    uint32_t be;
    std::memcpy(&be, &value, 4);
    be = htonl(be);
    std::memcpy(msg + n, &be, 4);
    n += 4;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, msg, n, 0, (sockaddr *)&a, sizeof a);
}

void sl_sync_latency(double now)
{
    static uint32_t sent_in = 0, sent_out = 0;
    static double last = 0;
    if (!g_sl_port) return;
    uint32_t in = 0, out = 0;
    for (Path *p : g_paths) {                 // premier lien seulement
        if (p->link_no != g_paths.front()->link_no || !p->enabled.load()) continue;
        if (p->dir == PB_DIR_SEND) in = p->latency.load();
        else out = p->latency.load();
    }
    if (!in || !out) return;                  // plugins pas encore là
    const bool changed = in != sent_in || out != sent_out;
    if (!changed && now - last < 5.0) return;
    osc_set_all(g_sl_port, "autoset_latency", 0.0f);
    osc_set_all(g_sl_port, "input_latency", (float)in);
    osc_set_all(g_sl_port, "output_latency", (float)out);
    last = now;
    if (changed)
        std::printf("SooperLooper (OSC %d) : compensation entrée %u + sortie %u trames (%.1f ms)\n",
                    g_sl_port, in, out, 1000.0 * (in + out) / g_fj);
    std::fflush(stdout);
    sent_in = in;
    sent_out = out;
}

int on_xrun(void *) { g_xruns.fetch_add(1, std::memory_order_relaxed); return 0; }
void on_shutdown(void *) { g_shutdown.store(true); }
void on_signal(int) { g_stop = 1; }

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void sleep_s(double s)
{
    timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, nullptr);
}

/* Attend que le callback ait fini d'utiliser un chemin désactivé. */
void wait_cycles(int k)
{
    const uint64_t c0 = g_cycles.load(std::memory_order_acquire);
    const double t0 = now_s();
    while (g_cycles.load(std::memory_order_acquire) < c0 + (uint64_t)k && now_s() - t0 < 1.0)
        sleep_s(0.001);
}

uint32_t peer_rate(const Path &p)
{
    return p.dir == PB_DIR_SEND ? PB_LOAD_RLX(&p.link.hdr->writer_rate)
                                : PB_LOAD_RLX(&p.link.hdr->reader_rate);
}

uint32_t compute_target(const Path &p)
{
    if (g_target_ms > 0) return (uint32_t)std::lround(g_target_ms * p.fp / 1000.0);
    uint32_t peer_block = p.dir == PB_DIR_SEND ? PB_LOAD_RLX(&p.link.hdr->writer_block)
                                               : PB_LOAD_RLX(&p.link.hdr->reader_block);
    if (peer_block == 0 || peer_block > 4096) peer_block = 128;
    /* Une période JACK (convertie à Fp) + un bloc PiPedal.
     * Mesuré (test-jack.sh) : JACK 128 -> 256 trames (5,3 ms), sans coupure. */
    const double jack_in_fp = g_period.load() * p.fp / g_fj;
    uint32_t t = (uint32_t)std::ceil(jack_in_fp) + peer_block;
    return t > PB_CAPACITY / 2 ? PB_CAPACITY / 2 : t;
}

/* (Re)construit le rééchantillonneur pour la fréquence de PiPedal (hors RT). */
void configure(Path &p, uint32_t fp)
{
    p.enabled.store(false, std::memory_order_release);
    wait_cycles(2);
    p.fp = fp;
    const double ratio = p.dir == PB_DIR_SEND ? (double)g_fj / fp : (double)fp / g_fj;
    p.rs.setup(ratio, 2, HLEN);
    p.state = ST_WAIT;
    p.env = 0;
    p.last[0] = p.last[1] = 0;
    pb_watch_init(&p.watch, g_fj / 4);
    p.target.store(compute_target(p));
    if (p.dir == PB_DIR_SEND) pb_reader_take(&p.link, p.me, g_fj);
    else { pb_writer_take(&p.link, p.me, g_fj); pb_writer_set_target(&p.link, p.target.load()); }
    p.enabled.store(true, std::memory_order_release);
    std::printf("Lien %u %s : PiPedal %u Hz <-> JACK %u Hz, cible %u trames (%.1f ms)\n",
                p.link_no, p.dir == PB_DIR_SEND ? "envoi " : "retour", fp, g_fj,
                p.target.load(), 1000.0 * p.target.load() / fp);
    update_latency(p);
}

/* Thread principal : ouvrir les tampons quand les plugins apparaissent,
 * suivre les changements de fréquence / période. */
void supervise(Path &p)
{
    if (!p.opened) {
        char err[256];
        pb_link l;
        int rc = pb_link_open(&l, p.link_no, p.dir, 0, err, sizeof err);
        if (rc) {
            if (!p.warned) {
                std::printf("Lien %u %s : en attente du plugin %s dans PiPedal (%s)\n",
                            p.link_no, p.dir == PB_DIR_SEND ? "envoi " : "retour",
                            p.dir == PB_DIR_SEND ? "Bridge Send" : "Bridge Return", err);
                p.warned = true;
            }
            return;
        }
        p.link = l;
        p.opened = true;
    }
    const uint32_t fp = peer_rate(p);
    if (fp == 0) return;                          // le plugin n'a pas encore tourné
    if (!p.enabled.load() || fp != (uint32_t)p.fp) {
        configure(p, fp);
        return;
    }
    const uint32_t t = compute_target(p);
    if (t != p.target.load()) {
        p.target.store(t);
        std::printf("Lien %u %s : nouvelle cible %u trames\n", p.link_no,
                    p.dir == PB_DIR_SEND ? "envoi " : "retour", t);
    }
    update_latency(p);
}

void print_stats(double t)
{
    std::printf("[%7.1fs] JACK xruns %llu, DSP %.1f %%\n", t,
                (unsigned long long)g_xruns.load(), jack_cpu_load(g_client));
    for (Path *p : g_paths) {
        const char *etat = !p->enabled.load() ? "fermé"
                         : !p->st_owner.load() ? "pris par un autre"
                         : p->st_state.load() == ST_PLAY ? "actif" : "attente";
        std::printf("   L%u %s : %-17s plugin:%-3s rempl %6.1f/%u  horloge PiPedal %+7.1f ppm (correction %+7.1f)  "
                    "départs %llu  sous-débits %llu  sur-débits %llu\n",
                    p->link_no, p->dir == PB_DIR_SEND ? "envoi " : "retour", etat,
                    p->st_peer.load() ? "oui" : "non", p->st_fill.load(), p->target.load(),
                    p->st_drift.load() * 1e6, (p->st_rr.load() - 1.0) * 1e6,
                    (unsigned long long)p->st_starts.load(),
                    (unsigned long long)p->st_under.load(),
                    (unsigned long long)p->st_over.load());
    }
    std::fflush(stdout);
}

void usage()
{
    std::fprintf(stderr,
        "usage : pedal_bridge [options]\n"
        "  --links 1,2      liens à ouvrir (défaut : 1)\n"
        "  --name NOM       nom du client JACK (défaut : pedal_bridge)\n"
        "  --server NOM     serveur JACK (défaut : celui par défaut)\n"
        "  --target-ms X    réserve visée dans chaque tampon (défaut : automatique)\n"
        "  --stats S        statistiques toutes les S secondes (défaut 5, 0 = jamais)\n"
        "  --loopback       relie sendN -> returnN dans JACK (pour les tests)\n"
        "  --pipedal-latency-ms X  latence propre de PiPedal (entrée + sortie) déclarée\n"
        "                   à JACK en plus du pont (défaut : lue dans /var/pipedal/AudioConfig.json,\n"
        "                   relue toutes les secondes)\n"
        "  --sl-osc PORT    tient à jour la compensation de latence de SooperLooper (OSC,\n"
        "                   ex. 9951) : il ne lit pas la latence des ports JACK\n"
        "Ports : sendN_L/R (sorties, son de PiPedal), returnN_L/R (entrées, vers PiPedal)\n");
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<uint32_t> links;
    std::string name = "pedal_bridge", server;
    double stats_every = 5;
    bool loopback = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "--links") {
            std::string s = next();
            size_t pos = 0;
            while (pos < s.size()) {
                size_t q = s.find(',', pos);
                long v = std::strtol(s.substr(pos, q - pos).c_str(), nullptr, 10);
                if (v < 1 || v > (long)PB_MAX_LINKS) { usage(); return 2; }
                links.push_back((uint32_t)v);
                if (q == std::string::npos) break;
                pos = q + 1;
            }
        }
        else if (a == "--name") name = next();
        else if (a == "--server") server = next();
        else if (a == "--target-ms") g_target_ms = std::atof(next());
        else if (a == "--stats") stats_every = std::atof(next());
        else if (a == "--loopback") loopback = true;
        else if (a == "--pipedal-latency-ms") { g_pp_lat_ms = std::atof(next()); g_pp_lat_fixed = true; }
        else if (a == "--sl-osc") g_sl_port = std::atoi(next());
        else { usage(); return 2; }
    }
    if (links.empty()) links.push_back(1);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "Avertissement : mlockall a échoué (%s)\n", std::strerror(errno));

    jack_status_t status;
    jack_options_t opts = (jack_options_t)(JackNoStartServer | (server.empty() ? 0 : JackServerName));
    g_client = server.empty() ? jack_client_open(name.c_str(), opts, &status)
                              : jack_client_open(name.c_str(), opts, &status, server.c_str());
    if (!g_client) {
        std::fprintf(stderr, "Impossible de joindre le serveur JACK%s%s (status 0x%x)\n",
                     server.empty() ? "" : " ", server.c_str(), (unsigned)status);
        return 1;
    }
    g_fj = jack_get_sample_rate(g_client);
    g_period.store(jack_get_buffer_size(g_client));

    for (uint32_t ln : links) {
        for (int dir = 0; dir < 2; dir++) {
            Path *p = new Path;
            p->link_no = ln;
            p->dir = dir;
            p->me = pb_new_owner_id();
            p->scratch.assign(2 * MAXF, 0.0f);
            for (int c = 0; c < 2; c++) {
                char pn[64];
                std::snprintf(pn, sizeof pn, "%s%u_%c", dir == PB_DIR_SEND ? "send" : "return", ln, c ? 'R' : 'L');
                p->port[c] = jack_port_register(g_client, pn, JACK_DEFAULT_AUDIO_TYPE,
                                                dir == PB_DIR_SEND ? JackPortIsOutput : JackPortIsInput, 0);
                if (!p->port[c]) { std::fprintf(stderr, "Port %s refusé par JACK\n", pn); return 1; }
            }
            g_paths.push_back(p);
        }
    }

    jack_set_process_callback(g_client, on_process, nullptr);
    jack_set_buffer_size_callback(g_client, on_bufsize, nullptr);
    jack_set_latency_callback(g_client, on_latency, nullptr);
    if (g_pp_lat_ms < 0) g_pp_lat_ms = read_pipedal_latency_ms();
    std::printf("Latence propre de PiPedal comptée : %.1f ms (entrée + sortie)\n", g_pp_lat_ms);
    jack_set_xrun_callback(g_client, on_xrun, nullptr);
    jack_on_shutdown(g_client, on_shutdown, nullptr);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (jack_activate(g_client)) { std::fprintf(stderr, "jack_activate a échoué\n"); return 1; }
    std::printf("pedal_bridge : JACK %u Hz, période %u, liens", g_fj, g_period.load());
    for (uint32_t ln : links) std::printf(" %u", ln);
    std::printf("\n");

    if (loopback) {
        const char *me = jack_get_client_name(g_client);
        for (uint32_t ln : links)
            for (char c : {'L', 'R'}) {
                std::string a = std::string(me) + ":send" + std::to_string(ln) + "_" + c;
                std::string b = std::string(me) + ":return" + std::to_string(ln) + "_" + c;
                if (jack_connect(g_client, a.c_str(), b.c_str()) != 0)
                    std::fprintf(stderr, "Connexion %s -> %s impossible\n", a.c_str(), b.c_str());
            }
    }

    const double t0 = now_s();
    double next_stats = t0 + stats_every;
    double next_cfg = t0 + 1.0;
    while (!g_stop && !g_shutdown.load()) {
        for (Path *p : g_paths) supervise(*p);
        const double t = now_s();
        if (t >= next_cfg) {
            next_cfg = t + 1.0;
            if (!g_pp_lat_fixed) {          // réglages audio de PiPedal changés ?
                const double v = read_pipedal_latency_ms();
                if (v > 0 && std::fabs(v - g_pp_lat_ms) > 0.01) {
                    std::printf("Latence propre de PiPedal : %.1f -> %.1f ms\n", g_pp_lat_ms, v);
                    g_pp_lat_ms = v;
                    for (Path *p : g_paths) if (p->enabled.load()) update_latency(*p);
                }
            }
            sl_sync_latency(t);
        }
        if (stats_every > 0 && t >= next_stats) {
            print_stats(t - t0);
            next_stats = t + stats_every;
        }
        sleep_s(0.1);
    }

    if (g_shutdown.load()) std::fprintf(stderr, "Le serveur JACK s'est arrêté.\n");
    else { print_stats(now_s() - t0); jack_deactivate(g_client); }
    for (Path *p : g_paths) {
        if (p->opened) {
            if (p->dir == PB_DIR_SEND) pb_reader_release(&p->link, p->me);
            else { pb_writer_set_target(&p->link, 0); pb_writer_release(&p->link, p->me); }
            pb_link_close(&p->link);
        }
    }
    if (!g_shutdown.load()) jack_client_close(g_client);
    return 0;
}
