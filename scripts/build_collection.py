#!/usr/bin/env python3
"""Bâtit la collection de batterie de DLS à partir d'un zip de morceaux
Hydrogen (.h2song), par exemple « 260 Drum Machine Patterns from RP Bardet ».

    python3 build_collection.py ZIP DOSSIER_SORTIE

- Un morceau = un style ; ses patterns = les grooves (Groove 1, 2, …).
- Morceaux sans aucune note (ex. DefaultSong) : laissés de côté.
- Morceaux en 4/4 swing « tassé » dans 144 ticks (voir h2_swing44.py) :
  convertis en vraies mesures de 4/4. La liste est explicite (SWING_TASSE) :
  une vraie valse (3/4) a aussi 144 ticks et ne doit PAS être convertie.
- Le zip n'est jamais extrait tel quel : chaque morceau est lu en mémoire et
  réécrit dans DOSSIER_SORTIE (qui ne doit pas exister).
"""
import importlib.util
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("h2_swing44", os.path.join(HERE, "h2_swing44.py"))
h2_swing44 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(h2_swing44)

# Morceaux RP Bardet écrits en 4/4 swing dans 144 ticks (temps à 0, 36, 72, 108).
SWING_TASSE = {"Jazz", "Shuffle", "Swing", "Reggae"}


def patterns(text):
    a, b = text.find("<patternList>"), text.find("</patternList>")
    if a < 0 or b < 0:
        return []
    out = []
    for p in text[a:b].split("<pattern>")[1:]:
        name = re.search(r"<name>([^<]*)</name>", p)
        size = re.search(r"<size>(\d+)</size>", p)
        out.append((name.group(1) if name else "?", int(size.group(1)) if size else 0, p.count("<note>")))
    return out


def safe_name(stem):
    """Nom de fichier simple (lettres, chiffres, espace, - et _)."""
    s = stem.replace("&", "n")             # R&B -> RnB
    s = re.sub(r"[^A-Za-z0-9 _-]", "", s).strip()
    return s or "morceau"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.exists(dst):
        raise SystemExit(f"{dst} existe déjà : je ne l'écrase pas")
    z = zipfile.ZipFile(src)
    entries = sorted(n for n in z.namelist() if n.lower().endswith(".h2song") and not n.endswith("/"))
    os.makedirs(dst)
    kept = 0
    for n in entries:
        stem = os.path.splitext(os.path.basename(n))[0]
        text = z.read(n).decode("utf-8")
        pats = patterns(text)
        if not pats or all(notes == 0 for _, _, notes in pats):
            print(f"  ignoré  {stem} (aucune note)")
            continue
        note = ""
        if stem in SWING_TASSE:
            text, done = h2_swing44.convert(text)
            note = f", {len(done)} pattern(s) swing convertis en 4/4"
            pats = patterns(text)
        beats = sorted({4 if s % 192 == 0 else 3 if s % 144 == 0 else 0 for _, s, _ in pats})
        out = os.path.join(dst, safe_name(stem) + ".h2song")
        with open(out, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        kept += 1
        what = "/".join({4: "4 temps", 3: "3 temps", 0: "autre"}[b] for b in beats)
        print(f"  ok      {safe_name(stem)} : {len(pats)} groove(s), {what}{note}")
    print(f"{kept} morceau(x) dans {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
