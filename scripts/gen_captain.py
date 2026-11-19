"""Pages du MIDI Captain pour le mode Drum (firmware officiel, « super mode »).

    python scripts/gen_captain.py

Écrit captain/page1.txt (DRUM), page2.txt (BROWSE) et page3.txt (PARTS) à partir
de captain/entete.txt (en-tête de la page 0 du musicien). Rien n'est écrit sur
le Captain : on copie soi-même les fichiers dans son dossier « supersetup ».

Chaque bouton envoie, sur le canal 2 (les pages de PiPedal utilisent le
canal 1) : 127 à l'appui, 0 au relâchement (aussi après un appui long). Le
pont mesure lui-même la durée (appui long).
Aucune couleur ici (DEL éteintes, comme la page 0) : le pont peint les DEL
(SysEx) à chaque changement de page et d'état ; les couleurs vivent dans le
pont seulement.
Rangée du haut : key1 à key5 ; rangée du bas : keyA à keyE.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "captain")
CH = 2

# Nom affiché par touche ; rangée du haut puis du bas.
PAGES = {
    1: ("DRUM", 21, ["Tap", "BPM-", "BPM+", "Var<", "Var>", "Fill", "Var", "Drum", "Part<", "Brk"]),
    2: ("BROWSE", 31, ["Styl<", "Styl>", "Kit<", "Kit>", "Metr", "Type<", "Type>", "Beat<", "Beat>", "Part"]),
    3: ("PARTS", 41, ["Intr", "Vers", "PreC", "Chor", "Brdg", "Back", "Next", "Outr", "AutF", "Brws"]),
}
KEYS = ["key1", "key2", "key3", "key4", "key5", "keyA", "keyB", "keyC", "keyD", "keyE"]


def page(name, first_cc, keys):
    head = open(os.path.join(OUT, "entete.txt"), encoding="utf-8").read()
    head = head.replace("page_name = [Kemper]", f"page_name = [{name}]")
    assert f"page_name = [{name}]" in head
    out = [head.rstrip("\n"), "", ""]
    for k, (key, label) in enumerate(zip(KEYS, keys)):
        cc = first_cc + k
        c = "[0x000000]"                              # éteintes : le pont peint les DEL
        out += [f"[{key}]", "keytimes = [1]", "ledmode = [normal]",
                f"ledcolor0 = {c}{c}{c}", f"ledcolor1 = {c}{c}{c}",
                f"short_dw1_name = [{label}]", f"short_dw1 = [{CH}][CC][{cc}][127]",
                "short_up1_name = []", f"short_up1 = [{CH}][CC][{cc}][0]",
                "long_up1_name = []", f"long_up1 = [{CH}][CC][{cc}][0]", ""]
    return "\n".join(out) + "\n"


for n, (name, cc, keys) in PAGES.items():
    # Fins de ligne Windows (CRLF), comme les pages d'origine du Captain.
    with open(os.path.join(OUT, f"page{n}.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(page(name, cc, keys))
    print(f"page{n}.txt : {name}, CC{cc} à CC{cc + 9}, canal {CH}")
