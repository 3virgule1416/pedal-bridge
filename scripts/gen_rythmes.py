#!/usr/bin/env python3
"""Bibliothèque de rythmes de Davis Loop Station : écrit des morceaux
Hydrogen (.h2song), un par style, à partir de grilles écrites à la main.

    python3 gen_rythmes.py MODÈLE.h2song DOSSIER_SORTIE [--kit FICHIER.kit] [--suffixe " - Kit"] [STYLE ...]

MODÈLE : un morceau Hydrogen qui utilise le kit voulu (sa liste
d'instruments est reprise telle quelle), par exemple
/usr/share/hydrogen/data/demo_songs/GM_kit_demo1.h2song pour GMRockKit.
Les fichiers « DLS <Style>.h2song » existants sont remplacés ; rien d'autre
n'est touché.

--kit : fichier de correspondance rôle -> instrument du kit (voir
scripts/kits/*.kit) ; par défaut, kits/GM Rock Kit.kit. Pour un autre kit, on
écrit son .kit (les noms de ses instruments) et on lui donne un modèle qui
l'utilise : les mêmes grilles jouent alors sur les bons instruments.

Chaque style a la même structure (comme une « chanson » de BeatBuddy) :

    Groove 1  Intro          Groove 5  Chorus fill
    Groove 2  Verse          Groove 6  Transition
    Groove 3  Verse fill     Groove 7  Ending
    Groove 4  Chorus

Grilles : une ligne par instrument, un caractère par pas. 16 pas par mesure
(doubles croches) ou 12 (croches en triolets : shuffle). Force de frappe :
X accent, x normal, o fantôme (ghost note), . rien.
Les rythmes sont écrits pour ce projet (idiomes de chaque style), pas copiés
d'un appareil.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read_kit(path):
    """Fichier .kit -> {rôle: [instrument, remplaçant, ...]}."""
    roles = {}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            raise SystemExit(f"{path}:{n} : « RÔLE = Instrument » attendu")
        role, names = line.split("=", 1)
        roles[role.strip()] = [x.strip() for x in names.split("|") if x.strip()]
    return roles
VELOCITY = {"X": 1.0, "x": 0.78, "o": 0.35}
PARTS = ["Intro", "Verse", "Verse fill", "Chorus", "Chorus fill", "Transition", "Ending"]

STYLES = {
    # ------------------------------------------------------------- Rock ---
    "Rock": {"bpm": 110, "parts": {
        "Intro": {
            "SC": "X...X...X...X...",
            "K":  "............X...",
            "S":  "............X.xx",
        },
        "Verse": {
            "H": "X.x.X.x.X.x.X.x.",
            "K": "X.......X.x.....",
            "S": "....X.......X...",
        },
        "Verse fill": {
            "H":  "X.x.X.x.X.x.....",
            "K":  "X.......X.......",
            "S":  "....X.......Xx..",
            "T1": "..............x.",
            "FT": "...............X",
        },
        "Chorus": {
            "C": "X...............",
            "Y": "X.x.X.x.X.x.X.x.",
            "K": "X.....x.X.x...x.",
            "S": "....X.......X...",
        },
        "Chorus fill": {
            "K":  "X.......X.......",
            "S":  "....X...XxXx....",
            "T1": "............Xx..",
            "FT": "..............XX",
        },
        "Transition": {
            "K": "X...X...X...X...",
            "S": "x.x.x.x.XxXxXXXX",
        },
        "Ending": {
            "C": "X...............",
            "K": "X...............",
            "S": "X...............",
        },
    }},
    # ------------------------------------------------------------- Funk ---
    "Funk": {"bpm": 100, "parts": {
        "Intro": {
            "H": "XoxoXoxoXoxoXoxo",
            "K": "X...............",
        },
        "Verse": {
            "H": "XoxoXoxoXoxoXoxo",
            "K": "X.x.......X..x..",
            "S": "....X.o..o..X..o",
        },
        "Verse fill": {
            "H": "XoxoXoxoXoxo....",
            "K": "X.x.......X.....",
            "S": "....X.o..o..XxXx",
        },
        "Chorus": {
            "H": "X...X...X...X...",
            "O": "..X...X...X...X.",
            "K": "X..x..x...X..x..",
            "S": "....X..o.o..X..o",
        },
        "Chorus fill": {
            "H":  "X.x.X.x.........",
            "K":  "X..x..x.........",
            "S":  "....X..oXxXx....",
            "T1": "............Xx..",
            "FT": "..............XX",
        },
        "Transition": {
            "H": "X.x.X.x.X.x.X.x.",
            "K": "X...X...X...X...",
            "S": "o.o.x.x.xxXxXXXX",
        },
        "Ending": {
            "C": "X...............",
            "K": "X...............",
            "S": "X...............",
        },
    }},
    # ---------------------------------------------- Shuffle (triolets) ---
    "Shuffle": {"bpm": 115, "parts": {
        "Intro": {
            "H": "X.xX.xX.xX.x",
            "K": "X...........",
        },
        "Verse": {
            "H": "X.xX.xX.xX.x",
            "K": "X.....X....x",
            "S": "...X.o...X.o",
        },
        "Verse fill": {
            "H": "X.xX.xX.x...",
            "K": "X.....X.....",
            "S": "...X.o...XXX",
        },
        "Chorus": {
            "C": "X...........",
            "Y": "X.xX.xX.xX.x",
            "K": "X..x..X..x..",
            "S": "...X.....X..",
        },
        "Chorus fill": {
            "K":  "X.....X.....",
            "S":  "...X..XXX...",
            "FT": ".........XXX",
        },
        "Transition": {
            "K": "X..X..X..X..",
            "S": "x.xx.xxxxXXX",
        },
        "Ending": {
            "C": "X...........",
            "K": "X...........",
            "S": "X...........",
        },
    }},
    # ------------------------------------------------------------- Punk ---
    "Punk": {"bpm": 180, "parts": {
        "Intro": {
            "SC": "X...X...X...X...",
        },
        "Verse": {
            "H": "X.X.X.X.X.X.X.X.",
            "K": "X.X.....X.X.....",
            "S": "....X.......X...",
        },
        "Verse fill": {
            "H": "X.X.X.X.X.X.....",
            "K": "X.X.....X.X.....",
            "S": "....X.......XxXx",
        },
        "Chorus": {
            "C": "X...............",
            "O": "..X...X...X...X.",
            "K": "X...X...X...X...",
            "S": "..X...X...X...X.",
        },
        "Chorus fill": {
            "K":  "X...X...........",
            "S":  "..X...X.XxXx....",
            "T1": "............Xx..",
            "FT": "..............XX",
        },
        "Transition": {
            "K": "X...X...X...X...",
            "S": "X.X.X.X.XxXxXXXX",
        },
        "Ending": {
            "C": "X...............",
            "K": "X...............",
            "S": "X...............",
        },
    }},
    # ------------------------------------------------------------ Metal ---
    "Metal": {"bpm": 160, "parts": {
        "Intro": {
            "C":  "X.......X.......",
            "K":  "X.......X.......",
            "S":  "X.......X.......",
            "T1": "............Xx..",
            "FT": "..............XX",
        },
        "Verse": {
            "H": "X.x.X.x.X.x.X.x.",
            "K": "X.XXX.XXX.XXX.XX",
            "S": "....X.......X...",
        },
        "Verse fill": {
            "H":  "X.x.X.x.X.x.....",
            "K":  "X.XXX.XXX.XX....",
            "S":  "....X.......Xx..",
            "T1": "..............x.",
            "FT": "...............X",
        },
        "Chorus": {
            "C": "X...X...X...X...",
            "K": "XxXxXxXxXxXxXxXx",
            "S": "....X.......X...",
        },
        "Chorus fill": {
            "K":  "XxXxXxXxXxXxXxXx",
            "S":  "XxXx............",
            "T1": "....XxXx........",
            "T2": "........XxXx....",
            "FT": "............XxXX",
        },
        "Transition": {
            "K": "XxXxXxXxXxXxXxXx",
            "S": "x.x.x.x.XxXxXXXX",
            "C": "...............X",
        },
        "Ending": {
            "C": "X...............",
            "K": "X...............",
            "S": "X...............",
        },
    }},
}


def instruments(template):
    """Nom -> id des instruments du modèle."""
    a, b = template.find("<instrumentList>"), template.find("</instrumentList>")
    if a < 0 or b < 0:
        raise SystemExit("modèle sans <instrumentList>")
    ids = {}
    for blk in template[a:b].split("<instrument>")[1:]:
        i = re.search(r"<id>(\d+)</id>", blk)
        n = re.search(r"<name>([^<]*)</name>", blk)
        if i and n:
            ids[n.group(1)] = int(i.group(1))
    return ids


def note_xml(pos, vel, inst):
    return f"""    <note>
     <position>{pos}</position>
     <leadlag>0</leadlag>
     <velocity>{vel:g}</velocity>
     <pan>0</pan>
     <pitch>0</pitch>
     <key>C0</key>
     <length>-1</length>
     <instrument>{inst}</instrument>
     <note_off>false</note_off>
     <probability>1</probability>
    </note>
"""


def pattern_xml(name, grid, ids, style, kit):
    lengths = {len(line) for line in grid.values()}
    if len(lengths) != 1:
        raise SystemExit(f"{style} / {name} : lignes de longueurs différentes {sorted(lengths)}")
    steps = lengths.pop()
    if steps not in (12, 16):
        raise SystemExit(f"{style} / {name} : {steps} pas (12 ou 16 attendus)")
    tick = 192 // steps                               # 48 ticks par noire, 1 mesure
    notes = []
    for role, line in grid.items():
        names = kit.get(role)
        if not names:
            raise SystemExit(f"rôle « {role} » absent du fichier .kit ({style} / {name})")
        inst = next((ids[x] for x in names if x in ids), None)
        if inst is None:
            raise SystemExit(f"le kit n'a aucun de ces instruments : {', '.join(names)} ({style} / {name})")
        for k, ch in enumerate(line):
            if ch in VELOCITY:
                notes.append((k * tick, inst, VELOCITY[ch]))
            elif ch != ".":
                raise SystemExit(f"{style} / {name} / {role} : caractère « {ch} » inconnu")
    notes.sort()
    body = "".join(note_xml(p, v, i) for p, i, v in notes)
    return f"""  <pattern>
   <name>{name}</name>
   <info></info>
   <category>DLS</category>
   <size>192</size>
   <denominator>4</denominator>
   <noteList>
{body}   </noteList>
  </pattern>
"""


def build(template, style, spec, kit):
    ids = instruments(template)
    pats = "".join(pattern_xml(part, spec["parts"][part], ids, style, kit) for part in PARTS)
    a = template.find("<patternList>")
    b = template.find("</patternList>")
    s = template.find("<patternSequence>")
    e = template.find("</patternSequence>")
    if min(a, b, s, e) < 0:
        raise SystemExit("modèle sans <patternList> ou <patternSequence>")
    out = (template[:a] + "<patternList>\n" + pats + " " + template[b:s] +
           "<patternSequence>\n  <group>\n   <patternID>Intro</patternID>\n  </group>\n " + template[e:])
    out = re.sub(r"<bpm>[^<]*</bpm>", f"<bpm>{spec['bpm']}</bpm>", out, count=1)
    out = re.sub(r"<name>[^<]*</name>", f"<name>DLS {style}</name>", out, count=1)
    out = re.sub(r"<mode>[^<]*</mode>", "<mode>pattern</mode>", out, count=1)
    # Repère pour rendu_kit.py : un style DLS se réécrit depuis ses grilles
    # (rôles exacts) au lieu d'être converti note par note.
    out = re.sub(r"<notes>[^<]*</notes>", f"<notes>dls-style: {style}</notes>", out, count=1)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    args = sys.argv[1:]
    kit_path = os.path.join(HERE, "kits", "GM Rock Kit.kit")
    if "--kit" in args:
        i = args.index("--kit")
        kit_path = args[i + 1]
        del args[i:i + 2]
    suffix = ""
    if "--suffixe" in args:
        i = args.index("--suffixe")
        suffix = args[i + 1]
        del args[i:i + 2]
    kit = read_kit(kit_path)
    template = open(args[0], encoding="utf-8").read()
    dst = args[1]
    wanted = args[2:] or list(STYLES)
    os.makedirs(dst, exist_ok=True)
    for style in wanted:
        if style not in STYLES:
            raise SystemExit(f"style inconnu : {style} (connus : {', '.join(STYLES)})")
        path = os.path.join(dst, f"DLS {style}{suffix}.h2song")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(build(template, style, STYLES[style], kit))
        print(f"  ok  {path} ({len(PARTS)} grooves, {STYLES[style]['bpm']} BPM conseillés)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
