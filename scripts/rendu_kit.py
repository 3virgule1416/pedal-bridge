#!/usr/bin/env python3
"""Rendu d'un morceau sur un kit : ce qu'appelle le pont quand on choisit un
Kit et une Song dans DLS Drum.

    python3 rendu_kit.py MORCEAU.h2song KIT.kit SORTIE.h2song [--kits DOSSIER]

- Style DLS (repère « dls-style: Nom » dans le morceau, écrit par
  gen_rythmes.py) : réécrit depuis ses grilles avec le kit (rôles exacts).
- Autre morceau : converti note par note (retarget_kit.py).
DOSSIER : où trouver les .kit (pour reconnaître le kit d'origine d'un
morceau) ; par défaut, celui de KIT.kit.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    args = sys.argv[1:]
    kits_dir = None
    if "--kits" in args:
        i = args.index("--kits")
        kits_dir = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3:
        print(__doc__)
        return 2
    song_path, kit_path, out_path = args
    kits_dir = kits_dir or os.path.dirname(os.path.abspath(kit_path))
    song = open(song_path, encoding="utf-8").read()
    rk = load("retarget_kit")
    info, roles = rk.read_kit(kit_path)
    if not info.get("MODELE"):
        raise SystemExit(f"{kit_path} : ligne « MODELE = … » manquante")
    template = open(info["MODELE"], encoding="utf-8").read()
    m = re.search(r"<notes>dls-style: ([^<]+)</notes>", song)
    gr = load("gen_rythmes")
    if m and m.group(1) in gr.STYLES:
        style = m.group(1)
        out = gr.build(template, style, gr.STYLES[style], roles)
        msg = f"ok {os.path.basename(out_path)} (style DLS {style}, kit {info.get('KIT', '?')})"
    else:
        src_kit = rk.find_kit_file(kits_dir, rk.drumkit_name(song))
        if not src_kit:
            raise SystemExit(f"kit d'origine « {rk.drumkit_name(song)} » : aucun .kit dans {kits_dir}")
        _, src_roles = rk.read_kit(src_kit)
        out, lost, lost_names = rk.retarget(song, src_roles, roles, template)
        msg = f"ok {os.path.basename(out_path)} ({rk.drumkit_name(song)} -> {info.get('KIT', '?')})"
        if lost:
            msg += f" ; {lost} note(s) sans équivalent ({', '.join(lost_names)})"
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    os.replace(tmp, out_path)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
