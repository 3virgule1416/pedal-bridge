"""Lecture de fichiers MIDI standard (SMF type 0 et 1), sans dépendance.

    m = read_midi(octets)
    m.ppq                      # ticks par noire
    m.notes                    # [(tick, note, vélocité)] (note-on, canal ignoré)
    m.time_sigs                # [(tick, numérateur, dénominateur)]
    m.tempos                   # [(tick, microsecondes par noire)]
    m.length                   # tick du dernier événement (fin de piste comprise)
"""
import struct


class Midi:
    def __init__(self):
        self.ppq = 480
        self.notes = []
        self.time_sigs = []
        self.tempos = []
        self.names = []
        self.length = 0


def _varlen(data, i):
    v = 0
    while True:
        b = data[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, i


def read_midi(data):
    if data[:4] != b"MThd":
        raise ValueError("pas un fichier MIDI (MThd absent)")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, div = struct.unpack(">HHH", data[8:14])
    if div & 0x8000:
        raise ValueError("division SMPTE non gérée")
    m = Midi()
    m.ppq = div
    i = 8 + hlen
    for _ in range(ntracks):
        if data[i:i + 4] != b"MTrk":
            break
        tlen = struct.unpack(">I", data[i + 4:i + 8])[0]
        j, end = i + 8, i + 8 + tlen
        tick, status = 0, 0
        while j < end:
            dt, j = _varlen(data, j)
            tick += dt
            b = data[j]
            if b & 0x80:
                status = b
                j += 1
            if status == 0xFF:                       # méta
                mtype = data[j]
                ln, j = _varlen(data, j + 1)
                body = data[j:j + ln]
                j += ln
                if mtype == 0x58 and ln >= 2:
                    m.time_sigs.append((tick, body[0], 2 ** body[1]))
                elif mtype == 0x51 and ln == 3:
                    m.tempos.append((tick, int.from_bytes(body, "big")))
                elif mtype == 0x03:
                    m.names.append(body.decode("latin-1", "replace"))
                elif mtype == 0x2F:
                    m.length = max(m.length, tick)
                    break
            elif status in (0xF0, 0xF7):                # sysex
                ln, j = _varlen(data, j)
                j += ln
            else:
                kind = status & 0xF0
                if kind in (0xC0, 0xD0):
                    j += 1
                else:
                    a, v = data[j], data[j + 1]
                    j += 2
                    if kind == 0x90 and v > 0:
                        m.notes.append((tick, a, v))
            m.length = max(m.length, tick)
        i = end
    m.notes.sort()
    return m
