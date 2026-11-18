#!/usr/bin/env python3
"""Convertit des packs de rythmes MIDI (.zip de .mid) en morceaux Hydrogen
rangés comme une « chanson » de BeatBuddy : un .h2song par ensemble (une
chanson du pack, ou une plage de tempo), ses patterns triés par partie
(Count, Intro, Verse, Verse Fill, Chorus, Chorus Fill, Transition, Bridge,
Groove, Fill, Outro…), et un fichier « .parts.json » à côté qui dit quelle
partie est sur quel groove (pour monter une chanson plus tard).

    python3 midi2h2.py MODÈLE.h2song KIT.kit SORTIE ZIP [ZIP ...]

MODÈLE / KIT : kit d'écriture (ex. Black Pearl : 2 crashs, china, splash) ;
le choix Kit de DLS Drum convertit ensuite vers les autres kits.

Les fichiers sources ne sont jamais extraits sur le disque : chaque .mid est
lu en mémoire. Les morceaux produits viennent de packs sous licence
personnelle : ils restent sur le Pi, PAS dans le dépôt.

Correspondance des notes : General MIDI, avec des exceptions par pack
(PACKS ci-dessous) : JJ Doge (toms et hi-hats à lui) et Groove Monkee
(kit Perfect Drums : hi-hat fermé sur 63). Notes inconnues : ignorées et
comptées dans le rapport.
"""
import importlib.util
import json
import math
import os
import re
import sys
import zipfile
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


midi_lib = _load("midi_lib")
gen = _load("gen_rythmes")
rk = _load("retarget_kit")

# Note General MIDI -> rôle (voir kits/*.kit).
GM = {
    35: "K", 36: "K", 37: "ST", 38: "S", 39: "CL", 40: "S",
    41: "FT", 43: "FT", 45: "T2", 47: "T2", 48: "T1", 50: "T1",
    42: "H", 44: "P", 46: "O",
    49: "C", 57: "C2", 52: "CH", 55: "Z", 51: "Y", 59: "Y", 53: "B", 56: "CB",
}
# Exceptions par pack (reconnu par un morceau du chemin dans le zip).
PACKS = [
    # JJ Doge (manuel) : tom de rack 2 = 43, tom basse 1 = 48, hi-hat fermé
    # « tip » = 54, hi-hat ouvert 3 = 58 ; note 0 = repère de longueur.
    ("JJ Groove Packs", {43: "T2", 48: "FT", 54: "H", 58: "O", 0: None}),
    # Groove Monkee : kit Perfect Drums ; hi-hat fermé sur 63.
    ("Groove Monkee", {63: "H"}),
    ("groove_monkee", {63: "H"}),
    ("Chilled Beat", {63: "H"}), ("Hip Hop", {63: "H"}), ("Half-Time Pop", {63: "H"}),
    ("Funk 1", {63: "H"}), ("Motown", {63: "H"}), ("Surf", {63: "H"}), ("Metal", {63: "H"}), ("DnB", {63: "H"}),
]

TICKS_PER_QUARTER = 48
MAX_PATTERN = 4 * 192            # éditeur d'Hydrogen : 4 mesures de 4/4 au plus

PART_ORDER = ["Count", "Intro", "Intro Fill", "Verse", "Verse Fill", "Pre-Chorus", "Chorus", "Chorus Fill",
              "Transition", "Bridge", "Bridge Fill", "Groove", "Fill", "Hits", "Outro", "Outro Fill"]


def part_of(path):
    """Partie d'après le dossier et le nom du fichier."""
    p = path.lower().replace("_", " ")
    name = os.path.basename(p)
    fill = "fill" in name
    jj = jj_category(path)
    if jj:                                       # JJ : la catégorie est le dossier
        return {"Fill": "Fill", "Intro": "Intro"}.get(jj, "Groove")
    if re.search(r"\bcounts?\b", p):
        return "Count"
    if re.search(r"\bhits?\b", name) or ("hits" in p and "intro" not in name):
        return "Hits"
    if "transition" in name:
        return "Transition"
    if "intro" in name or ("intro" in p and fill):
        return "Intro Fill" if fill else "Intro"
    if "outro" in name or "ending" in name or "outro" in p:
        return "Outro Fill" if fill else "Outro"
    if "pre chorus" in name or "pre-chorus" in name:
        return "Pre-Chorus"
    if "chorus" in name:
        return "Chorus Fill" if fill else "Chorus"
    if "verse" in name:
        return "Verse Fill" if fill else "Verse"
    if "bridge" in name:
        return "Bridge Fill" if fill else "Bridge"
    if fill or "fills" in p:
        return "Fill"
    return "Groove"


def jj_category(path):
    """Packs JJ : catégorie d'après le dossier (Straight, Backbeat, Grooves,
    Fills, Intro - Hits - Counts) ; None pour un autre pack."""
    parts = path.split("/")
    if "JJ Groove Packs" not in path or len(parts) < 5:
        return None
    folder = re.sub(r"^\d+\s*-\s*", "", parts[3]).lower()
    if "fill" in folder:
        return "Fill"
    if "intro" in folder or "hit" in folder or "count" in folder:
        return "Intro"
    if "backbeat" in folder:
        return "Backbeat"
    if "straight" in folder:
        return "Straight"
    return "Groove"


def bpm_of(path):
    """Tempo de référence d'après le chemin : « 190 - 210 BPM », « 170bpm », « 146 S03 »."""
    m = re.search(r"(\d{2,3})\s*-\s*(\d{2,3})\s*bpm", path, re.I)
    if m:
        return [int(m.group(1)), int(m.group(2))]
    m = re.search(r"(\d{2,3})\s*bpm", path, re.I)
    if m:
        return int(m.group(1))
    m = re.search(r"(?:^|/)(\d{3})\s", path)
    if m and 40 <= int(m.group(1)) <= 260:
        return int(m.group(1))
    return None


def note_map(path):
    nm = dict(GM)
    for key, over in PACKS:
        if key.lower() in path.lower():
            nm.update(over)
    return nm


def convert_file(data, path):
    """-> liste de segments (taille en ticks Hydrogen, notes [(pos, rôle, vél)], num, den), notes perdues."""
    m = midi_lib.read_midi(data)
    num, den = (m.time_sigs[0][1], m.time_sigs[0][2]) if m.time_sigs else (4, 4)
    nm = note_map(path)
    q = TICKS_PER_QUARTER / m.ppq
    notes, lost = [], {}
    for t, n, v in m.notes:
        if n in nm:
            if nm[n]:
                notes.append((round(t * q), nm[n], v / 127.0))
        else:
            lost[n] = lost.get(n, 0) + 1
    bar = round(TICKS_PER_QUARTER * 4 * num / den)
    beat = round(TICKS_PER_QUARTER * 4 / den)
    end = m.length * q
    last = max((p for p, _, _ in notes), default=0)
    # Longueur : arrondie à la mesure si on en est proche (fichiers qui
    # s'arrêtent un peu avant la barre), sinon au temps.
    size = max(end, last + 1)
    bars = size / bar
    if abs(bars - round(bars)) < 0.15 and round(bars) >= 1:
        size = round(bars) * bar
    else:
        size = max(beat, math.ceil((size - 0.1 * beat) / beat) * beat)
    # Découpe en morceaux de 4 mesures au plus (limite d'Hydrogen).
    chunk = max(bar, (MAX_PATTERN // bar) * bar)
    segs = []
    for start in range(0, int(size), chunk):
        length = min(chunk, int(size) - start)
        segs.append((length, [(p - start, r, v) for p, r, v in notes if start <= p < start + length], num, den))
    return segs, lost


def clean(s):
    s = s.replace("&", "n")
    s = re.sub(r"[^A-Za-z0-9 ._-]", "", s)
    return re.sub(r"\s+", " ", s).strip(" .") or "x"


# Dossier de style (liste Song de DLS Drum) d'après le genre du pack.
STYLE_OF = {
    "ballads": "Ballad", "blues classics": "Blues", "classic rock": "Rock", "contemporary rock": "Rock",
    "country outlaws": "Country", "doom": "Metal", "garage rock": "Rock", "hi-octane": "Rock",
    "rock essentials 1": "Rock", "rock essentials 2": "Rock", "southern rock": "Rock",
    "chilled beat": "Hip Hop", "hip hop": "Hip Hop", "half-time pop": "Pop", "funk": "Funk",
    "motown": "Pop", "surf": "Surf", "metal": "Metal", "dnb": "Electro",
}


def style_folder(genre):
    g = genre.lower()
    for k, v in STYLE_OF.items():
        if k in g:
            return v
    return "Divers"


def group_key(zip_name, path):
    """(dossier de style, nom de la chanson) pour un fichier du zip. Le nom
    commence par la source (JJ, GM = Groove Monkee, Pack) pour s'y retrouver."""
    parts = path.split("/")
    low = path.lower()
    if "jj groove packs" in low:
        pack = parts[1].replace(" (JJ Groove Packs)", "").replace(" Vol. 1", "")
        rng = re.sub(r"^\d+\s*-\s*", "", parts[2]).replace(" BPM", "").replace(" - ", "-")
        return "Metal", f"JJ {pack} {rng}"
    if "groove monkee free midi pd" in low:
        return style_folder(parts[1]), f"GM {parts[1]} " + re.sub(r"^Song\s+", "S", parts[2])
    if zip_name.lower().startswith("groove_monkee_"):
        name = re.sub(r"^\d+\s+", "", parts[0])
        return style_folder(name), f"GM {name}"
    if "punk" in low:
        b = bpm_of(os.path.basename(path))
        return "Punk", f"Pop Punk {b}" if b else "Modern Pop Punk"
    if "rock" in low:
        b = bpm_of(os.path.basename(path))
        return "Rock", f"Alt Rock {b}" if b else "Alt Rock"
    return "Divers", os.path.splitext(zip_name)[0]


def skip(path):
    low = path.lower()
    return (not low.endswith(".mid") or "__macosx" in low or ".lib/" in low or "/._" in low
            or "full song" in low)


def pattern_name(path, part, k):
    base = os.path.splitext(os.path.basename(path))[0]
    jj = jj_category(path)
    if jj:                                       # « 7-4 - Groove 03 » -> « Straight 7-4 03 »
        meter = re.match(r"(\d+-\d+)", base)
        num = re.search(r"(\d+)\s*$", base)
        base = jj + (" " + meter.group(1) if meter and meter.group(1) != "4-4" else "") + (" " + num.group(1) if num else "")
    base = re.sub(r"^\d{2,3}\s*(bpm)?\s*", "", base, flags=re.I)        # tempo en tête
    base = re.sub(r"^S\d{2}\s+", "", base)
    return clean(base)[:40] + (f" ({k})" if k else "")


def build_song(template, roles, style, title, items):
    ids = gen.instruments(template)
    pats, index = [], []
    for g, it in enumerate(items, 1):
        notes_xml = []
        for pos, role, vel in sorted(it["notes"]):
            names = roles.get(role, [])
            inst = next((ids[x] for x in names if x in ids), None)
            if inst is not None:
                notes_xml.append(gen.note_xml(pos, round(vel, 3), inst))
        pats.append(f"""  <pattern>
   <name>{escape(it['name'])}</name>
   <info>{escape(it['src'])}</info>
   <category>{escape(it['part'])}</category>
   <size>{it['size']}</size>
   <denominator>{it['den']}</denominator>
   <noteList>
{''.join(notes_xml)}   </noteList>
  </pattern>
""")
        index.append({"groove": g, "part": it["part"], "name": it["name"],
                      "bars": round(it["size"] / (TICKS_PER_QUARTER * 4 * it["num"] / it["den"]), 2),
                      "time": f"{it['num']}/{it['den']}", "source": it["src"]})
    a, b = template.find("<patternList>"), template.find("</patternList>")
    s, e = template.find("<patternSequence>"), template.find("</patternSequence>")
    first = escape(items[0]["name"])
    out = (template[:a] + "<patternList>\n" + "".join(pats) + " " + template[b:s] +
           f"<patternSequence>\n  <group>\n   <patternID>{first}</patternID>\n  </group>\n " + template[e:])
    out = re.sub(r"<name>[^<]*</name>", f"<name>{escape(style + ' - ' + title)}</name>", out, count=1)
    out = re.sub(r"<mode>[^<]*</mode>", "<mode>pattern</mode>", out, count=1)
    return out, index


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    template = open(sys.argv[1], encoding="utf-8").read()
    _, roles = rk.read_kit(sys.argv[2])
    dst = sys.argv[3]
    os.makedirs(dst, exist_ok=True)
    groups = {}
    lost_total = {}
    for zpath in sys.argv[4:]:
        zf = zipfile.ZipFile(zpath)
        for info in zf.infolist():
            path = info.filename
            if info.is_dir() or skip(path):
                continue
            segs, lost = convert_file(zf.read(info), path)
            for n, c in lost.items():
                lost_total[n] = lost_total.get(n, 0) + c
            key = group_key(os.path.basename(zpath), path)
            part = part_of(path)
            for k, (size, notes, num, den) in enumerate(segs, 1):
                groups.setdefault(key, []).append({
                    "part": part, "src": path, "size": size, "notes": notes, "num": num, "den": den,
                    "name": pattern_name(path, part, k if len(segs) > 1 else 0), "bpm": bpm_of(path)})
    total = 0
    for (style, title), items in sorted(groups.items()):
        items.sort(key=lambda it: (PART_ORDER.index(it["part"]), it["name"]))
        # Noms uniques (Hydrogen repère les patterns par leur nom).
        seen = {}
        for it in items:
            n = it["name"]
            seen[n] = seen.get(n, 0) + 1
            if seen[n] > 1:
                it["name"] = f"{n} #{seen[n]}"
        song, index = build_song(template, roles, style, title, items)
        bpms = [b for it in items for b in (it["bpm"] if isinstance(it["bpm"], list) else [it["bpm"]]) if b]
        fname = clean(title)
        os.makedirs(os.path.join(dst, style), exist_ok=True)
        with open(os.path.join(dst, style, fname + ".h2song"), "w", encoding="utf-8", newline="") as f:
            f.write(song)
        meta = {"style": style, "title": title, "bpm_ref": [min(bpms), max(bpms)] if bpms else None,
                "grooves": index}
        with open(os.path.join(dst, style, fname + ".parts.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=1)
        parts = {}
        for it in items:
            parts[it["part"]] = parts.get(it["part"], 0) + 1
        total += len(items)
        print(f"  {style + '/' + fname:55s} {len(items):4d} grooves  " + ", ".join(f"{p} {c}" for p, c in parts.items()))
    print(f"{len(groups)} chansons, {total} grooves")
    if lost_total:
        print("notes ignorées (sans rôle) :", ", ".join(f"{n}×{c}" for n, c in sorted(lost_total.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
