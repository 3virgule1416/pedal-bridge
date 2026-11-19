"""Pages du MIDI Captain pour le mode Drum (firmware officiel, « super mode »).

    python scripts/gen_captain.py

Écrit captain/page1.txt (DRUM), page2.txt (BRWS) et page3.txt (PART) à partir
de captain/entete.txt (en-tête de la page 0 du musicien). Rien n'est écrit sur
le Captain : on copie soi-même les fichiers dans son dossier « supersetup ».

Chaque bouton envoie, sur le canal 2 (les pages de PiPedal utilisent le
canal 1) : 127 à l'appui, 0 au relâchement (aussi après un appui long). Le
pont mesure lui-même la durée (appui long) et change les couleurs (SysEx)
selon l'état ; les couleurs écrites ici sont celles de départ.
Rangée du haut : key1 à key5 ; rangée du bas : keyA à keyE.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "captain")
CH = 2

# Couleurs de départ (pâles : le pont avive la sélection).
PALE = {
    "blanc": 0x404040, "jaune": 0x404000, "orange": 0x401800, "cyan": 0x003840,
    "vert": 0x004000, "rouge": 0x400000, "bleu": 0x000040, "turquoise": 0x003020,
    "violet": 0x200040, "magenta": 0x400040, "noir": 0x000000,
}

# (nom affiché, couleur) par touche ; rangée du haut puis du bas.
PAGES = {
    1: ("DRUM", 21, [("Tap", "blanc"), ("BPM-", "jaune"), ("BPM+", "jaune"), ("Var<", "cyan"), ("Var>", "cyan"),
                     ("Fill", "orange"), ("Var", "cyan"), ("Drum", "vert"), ("Part<", "vert"), ("Brk", "rouge")]),
    2: ("BRWS", 31, [("Styl<", "jaune"), ("Styl>", "jaune"), ("Kit<", "blanc"), ("Kit>", "blanc"), ("Metr", "rouge"),
                     ("Type<", "orange"), ("Type>", "orange"), ("Beat<", "cyan"), ("Beat>", "cyan"), ("Part", "blanc")]),
    3: ("PART", 41, [("Intr", "bleu"), ("Vers", "vert"), ("PreC", "turquoise"), ("Chor", "orange"), ("Brdg", "violet"),
                     ("Back", "blanc"), ("Next", "blanc"), ("Outr", "magenta"), ("AutF", "vert"), ("Brws", "blanc")]),
}
KEYS = ["key1", "key2", "key3", "key4", "key5", "keyA", "keyB", "keyC", "keyD", "keyE"]


def page(name, first_cc, keys):
    head = open(os.path.join(OUT, "entete.txt"), encoding="utf-8").read()
    head = head.replace("page_name = [Kemper]", f"page_name = [{name}]")
    assert f"page_name = [{name}]" in head
    out = [head.rstrip("\n"), "", ""]
    for k, (key, (label, color)) in enumerate(zip(KEYS, keys)):
        cc = first_cc + k
        c = f"[0x{PALE[color]:06X}]"
        out += [f"[{key}]", "keytimes = [1]", "ledmode = [normal]",
                f"ledcolor0 = {c}{c}{c}", f"ledcolor1 = {c}{c}{c}",
                f"short_dw1_name = [{label}]", f"short_dw1 = [{CH}][CC][{cc}][127]",
                "short_up1_name = []", f"short_up1 = [{CH}][CC][{cc}][0]",
                "long_up1_name = []", f"long_up1 = [{CH}][CC][{cc}][0]", ""]
    return "\n".join(out) + "\n"


for n, (name, cc, keys) in PAGES.items():
    with open(os.path.join(OUT, f"page{n}.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(page(name, cc, keys))
    print(f"page{n}.txt : {name}, CC{cc} à CC{cc + 9}, canal {CH}")
