#!/usr/bin/env python3
"""Réunit toutes les chansons de la batterie (dossier DLS Drums et ses
sous-dossiers de style) dans UN seul morceau Hydrogen, pour un kit : changer
de chanson devient alors un saut vers d'autres patterns, sans recharger
(Hydrogen arrête la lecture quand il ouvre un morceau).

    python3 gros_morceau.py DOSSIER_CHANSONS KIT.kit SORTIE.h2song

Écrit aussi SORTIE.index.json : pour chaque chanson (chemin relatif au
dossier), le numéro de son premier pattern dans le gros morceau (à partir
de 0), son nombre de patterns et leurs tailles en ticks.
"""
import importlib.util
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rk = load("retarget_kit")
gr = load("gen_rythmes")


def render(song, roles, template, kits_dir):
    """Même rendu que rendu_kit.py, en mémoire."""
    m = re.search(r"<notes>dls-style: ([^<]+)</notes>", song)
    if m and m.group(1) in gr.STYLES:
        return gr.build(template, m.group(1), gr.STYLES[m.group(1)], roles)
    src_kit = rk.find_kit_file(kits_dir, rk.drumkit_name(song))
    if not src_kit:
        raise ValueError(f"kit d'origine « {rk.drumkit_name(song)} » inconnu")
    _, src_roles = rk.read_kit(src_kit)
    out, _, _ = rk.retarget(song, src_roles, roles, template)
    return out


def patterns(text):
    a, b = text.find("<patternList>"), text.find("</patternList>")
    body = text[a + len("<patternList>"):b]
    return ["<pattern>" + p[:p.rfind("</pattern>") + len("</pattern>")] for p in body.split("<pattern>")[1:]]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    songs_dir, kit_path, out_path = sys.argv[1:]
    kits_dir = os.path.dirname(os.path.abspath(kit_path))
    info, roles = rk.read_kit(kit_path)
    template = open(info["MODELE"], encoding="utf-8").read()
    t0 = time.time()
    all_pats, index = [], {}
    files = []
    for root, _, names in os.walk(songs_dir):
        for n in names:
            if n.endswith(".h2song"):
                files.append(os.path.join(root, n))
    for k, path in enumerate(sorted(files)):
        rel = os.path.relpath(path, songs_dir).replace(os.sep, "/")
        try:
            pats = patterns(render(open(path, encoding="utf-8").read(), roles, template, kits_dir))
        except Exception as e:                      # chanson illisible : laissée de côté
            print(f"  ignoré {rel} : {e}")
            continue
        sizes = []
        for j, p in enumerate(pats):
            # Noms uniques dans le gros morceau : préfixe de la chanson.
            p = re.sub(r"<name>([^<]*)</name>", lambda m: f"<name>C{k:03d} {m.group(1)}</name>", p, count=1)
            sizes.append(int(re.search(r"<size>(\d+)</size>", p).group(1)))
            all_pats.append(p)
        index[rel] = {"first": len(all_pats) - len(pats), "count": len(pats), "sizes": sizes}
    a, b = template.find("<patternList>"), template.find("</patternList>")
    s, e = template.find("<patternSequence>"), template.find("</patternSequence>")
    first = re.search(r"<name>([^<]*)</name>", all_pats[0]).group(1)
    out = (template[:a] + "<patternList>\n  " + "\n  ".join(all_pats) + "\n " + template[b:s] +
           f"<patternSequence>\n  <group>\n   <patternID>{first}</patternID>\n  </group>\n " + template[e:])
    out = re.sub(r"<mode>[^<]*</mode>", "<mode>pattern</mode>", out, count=1)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    with open(out_path + ".index.json", "w", encoding="utf-8") as f:
        json.dump({"kit": info.get("KIT"), "songs": index}, f, ensure_ascii=False)
    print(f"{out_path} : {len(index)} chansons, {len(all_pats)} patterns, "
          f"{os.path.getsize(out_path) / 1e6:.1f} Mo, {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
