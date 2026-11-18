#!/usr/bin/env python3
"""Range la liste Song de DLS Drum en 3 niveaux : Style / Groupe (type de
beat) / Beat. Rien n'est effacé : les fichiers sont déplacés (et leur
.parts.json avec eux) ; les anciens fichiers JJ (un par plage de tempo) vont
dans DOSSIER_ARCHIVE, remplacés par les nouveaux (un par famille de beat).

    python3 ranger.py DOSSIER_CHANSONS DOSSIER_JJ_NOUVEAU DOSSIER_ARCHIVE

Écrit DOSSIER_CHANSONS/.alias.txt (« ancien<TAB>nouveau », chemins relatifs) :
le pont s'en sert pour retrouver la chanson d'un ancien preset.
Les noms ne répètent pas le style (le dossier le dit) et n'ont pas de tempo
(c'est le musicien qui le choisit ; il reste dans les .parts.json).
"""
import os
import re
import shutil
import sys
import time

# Ancien chemin (sans .h2song) -> nouveau (Style/Groupe/Nom).
MAP = {
    "Ballad/GM Ballads S13 088 BPM": "Ballad/Ballade/Ballads S13",
    "Ballad/RP Bardet Slow": "Ballad/Ballade/Slow (RP Bardet)",
    "Ballad/RP Bardet Waltz": "Ballad/Valse/Waltz (RP Bardet)",
    "Blues/DLS Shuffle": "Blues/Shuffle/DLS",
    "Blues/RP Bardet Shuffle": "Blues/Shuffle/RP Bardet",
    "Blues/RP Bardet Boogie": "Blues/Shuffle/Boogie (RP Bardet)",
    "Blues/RP Bardet Blues": "Blues/Blues/RP Bardet",
    "Blues/GM Blues Classics 110 Cold Shock": "Blues/Blues/Cold Shock",
    "Country/GM Country Outlaws 107 Onry And Mean": "Country/Country/Onry And Mean",
    "Divers/RP Bardet Ending": "Divers/Fins/Ending (RP Bardet)",
    "Electro/GM DnB": "Electro/DnB/DnB",
    "Funk/DLS Funk": "Funk/Funk/DLS",
    "Funk/GM Funk 1": "Funk/Funk/Funk 1",
    "Funk/RP Bardet Funk": "Funk/Funk/RP Bardet",
    "Hip Hop/GM Chilled Beat 1": "Hip Hop/Chill/Chilled Beat 1",
    "Hip Hop/GM Chilled Beat 2": "Hip Hop/Chill/Chilled Beat 2",
    "Hip Hop/GM Hip Hop 1": "Hip Hop/Hip Hop/Hip Hop 1",
    "Jazz/RP Bardet Jazz": "Jazz/Jazz et Swing/Jazz",
    "Jazz/RP Bardet Swing": "Jazz/Jazz et Swing/Swing",
    "Latin/RP Bardet Bossa": "Latin/Bossa et Samba/Bossa",
    "Latin/RP Bardet Samba": "Latin/Bossa et Samba/Samba",
    "Latin/RP Bardet Chacha": "Latin/Cha-cha et Afro/Chacha",
    "Latin/RP Bardet AfroCub": "Latin/Cha-cha et Afro/AfroCub",
    "Latin/RP Bardet March and Tango": "Latin/Tango et Paso/March and Tango",
    "Latin/RP Bardet Paso and Charleston": "Latin/Tango et Paso/Paso and Charleston",
    "Metal/DLS Metal": "Metal/Straight/DLS",
    "Metal/GM 3-4 Metal": "Metal/Mesures impaires/3-4",
    "Metal/GM 7-4 Metal Double Kick": "Metal/Mesures impaires/7-4 Double Kick",
    "Metal/GM 7-4 Metal Single Kick": "Metal/Mesures impaires/7-4 Single Kick",
    "Metal/GM Doom S01 105 BPM": "Metal/Doom/Doom S01",
    "Pop/RP Bardet Pop": "Pop/Pop/RP Bardet",
    "Pop/GM Half-Time Pop": "Pop/Pop/Half-Time Pop",
    "Pop/GM Motown Beat": "Pop/Pop/Motown",
    "Pop/RP Bardet Disco": "Pop/Danse/Disco",
    "Pop/RP Bardet Twist": "Pop/Danse/Twist",
    "Pop/RP Bardet RnB": "Pop/Danse/RnB",
    "Punk/DLS Punk": "Punk/Punk/DLS",
    "Punk/Pop Punk 160": "Punk/Pop Punk/Pop Punk 1",
    "Punk/Pop Punk 170": "Punk/Pop Punk/Pop Punk 2",
    "Punk/Modern Pop Punk": "Punk/Pop Punk/Modern",
    "Reggae/RP Bardet Reggae": "Reggae/Reggae et Ska/Reggae",
    "Reggae/RP Bardet Ska": "Reggae/Reggae et Ska/Ska",
    "Rock/DLS Rock": "Rock/Rock droit/DLS",
    "Rock/RP Bardet Rock": "Rock/Rock droit/RP Bardet",
    "Rock/GM Rock Essentials 1 8th Rock": "Rock/Rock droit/8th",
    "Rock/GM Rock Essentials 1 Driving Rock": "Rock/Rock droit/Driving",
    "Rock/GM Classic Rock S03 146 Jail Breakin": "Rock/Rock droit/Classic S03",
    "Rock/GM Hi-Octane S15 130 BPM": "Rock/Rock droit/Hi-Octane S15",
    "Rock/GM Garage Rock S11 139 Get Free": "Rock/Rock droit/Garage S11",
    "Rock/GM Southern Rock S12 100 Road Fever": "Rock/Rock droit/Southern S12",
    "Rock/GM Contemporary Rock S06 120": "Rock/Rock droit/Contemporary S06",
    "Rock/GM Rock Essentials 1 Fills": "Rock/Rock droit/Fills (Rock Essentials)",
    "Rock/GM Rock Essentials 1 Half-Time Rock": "Rock/Half-time/Half-Time",
    "Rock/GM Rock Essentials 1 Rock Ballad": "Rock/Half-time/Ballad",
    "Rock/GM Rock Essentials 1 Floor Tom Rock": "Rock/Half-time/Floor Tom",
    "Rock/GM Rock Essentials 2 HT Shuffle 1": "Rock/Shuffle et 12-8/HT Shuffle",
    "Rock/GM Rock Essentials 2 060 12-8 02": "Rock/Shuffle et 12-8/12-8",
    "Rock/GM Rock Essentials 2 075 6-8 02 Stick": "Rock/Shuffle et 12-8/6-8 Stick",
    "Rock/Alt Rock 133": "Rock/Alt Rock/Verse et Chorus",
    "Rock/Alt Rock 137": "Rock/Alt Rock/Chorus et Bridge",
    "Rock/Alt Rock 139": "Rock/Alt Rock/Verse et Pre-Chorus",
    "Rock/Alt Rock 145": "Rock/Alt Rock/Chorus",
    "Rock/Alt Rock 146": "Rock/Alt Rock/Intro et Outro",
    "Rock/Alt Rock 150": "Rock/Alt Rock/Chorus et Post Chorus",
    "Rock/Alt Rock 153": "Rock/Alt Rock/Groove et Fill",
    "Rock/Alt Rock 160": "Rock/Alt Rock/Verse et Pre-Chorus 2",
    "Rock/Alt Rock 166": "Rock/Alt Rock/Verse",
    "Rock/Alt Rock 193": "Rock/Alt Rock/Chorus et Bridge 2",
    "Surf/GM 42TF Surf": "Surf/Surf/42TF Surf",
    "Surf/GM Surf 2": "Surf/Surf/Surf 2",
}

# Avant le rangement par style (premiers presets) : à la racine.
ROOT_OLD = {
    "Rock": "Rock/RP Bardet Rock", "Funk": "Funk/RP Bardet Funk", "Jazz": "Jazz/RP Bardet Jazz",
    "Swing": "Jazz/RP Bardet Swing", "Shuffle": "Blues/RP Bardet Shuffle", "Blues": "Blues/RP Bardet Blues",
    "Boogie": "Blues/RP Bardet Boogie", "Pop": "Pop/RP Bardet Pop", "Disco": "Pop/RP Bardet Disco",
    "RnB": "Pop/RP Bardet RnB", "Twist": "Pop/RP Bardet Twist", "Slow": "Ballad/RP Bardet Slow",
    "Waltz": "Ballad/RP Bardet Waltz", "Reggae": "Reggae/RP Bardet Reggae", "Ska": "Reggae/RP Bardet Ska",
    "AfroCub": "Latin/RP Bardet AfroCub", "Bossa": "Latin/RP Bardet Bossa", "Chacha": "Latin/RP Bardet Chacha",
    "Samba": "Latin/RP Bardet Samba", "March and Tango": "Latin/RP Bardet March and Tango",
    "Paso and Charleston": "Latin/RP Bardet Paso and Charleston", "Ending": "Divers/RP Bardet Ending",
    "DLS Rock": "Rock/DLS Rock", "DLS Funk": "Funk/DLS Funk", "DLS Shuffle": "Blues/DLS Shuffle",
    "DLS Punk": "Punk/DLS Punk", "DLS Metal": "Metal/DLS Metal",
}


def speed(lo, hi):
    """Allure d'une plage de tempo JJ (mots plutôt que BPM)."""
    if hi <= 100:
        return "lent"
    if hi <= 130:
        return "moyen"
    if hi <= 170:
        return "rapide"
    if hi <= 200:
        return "très rapide"
    return "extrême"


def jj_target(name):
    """« JJ Lamb Chops 190-210 Straight » -> « Metal/Straight/Lamb Chops très rapide »."""
    m = re.match(r"JJ (.+) (\d+)-(\d+) (Straight|Backbeat|Groove|Mesures impaires)$", name)
    if not m:
        return None
    pack, lo, hi, fam = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
    group = "Doom" if hi <= 100 else {"Straight": "Straight", "Groove": "Straight",
                                       "Backbeat": "Backbeat", "Mesures impaires": "Mesures impaires"}[fam]
    label = f"{pack} {speed(lo, hi)}"
    if fam == "Groove":
        label += " (grooves)"
    elif fam == "Backbeat" and group == "Doom":
        label += " (backbeat)"
    return f"Metal/{group}/{label}"


def move(src_base, dst_base, root):
    """Déplace X.h2song et X.parts.json (sans écraser)."""
    os.makedirs(os.path.dirname(os.path.join(root, dst_base)), exist_ok=True)
    for ext in (".h2song", ".parts.json"):
        s, d = os.path.join(root, src_base + ext), os.path.join(root, dst_base + ext)
        if os.path.exists(s) and not os.path.exists(d):
            os.rename(s, d)
            os.utime(d)                      # plus récent : le gros morceau sera refait


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    root, jj_new, archive = sys.argv[1:]
    aliases = {}
    moved = 0
    for old, new in MAP.items():
        if os.path.exists(os.path.join(root, old + ".h2song")):
            move(old, new, root)
            moved += 1
        aliases[old] = new
    for old, mid in ROOT_OLD.items():
        if mid in MAP:
            aliases[old] = MAP[mid]
    # JJ : anciens fichiers par plage -> archive ; nouveaux par famille -> Metal/<groupe>/.
    os.makedirs(archive, exist_ok=True)
    olds = [f[:-7] for f in os.listdir(os.path.join(root, "Metal")) if f.startswith("JJ ") and f.endswith(".h2song")]
    for base in olds:
        for ext in (".h2song", ".parts.json"):
            s = os.path.join(root, "Metal", base + ext)
            if os.path.exists(s):
                shutil.move(s, os.path.join(archive, base + ext))
    used = set()
    jj_files = sorted(f[:-7] for f in os.listdir(os.path.join(jj_new, "Metal")) if f.endswith(".h2song"))
    for base in jj_files:
        target = jj_target(base)
        if not target:
            print(f"  JJ ignoré (nom inattendu) : {base}")
            continue
        t, k = target, 2
        while t in used:                              # deux plages de même allure
            t, k = f"{target} {k}", k + 1
        used.add(t)
        os.makedirs(os.path.dirname(os.path.join(root, t)), exist_ok=True)
        for ext in (".h2song", ".parts.json"):
            s = os.path.join(jj_new, "Metal", base + ext)
            if os.path.exists(s):
                shutil.copy2(s, os.path.join(root, t + ext))
                os.utime(os.path.join(root, t + ext))
        m = re.match(r"JJ (.+ \d+-\d+) ", base)
        if m and base.endswith(("Straight", "Groove")):
            aliases.setdefault(f"Metal/JJ {m.group(1)}", t)   # ancien fichier par plage -> famille principale
    with open(os.path.join(root, ".alias.txt"), "w", encoding="utf-8", newline="\n") as f:
        for old, new in sorted(aliases.items()):
            f.write(f"{old}.h2song\t{new}.h2song\n")
    print(f"{moved} beats déplacés, {len(used)} beats JJ par famille, {len(olds)} anciens JJ archivés ; "
          f"{len(aliases)} alias ({time.strftime('%H:%M:%S')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
