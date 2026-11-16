#!/usr/bin/env python3
"""Convertit les patterns « swing tassés » d'un morceau Hydrogen en vraies
mesures de 4/4.

Certains morceaux (ex. Jazz, Shuffle, Swing de la collection RP Bardet)
écrivent une mesure de 4/4 swing dans 144 ticks : temps aux positions 0, 36,
72 et 108 au lieu de 0, 48, 96 et 144 (Hydrogen compte 48 ticks par noire).
Joués tels quels, ils durent 3 noires : la grille du pont (4 temps) et les
boucles ne tombent pas juste. Ce script multiplie les positions (et
longueurs) par 4/3 et met la taille à 192 : le rythme ne change pas, il est
simplement écrit comme Hydrogen écrit normalement les triolets (16 ticks par
croche de triolet).

    python3 h2_swing44.py ENTRÉE.h2song SORTIE.h2song [--patterns 1,2,5]

Par défaut, tous les patterns de 144 ticks sont convertis. Ne pas l'utiliser
pour une vraie valse (3/4 : temps à 0, 48, 96). Le fichier d'entrée n'est
jamais modifié ; la sortie ne doit pas exister.
"""
import os
import re
import sys


def convert(text, only=None):
    a = text.find("<patternList>")
    b = text.find("</patternList>")
    if a < 0 or b < 0:
        raise SystemExit("pas de <patternList> : ce n'est pas un morceau Hydrogen")
    head, body, tail = text[:a], text[a:b], text[b:]
    parts = body.split("<pattern>")
    done = []
    for k in range(1, len(parts)):
        p = parts[k]
        num = k                                     # numéro de groove (à partir de 1)
        m = re.search(r"<size>(\d+)</size>", p)
        if not m or int(m.group(1)) != 144 or (only and num not in only):
            continue
        positions = [int(v) for v in re.findall(r"<position>(\d+)</position>", p)]
        bad = [v for v in positions if v % 3]
        if bad:
            raise SystemExit(f"pattern {num} : positions non divisibles par 3 ({bad[:5]}), conversion inexacte")
        p = p.replace(m.group(0), "<size>192</size>", 1)
        p = re.sub(r"<position>(\d+)</position>", lambda x: f"<position>{int(x.group(1)) * 4 // 3}</position>", p)
        p = re.sub(r"<length>(\d+)</length>", lambda x: f"<length>{int(x.group(1)) * 4 // 3}</length>", p)
        parts[k] = p
        name = re.search(r"<name>([^<]*)</name>", p)
        done.append(f"{num} ({name.group(1) if name else '?'})")
    return head + "<pattern>".join(parts) + tail, done


def main():
    args = sys.argv[1:]
    only = None
    if "--patterns" in args:
        i = args.index("--patterns")
        only = {int(v) for v in args[i + 1].split(",")}
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    src, dst = args
    if os.path.exists(dst):
        raise SystemExit(f"{dst} existe déjà : je ne l'écrase pas")
    with open(src, encoding="utf-8") as f:
        text = f.read()
    out, done = convert(text, only)
    with open(dst, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    print(f"{len(done)} pattern(s) convertis en 4/4 (192 ticks) : {', '.join(done) or 'aucun'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
