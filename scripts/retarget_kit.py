#!/usr/bin/env python3
"""Fait jouer un morceau Hydrogen (.h2song) sur un autre kit de batterie.

    python3 retarget_kit.py MORCEAU.h2song KIT_CIBLE.kit SORTIE.h2song [--kits DOSSIER]

Chaque note du morceau est sur un instrument du kit d'origine. Le fichier
.kit d'origine (trouvé dans DOSSIER, par sa ligne « KIT = <nom du kit> »)
dit quel RÔLE joue cet instrument (grosse caisse, caisse claire, hi-hat…) ;
le .kit cible dit quel instrument joue ce rôle dans le nouveau kit. La liste
d'instruments du morceau est remplacée par celle du modèle du kit cible
(ligne « MODELE = … » du .kit cible).

Lignes spéciales d'un .kit :
    KIT    = nom du kit dans Hydrogen (ex. GMRockKit)
    MODELE = morceau Hydrogen qui utilise ce kit (sa liste d'instruments)
Les autres lignes : RÔLE = Instrument | Remplaçant | ...
"""
import os
import re
import sys

SPECIAL = ("KIT", "MODELE")


def read_kit(path):
    """-> (infos {KIT, MODELE}, rôles {rôle: [noms]})."""
    info, roles = {}, {}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            raise SystemExit(f"{path}:{n} : « CLÉ = valeur » attendu")
        k, v = (x.strip() for x in line.split("=", 1))
        if k in SPECIAL:
            info[k] = os.path.expanduser(v)
        else:
            roles[k] = [x.strip() for x in v.split("|") if x.strip()]
    return info, roles


def instruments(text):
    """Liste d'instruments d'un .h2song : (id, nom), dans l'ordre."""
    a, b = text.find("<instrumentList>"), text.find("</instrumentList>")
    if a < 0 or b < 0:
        raise SystemExit("pas de <instrumentList>")
    out = []
    for blk in text[a:b].split("<instrument>")[1:]:
        i = re.search(r"<id>(-?\d+)</id>", blk)
        n = re.search(r"<name>([^<]*)</name>", blk)
        if i and n:
            out.append((int(i.group(1)), n.group(1)))
    return out


def drumkit_name(text):
    m = re.search(r"<last_loaded_drumkit_name>([^<]*)</last_loaded_drumkit_name>", text)
    return m.group(1) if m else ""


def find_kit_file(kits_dir, name):
    for f in sorted(os.listdir(kits_dir)):
        if f.endswith(".kit"):
            info, _ = read_kit(os.path.join(kits_dir, f))
            if info.get("KIT") == name:
                return os.path.join(kits_dir, f)
    return None


def retarget(song, src_roles, dst_roles, template):
    """Morceau -> même morceau sur le kit du modèle. Retourne (texte, notes perdues)."""
    src = dict(instruments(song))                       # id -> nom
    dst = {name: iid for iid, name in instruments(template)}
    # Instrument d'origine -> rôle : rôle dont il est le PREMIER choix, sinon
    # le premier rôle qui le cite comme remplaçant.
    role_of = {}
    for role, names in src_roles.items():
        if names and names[0] not in role_of:
            role_of[names[0]] = role
    for role, names in src_roles.items():
        for n in names[1:]:
            role_of.setdefault(n, role)
    id_map, lost_names = {}, set()
    for iid, name in src.items():
        role = role_of.get(name)
        target = next((dst[n] for n in dst_roles.get(role, []) if n in dst), None) if role else None
        if target is None:
            lost_names.add(name)
        else:
            id_map[iid] = target

    a, b = song.find("<patternList>"), song.find("</patternList>")
    body = song[a:b]
    lost = 0

    def note(m):
        nonlocal lost
        n = m.group(0)
        iid = int(re.search(r"<instrument>(-?\d+)</instrument>", n).group(1))
        if iid not in id_map:
            lost += 1
            return ""
        return re.sub(r"<instrument>-?\d+</instrument>", f"<instrument>{id_map[iid]}</instrument>", n)

    body = re.sub(r"<note>.*?</note>\s*", note, body, flags=re.S)
    out = song[:a] + body + song[b:]
    # Liste d'instruments et kit : ceux du modèle.
    ta, tb = template.find("<instrumentList>"), template.find("</instrumentList>")
    oa, ob = out.find("<instrumentList>"), out.find("</instrumentList>")
    out = out[:oa] + template[ta:tb] + out[ob:]
    ca, cb = template.find("<componentList>"), template.find("</componentList>")
    oa, ob = out.find("<componentList>"), out.find("</componentList>")
    if min(ca, cb, oa, ob) >= 0:
        out = out[:oa] + template[ca:cb] + out[ob:]
    for tag in ("last_loaded_drumkit", "last_loaded_drumkit_name"):
        m = re.search(rf"<{tag}>[^<]*</{tag}>", template)
        if m:
            out = re.sub(rf"<{tag}>[^<]*</{tag}>", m.group(0).replace("\\", "\\\\"), out, count=1)
    return out, lost, sorted(lost_names)


def main():
    args = sys.argv[1:]
    kits_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kits")
    if "--kits" in args:
        i = args.index("--kits")
        kits_dir = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3:
        print(__doc__)
        return 2
    song_path, dst_kit_path, out_path = args
    song = open(song_path, encoding="utf-8").read()
    src_name = drumkit_name(song)
    src_kit_path = find_kit_file(kits_dir, src_name)
    if not src_kit_path:
        raise SystemExit(f"aucun .kit avec « KIT = {src_name} » dans {kits_dir}")
    _, src_roles = read_kit(src_kit_path)
    dst_info, dst_roles = read_kit(dst_kit_path)
    if not dst_info.get("MODELE"):
        raise SystemExit(f"{dst_kit_path} : ligne « MODELE = … » manquante")
    template = open(dst_info["MODELE"], encoding="utf-8").read()
    out, lost, lost_names = retarget(song, src_roles, dst_roles, template)
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    os.replace(tmp, out_path)
    msg = f"ok {os.path.basename(out_path)} ({src_name} -> {dst_info.get('KIT', '?')})"
    if lost:
        msg += f" ; {lost} note(s) sans équivalent ({', '.join(lost_names)})"
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
