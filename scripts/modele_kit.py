#!/usr/bin/env python3
"""Fabrique un modèle .h2song (Hydrogen 1.2) pour un kit de l'ANCIEN format
(drumkit.xml sans <instrumentComponent>, ex. DeathMetal de 2008).

Hydrogen 1.2 n'arrive pas à convertir ces kits (« Unable to export
drumkit ») : chargé par LOAD_DRUMKIT, le kit a des instruments sans son et en
sourdine. Ce script prend un modèle valide fait par Hydrogen avec un autre kit
(structure 1.2) et y remplace la liste d'instruments par ceux du vieux kit.

    python3 modele_kit.py MODÈLE_VALIDE.h2song DOSSIER_DU_KIT SORTIE.h2song [--gain G]

--gain : gain de chaque instrument (Hydrogen accepte jusqu'à 5), pour un kit
enregistré trop bas ; les volumes sont aussi ramenés pour que le plus fort
soit à 1 (l'équilibre entre instruments est gardé).
"""
import os
import re
import sys
from xml.sax.saxutils import escape


def field(block, tag, default=""):
    m = re.search(rf"<{tag}>([^<]*)</{tag}>", block)
    return m.group(1) if m else default


def main():
    args = sys.argv[1:]
    gain = 1.0
    if "--gain" in args:
        i = args.index("--gain")
        gain = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 3 or not 0 < gain <= 5:
        print(__doc__)
        return 2
    ref_path, kit_dir, out_path = args
    if os.path.exists(out_path):
        raise SystemExit(f"{out_path} existe déjà : je ne l'écrase pas")
    ref = open(ref_path, encoding="utf-8").read()
    kit_xml = open(os.path.join(kit_dir, "drumkit.xml"), encoding="utf-8", errors="replace").read()
    kit_dir = os.path.abspath(kit_dir)
    kit_name = field(kit_xml, "name") or os.path.basename(kit_dir)
    if "<instrumentComponent>" in kit_xml:
        raise SystemExit("ce kit est déjà au format récent : le charger dans Hydrogen suffit")

    # Squelette d'un instrument 1.2 : le premier du modèle, sans ses couches.
    a = ref.find("<instrument>")
    b = ref.find("</instrument>", a) + len("</instrument>")
    skel = ref[a:b]
    ca = skel.find("<instrumentComponent>")
    cb = skel.find("</instrumentComponent>") + len("</instrumentComponent>")
    layer_a = skel.find("<layer>")
    layer_b = skel.find("</layer>") + len("</layer>")
    layer_skel = skel[layer_a:layer_b]
    comp_head = skel[ca:layer_a]
    comp_tail = skel[skel.rfind("</layer>") + len("</layer>"):cb]

    vols = [float(field(b, "volume", "1") or 1) for b in kit_xml.split("<instrument>")[1:]]
    vscale = 1.0 / max(vols) if gain != 1.0 and vols and max(vols) > 0 else 1.0
    out_ins = []
    files_missing = []
    for k, blk in enumerate(kit_xml.split("<instrument>")[1:]):
        blk = blk[:blk.find("</instrument>")]
        iid = field(blk, "id", str(k))
        name = field(blk, "name", f"Instrument {iid}")
        layers = []
        for lb in blk.split("<layer>")[1:]:
            lb = lb[:lb.find("</layer>")]
            fn = field(lb, "filename")
            if not os.path.exists(os.path.join(kit_dir, fn)):
                files_missing.append(fn)
            L = layer_skel
            for tag, val in (("filename", escape(fn)), ("min", field(lb, "min", "0")), ("max", field(lb, "max", "1")),
                             ("gain", field(lb, "gain", "1")), ("pitch", field(lb, "pitch", "0"))):
                L = re.sub(rf"<{tag}>[^<]*</{tag}>", f"<{tag}>{val}</{tag}>", L, count=1)
            layers.append(L)
        ins = skel[:ca] + comp_head + "\n    ".join(layers) + comp_tail + skel[cb:]
        for tag, val in (("id", iid), ("name", escape(name)), ("drumkitPath", escape(kit_dir)),
                         ("drumkit", escape(kit_name)),
                         ("volume", f"{min(1.0, float(field(blk, 'volume', '1') or 1) * vscale):.3g}"),
                         ("isMuted", "false"), ("pan_L", field(blk, "pan_L", "1")), ("pan_R", field(blk, "pan_R", "1")),
                         ("gain", f"{float(field(blk, 'gain', '1') or 1) * gain:.3g}"), ("midiOutNote", str(36 + int(iid))),
                         ("muteGroup", "-1")):
            ins = re.sub(rf"<{tag}>[^<]*</{tag}>", f"<{tag}>{val}</{tag}>", ins, count=1)
        out_ins.append(ins)

    la = ref.find("<instrumentList>") + len("<instrumentList>")
    lb = ref.find("</instrumentList>")
    out = ref[:la] + "\n  " + "\n  ".join(out_ins) + "\n " + ref[lb:]
    out = re.sub(r"<last_loaded_drumkit>[^<]*</last_loaded_drumkit>",
                 f"<last_loaded_drumkit>{escape(kit_dir)}</last_loaded_drumkit>", out, count=1)
    out = re.sub(r"<last_loaded_drumkit_name>[^<]*</last_loaded_drumkit_name>",
                 f"<last_loaded_drumkit_name>{escape(kit_name)}</last_loaded_drumkit_name>", out, count=1)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    print(f"{out_path} : {len(out_ins)} instruments du kit {kit_name}"
          + (f" ; fichiers absents : {', '.join(sorted(set(files_missing)))}" if files_missing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
