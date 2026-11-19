#!/usr/bin/env python3
"""Réunit toutes les chansons de la batterie (dossier DLS Drums et ses
sous-dossiers de style) dans UN seul morceau Hydrogen, pour un kit : changer
de chanson devient alors un saut vers d'autres patterns, sans recharger
(Hydrogen arrête la lecture quand il ouvre un morceau).

    python3 gros_morceau.py DOSSIER_CHANSONS KIT.kit SORTIE.h2song

Écrit aussi SORTIE.index.json : pour chaque chanson (chemin relatif au
dossier), le numéro de son premier pattern dans le gros morceau (à partir
de 0), son nombre de patterns et leurs tailles en ticks ; et SORTIE.index.txt,
le même en lignes « premier<TAB>nombre<TAB>chemin » (lu par le pont).
Le fichier est d'abord écrit à côté puis renommé : un gros morceau à moitié
écrit n'est jamais ouvert.
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


# Réglages de note égaux à la valeur qu'Hydrogen met quand ils manquent
# (Note::load_from, Hydrogen 1.2 : leadlag 0, pitch 0, key C0, length -1,
# note_off false, probability 1) : retirés, le son est identique. « pan » est
# gardé : absent, Hydrogen cherche l'ancien format et écrit un avertissement.
# Mesuré : gros morceau 27 -> 9 Mo, chargement 4,1 -> 2,5 s.
DEFAULTS = re.compile(r"\s*<(?:leadlag>0</leadlag|pitch>0</pitch|key>C0</key|length>-1</length"
                      r"|note_off>false</note_off|probability>1</probability)>")


def lighten(pattern):
    p = DEFAULTS.sub("", pattern)
    return re.sub(r"\n\s+", "\n", p)


def patterns(text):
    a, b = text.find("<patternList>"), text.find("</patternList>")
    body = text[a + len("<patternList>"):b]
    return [lighten("<pattern>" + p[:p.rfind("</pattern>") + len("</pattern>")]) for p in body.split("<pattern>")[1:]]


# --- Structure des parties (à la BeatBuddy) ---------------------------------
# Rôle de chaque pattern d'après sa catégorie (midi2h2 : la partie) ou son nom
# (styles DLS : Intro, Verse, Verse fill… ; RP Bardet : Pattern N, Break N).
PARTS = ["Verse", "Pre-Chorus", "Chorus", "Bridge", "Groove"]          # parties jouables, dans l'ordre


def role_of(pattern):
    name = (re.search(r"<name>([^<]*)</name>", pattern) or [None, ""])[1]
    cat = (re.search(r"<category>([^<]*)</category>", pattern) or [None, ""])[1]
    if cat in ("Count", "Intro", "Intro Fill", "Verse", "Verse Fill", "Pre-Chorus", "Chorus", "Chorus Fill",
               "Transition", "Bridge", "Bridge Fill", "Groove", "Fill", "Hits", "Outro", "Outro Fill"):
        return cat
    n = name.lower()
    if re.match(r"break\s*\d*", n):                       # RP Bardet
        return "Fill"
    if re.match(r"pattern\s*\d*", n):
        return "Groove"
    for key, role in (("verse fill", "Verse Fill"), ("chorus fill", "Chorus Fill"), ("intro", "Intro"),
                      ("verse", "Verse"), ("chorus", "Chorus"), ("transition", "Transition"),
                      ("ending", "Outro"), ("outro", "Outro"), ("fill", "Fill")):
        if key in n:
            return role
    return "Groove"


def phrases(nums, pats):
    """Regroupe les moitiés d'une phrase coupée à 4 mesures (« Verse (1) »,
    « Verse (2) »…, consécutives) : -> [[4, 5], [6, 7], [8]]."""
    out, prev = [], None
    for j in nums:
        name = (re.search(r"<name>([^<]*)</name>", pats[j - 1]) or [None, ""])[1]
        m = re.match(r"^(.*) \((\d+)\)$", name)
        base, k = (m.group(1), int(m.group(2))) if m else (None, 0)
        if base and prev and prev[0] == base and k == prev[1] + 1 and out and out[-1][-1] == j - 1:
            out[-1].append(j)
        else:
            out.append([j])
        prev = (base, k) if base else None
    return out


def structure(pats):
    """-> {clé: [variations]}, une variation = [numéros de pattern enchaînés].
    Clés : part:<Partie>, fill:<Partie> (aussi fill:Intro), fill:* (fills
    communs), intro, outro (phrases de fin + fills de fin), transition, hits.
    Numéros dans la chanson, à partir de 1."""
    st = {}
    for j, p in enumerate(pats, 1):
        r = role_of(p)
        if r in PARTS:
            key = f"part:{r}"
        elif r.endswith(" Fill") and r[:-5] in PARTS:
            key = f"fill:{r[:-5]}"
        elif r == "Fill":
            key = "fill:*"
        elif r in ("Intro", "Count"):
            key = "intro"
        elif r == "Intro Fill":
            key = "fill:Intro"
        elif r == "Outro":
            key = "outro"
        elif r == "Outro Fill":
            key = "fill:Outro"
        else:
            key = r.lower()                                   # transition, hits
        st.setdefault(key, []).append(j)
    if not any(k.startswith("part:") for k in st):           # rien de jouable : tout devient groove
        st = {"part:Groove": list(range(1, len(pats) + 1))}
    out = {}
    for key, nums in st.items():
        if key.startswith("part:") or key in ("intro", "outro"):
            out[key] = phrases(nums, pats)                    # phrases enchaînées
        elif key != "fill:Outro":
            out[key] = [[n] for n in nums]                    # un pattern par variation
    if "fill:Outro" in st:                                    # fins « fill » : aussi des fins
        out.setdefault("outro", []).extend([[n] for n in st["fill:Outro"]])
    return out


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
        index[rel] = {"first": len(all_pats) - len(pats), "count": len(pats), "sizes": sizes,
                      "parts": structure(pats)}
    silence = len(all_pats)
    all_pats.append("<pattern>\n<name>Silence</name>\n<info></info>\n<category>DLS</category>\n"
                    "<size>192</size>\n<denominator>4</denominator>\n<noteList>\n</noteList>\n</pattern>")
    # Coup final (End d'une chanson sans outro, après un fill) : grosse caisse et
    # crash(s) sur le 1, une mesure pour laisser sonner.
    ids = {name: i for i, name in rk.instruments(template)}
    hit = []
    for role in ("K", "C", "C2"):
        inst = next((ids[n] for n in roles.get(role, []) if n in ids), None)
        if inst is not None and inst not in hit:
            hit.append(inst)
    final = len(all_pats)
    all_pats.append("<pattern>\n<name>Final hit</name>\n<info></info>\n<category>DLS</category>\n"
                    "<size>192</size>\n<denominator>4</denominator>\n<noteList>\n" +
                    "".join(f"<note>\n<position>0</position>\n<velocity>1</velocity>\n<pan>0</pan>\n"
                            f"<instrument>{i}</instrument>\n</note>\n" for i in hit) +
                    "</noteList>\n</pattern>")
    a, b = template.find("<patternList>"), template.find("</patternList>")
    s, e = template.find("<patternSequence>"), template.find("</patternSequence>")
    first = re.search(r"<name>([^<]*)</name>", all_pats[0]).group(1)
    out = (template[:a] + "<patternList>\n  " + "\n  ".join(all_pats) + "\n " + template[b:s] +
           f"<patternSequence>\n  <group>\n   <patternID>{first}</patternID>\n  </group>\n " + template[e:])
    out = re.sub(r"<mode>[^<]*</mode>", "<mode>pattern</mode>", out, count=1)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path + ".tmp", "w", encoding="utf-8", newline="") as f:
        f.write(out)
    with open(out_path + ".index.json", "w", encoding="utf-8") as f:
        json.dump({"kit": info.get("KIT"), "songs": index}, f, ensure_ascii=False)
    # Même index, simple à lire pour le pont : « premier<TAB>nombre<TAB>chemin ».
    with open(out_path + ".index.txt", "w", encoding="utf-8", newline="\n") as f:
        for rel, e in index.items():
            f.write(f"{e['first']}\t{e['count']}\t{rel}\n")
        f.write(f"{silence}\t1\t@silence\n")
        f.write(f"{final}\t1\t@final\n")
    # Structure des parties, pour le pont : « chemin<TAB>clé<TAB>1,2,3 ».
    with open(out_path + ".parts.txt", "w", encoding="utf-8", newline="\n") as f:
        for rel, e in index.items():
            for key, variations in e["parts"].items():
                f.write(f"{rel}\t{key}\t{','.join('+'.join(map(str, v)) for v in variations)}\n")
    os.replace(out_path + ".tmp", out_path)               # le morceau en dernier : l'index est prêt
    print(f"{out_path} : {len(index)} chansons, {len(all_pats)} patterns, "
          f"{os.path.getsize(out_path) / 1e6:.1f} Mo, {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
