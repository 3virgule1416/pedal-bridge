/* Client websocket de PiPedal (voir pipedal_ws.h). */
#include "pipedal_ws.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) close(fd); }
};

bool send_all(int fd, const void *p, size_t n)
{
    const char *c = (const char *)p;
    while (n > 0) {
        const ssize_t k = send(fd, c, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

bool recv_all(int fd, void *p, size_t n)
{
    char *c = (char *)p;
    while (n > 0) {
        const ssize_t k = recv(fd, c, n, 0);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

/* Trame texte du client (masquée, comme l'exige le protocole). */
bool send_text(int fd, const std::string &s)
{
    static std::mt19937 rng{std::random_device{}()};
    std::string f;
    f.push_back((char)0x81);
    const size_t n = s.size();
    if (n < 126) {
        f.push_back((char)(0x80 | n));
    } else if (n < 65536) {
        f.push_back((char)(0x80 | 126));
        f.push_back((char)(n >> 8));
        f.push_back((char)(n & 0xff));
    } else {
        f.push_back((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) f.push_back((char)((uint64_t)n >> (8 * i)));
    }
    const uint32_t m = rng();
    const unsigned char mask[4] = {(unsigned char)(m >> 24), (unsigned char)(m >> 16),
                                   (unsigned char)(m >> 8), (unsigned char)m};
    f.append((const char *)mask, 4);
    for (size_t i = 0; i < n; i++) f.push_back((char)(s[i] ^ mask[i % 4]));
    return send_all(fd, f.data(), f.size());
}

/* Message complet du serveur (trames de suite rassemblées). Retourne
 * l'opcode du message (1 texte, 8 fermeture...) ou -1. */
int recv_message(int fd, std::string &out)
{
    out.clear();
    int op = -1;
    for (;;) {
        unsigned char h[2];
        if (!recv_all(fd, h, 2)) return -1;
        uint64_t n = h[1] & 0x7f;
        if (n == 126) {
            unsigned char e[2];
            if (!recv_all(fd, e, 2)) return -1;
            n = ((uint64_t)e[0] << 8) | e[1];
        } else if (n == 127) {
            unsigned char e[8];
            if (!recv_all(fd, e, 8)) return -1;
            n = 0;
            for (int i = 0; i < 8; i++) n = (n << 8) | e[i];
        }
        if (n > (16u << 20)) return -1;                    // garde-fou
        unsigned char mask[4] = {0, 0, 0, 0};
        if ((h[1] & 0x80) && !recv_all(fd, mask, 4)) return -1;
        std::string d(n, '\0');
        if (n && !recv_all(fd, &d[0], n)) return -1;
        if (h[1] & 0x80) for (size_t i = 0; i < n; i++) d[i] ^= (char)mask[i % 4];
        const int fop = h[0] & 0x0f;
        if (fop >= 8) {                                    // contrôle (ping, pong, fermeture)
            if (fop == 8) return 8;
            continue;
        }
        if (fop != 0) op = fop;
        out += d;
        if (h[0] & 0x80) return op;
    }
}

/* instanceId de chaque élément dont l'URI commence par prefix. Dans le JSON
 * de PiPedal, "instanceId" précède toujours "uri" dans un élément. */
std::vector<long> find_instances(const std::string &json, const std::string &prefix)
{
    std::vector<long> ids;
    const std::string key = "\"uri\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + key.size())) {
        size_t q = json.find('"', json.find(':', p + key.size()));
        if (q == std::string::npos) break;
        if (json.compare(q + 1, prefix.size(), prefix) != 0) continue;
        const size_t id = json.rfind("\"instanceId\"", p);
        if (id == std::string::npos) continue;
        const size_t colon = json.find(':', id);
        if (colon == std::string::npos || colon > p) continue;
        ids.push_back(std::strtol(json.c_str() + colon + 1, nullptr, 10));
    }
    return ids;
}

}  // namespace

/* --- Connexion qui reste ouverte (pb_leds) --- */
int pipedal_ws_open(int port, std::string &err)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket"; return -1; }
    timeval tv{2, 0};                                  // une trame qui n'arrive pas en entier : connexion perdue
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&a, sizeof a) != 0) { close(fd); err = "PiPedal ne répond pas"; return -1; }
    const std::string req = "GET /pipedal HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Key: cGVkYWwtYnJpZGdlLWtleQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(fd, req.data(), req.size())) { close(fd); err = "poignée de main"; return -1; }
    std::string head;
    char c;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        if (recv(fd, &c, 1, 0) != 1) { close(fd); err = "réponse coupée"; return -1; }
        head.push_back(c);
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) { close(fd); err = "websocket refusé"; return -1; }
    return fd;
}

bool pipedal_ws_send(int fd, const std::string &text) { return send_text(fd, text); }

int pipedal_ws_recv(int fd, std::string &out) { return recv_message(fd, out); }

int pipedal_set_controls(int port, const std::vector<PipedalSet> &sets, std::string &err)
{
    Fd s;
    s.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s.fd < 0) { err = "socket"; return -1; }
    timeval tv{1, 0};
    setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s.fd, (sockaddr *)&a, sizeof a) != 0) { err = "PiPedal ne répond pas (port " + std::to_string(port) + ")"; return -1; }

    const std::string req = "GET /pipedal HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Key: cGVkYWwtYnJpZGdlLWtleQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(s.fd, req.data(), req.size())) { err = "envoi de la poignée de main"; return -1; }
    std::string head;
    char c;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        if (recv(s.fd, &c, 1, 0) != 1) { err = "réponse de PiPedal coupée"; return -1; }
        head.push_back(c);
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) { err = "PiPedal refuse le websocket"; return -1; }

    if (!send_text(s.fd, "[{\"replyTo\":1,\"message\":\"currentPedalboard\"}]")) { err = "envoi"; return -1; }
    std::string msg;
    for (int tries = 0; tries < 50; tries++) {
        const int op = recv_message(s.fd, msg);
        if (op != 1) { err = "pedalboard non reçu"; return -1; }
        if (msg.find("\"currentPedalboard\"") != std::string::npos && msg.find("\"reply\"") != std::string::npos) break;
        msg.clear();
    }
    if (msg.empty()) { err = "pedalboard non reçu"; return -1; }

    int sent = 0;
    for (const PipedalSet &set : sets)
        for (long id : find_instances(msg, set.uri_prefix)) {
            if (!set.property.empty()) {              // propriété fichier : setPatchProperty
                std::string esc;
                for (char ch : set.path) {
                    if (ch == '"' || ch == '\\') esc.push_back('\\');
                    if ((unsigned char)ch >= 0x20) esc.push_back(ch);
                }
                const std::string b = "[{\"message\":\"setPatchProperty\"},{\"instanceId\":" + std::to_string(id) +
                                      ",\"propertyUri\":\"" + set.property + "\",\"value\":{\"otype_\":\"Path\",\"value\":\"" +
                                      esc + "\"}}]";
                if (!send_text(s.fd, b.c_str())) { err = "envoi de setPatchProperty"; return -1; }
                sent++;
                continue;
            }
            char b[256];
            std::snprintf(b, sizeof b,
                          "[{\"message\":\"setControl\"},{\"clientId\":0,\"instanceId\":%ld,\"symbol\":\"%s\",\"value\":%g}]",
                          id, set.symbol.c_str(), (double)set.value);
            if (!send_text(s.fd, b)) { err = "envoi de setControl"; return -1; }
            sent++;
        }
    /* Fermeture propre (trame 8 masquée, sans données). */
    const unsigned char close_frame[6] = {0x88, 0x80, 0, 0, 0, 0};
    send_all(s.fd, close_frame, sizeof close_frame);
    return sent;
}
