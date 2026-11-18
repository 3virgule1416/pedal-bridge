# Génère lv2/ttl/looper.ttl et lv2/ttl/drum.ttl (DLS Looper et DLS Drum,
# mono et stéréo).
#   python scripts/gen_dls_ttl.py
# L'ordre des ports doit suivre l'enum de lv2/looper.c et de lv2/drum.c.
# Sections (pg:group) et couleurs des voyants (pipedal_ui:ledColor) comme les
# plugins TooB : PiPedal affiche chaque section dans sa propre boîte.
import os
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lv2", "ttl")

GROUPS = [("boucle_g", "Loop"), ("transport_g", "Transport"), ("pont_g", "Bridge")]
G = "http://pedal-bridge.local/plugins/looper#"

PREFIXES = """@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix epp:   <http://lv2plug.in/ns/ext/port-props#> .
@prefix foaf:  <http://xmlns.com/foaf/0.1/> .
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix mod:   <http://moddevices.com/ns/mod#> .
@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix pg:    <http://lv2plug.in/ns/ext/port-groups#> .
@prefix pipedal_ui: <http://github.com/rerdavies/pipedal/ui#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .

"""


def head(groups):
    return PREFIXES + "".join(f"""<{G}{sym}>
    a pg:ControlGroup ;
    lv2:name "{name}" ;
    lv2:symbol "{sym}" .

""" for sym, name in groups)

DESC = '"""' + """Live looper for Davis Loop Station, driven from PiPedal and your MIDI pedals.

Your signal is sent to SooperLooper through the Davis Loop Station bridge, then the loops are mixed back in. Place it at the start of the chain to record a dry guitar and run the loops through your effects, or at the end to record the loops with their effects. The signal is sent before the loops are added, so loops never record themselves.

Record, Overdub, Undo, Redo, Mute and Clear act on the selected loop. Starts and ends of recordings fall exactly on the beat grid of the tempo. Play/Stop stops or restarts everything (loops and drums) from bar 1.

Needs the pedal_bridge client and SooperLooper running on the Raspberry Pi (scripts/session.sh start). Without them, your signal passes through unchanged.""" + '"""'


def scale(points):
    return " ,\n                     ".join(f'[ rdfs:label "{l}" ; rdf:value {v} ]' for v, l in points)


def port(kind, idx, sym, name, comment, rest, group, color=None):
    extra = f' ;\n      pg:group <{G}{group}>'
    if color:
        extra += f' ;\n      pipedal_ui:ledColor "{color}"'
    return f"""    [ a lv2:ControlPort, {kind} ; lv2:index {idx} ; lv2:symbol "{sym}" ; lv2:name "{name}" ;
      rdfs:comment "{comment}" ;
      {rest}{extra} ]"""


def ctl_in(idx, sym, name, comment, rest, group):
    return port("lv2:InputPort", idx, sym, name, comment, rest, group)


def ctl_out(idx, sym, name, comment, rest, group, color=None):
    return port("lv2:OutputPort", idx, sym, name, comment, rest, group, color)


def button(idx, sym, name, comment, group):
    return ctl_in(idx, sym, name, comment,
                  "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled, epp:trigger", group)


def enum(lo, hi, points):
    return (f"lv2:default {lo} ; lv2:minimum {lo} ; lv2:maximum {hi} ; lv2:portProperty lv2:integer, lv2:enumeration ;\n"
            "      lv2:scalePoint " + scale(points))


LED = "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled"
STATES = ["offline", "empty", "waiting", "recording", "overdub", "playing", "muted", "paused", "other"]


def audio_ports(stereo):
    p = []
    if stereo:
        p += ['    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol "in_l" ; lv2:name "In L" ]',
              '    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 1 ; lv2:symbol "in_r" ; lv2:name "In R" ]',
              '    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 2 ; lv2:symbol "out_l" ; lv2:name "Out L" ]',
              '    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 3 ; lv2:symbol "out_r" ; lv2:name "Out R" ]']
    else:
        p += ['    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ]',
              '    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ]']
    return p


def plugin_head(uri, name, label, desc):
    return f"""<{uri}>
    a lv2:Plugin, lv2:UtilityPlugin ;
    doap:name "Davis Loop Station {name}" ;
    mod:brand "DLS" ;
    mod:label "{label}" ;
    doap:maintainer [ a foaf:Person ; foaf:name "Davis" ] ;
    rdfs:comment {desc} ;
    doap:license <http://opensource.org/licenses/isc> ;
    lv2:minorVersion 1 ; lv2:microVersion 0 ;
    lv2:optionalFeature lv2:hardRTCapable ;
    lv2:port
"""


def plugin(uri, name, label, stereo):
    p = audio_ports(stereo)
    i = 4 if stereo else 2
    B, T, P = "boucle_g", "transport_g", "pont_g"

    # Ordre = enum de lv2/looper.c, rangé par section.
    p.append(ctl_in(i, "boucle", "Loop", "Loop that the buttons act on (SooperLooper loop 1 to 8).",
                    "lv2:default 1 ; lv2:minimum 1 ; lv2:maximum 8 ; lv2:portProperty lv2:integer, lv2:enumeration ;\n"
                    "      lv2:scalePoint " + scale([(k, f"Loop {k}") for k in range(1, 9)]), B)); i += 1
    for sym, nm, cm in [
        ("record", "Record", "Start recording at the next cycle; press again to stop at the end of the cycle."),
        ("overdub", "Overdub", "Add a layer over the loop; press again to stop."),
        ("undo", "Undo", "Remove the last layer."),
        ("redo", "Redo", "Put back the last removed layer."),
        ("mute", "Mute", "Silence the loop, or bring it back."),
        ("effacer", "Clear", "Clear the loop completely."),
    ]:
        p.append(button(i, sym, nm, cm, B)); i += 1
    p.append(ctl_out(i, "etat", "State", "State of the selected loop.",
                     enum(0, len(STATES) - 1, list(enumerate(STATES))), B)); i += 1

    p.append(button(i, "play_stop", "Play / Stop", "Stop everything, or restart loops and drums together from bar 1.", T)); i += 1
    p.append(ctl_in(i, "tempo", "Tempo", "Tempo in BPM, applied when you change it. Set it before recording: loops do not stretch.",
                    "lv2:default 120 ; lv2:minimum 40 ; lv2:maximum 240 ; lv2:portProperty lv2:integer ; units:unit units:bpm", T)); i += 1
    p.append(ctl_out(i, "temps", "Beat", "Current beat in the bar (- when stopped).",
                     enum(0, 8, [(0, "-")] + [(k, str(k)) for k in range(1, 9)]), T)); i += 1
    # Voyants : étroits, donc un nom très court (le détail est dans la bulle).
    p.append(ctl_out(i, "pulsation", "♩", "Beat: flashes on every beat.", LED, T, "green")); i += 1
    p.append(ctl_out(i, "sur_le_1", "1", "Flashes on the first beat of each bar.", LED, T, "red")); i += 1
    p.append(ctl_out(i, "lecture", "▶", "Lit while the transport is playing.", LED, T, "green")); i += 1
    p.append(ctl_out(i, "tempo_actuel", "BPM", "Tempo currently used by the bridge.",
                     enum(0, 240, [(0, "-")] + [(k, str(k)) for k in range(40, 241)]), T)); i += 1

    p.append(ctl_in(i, "gain", "Volume", "Level of the loops mixed into your signal.",
                    "lv2:default 0.0 ; lv2:minimum -60.0 ; lv2:maximum 12.0 ; units:unit units:db", P)); i += 1
    p.append(ctl_out(i, "liaison", "Link", "OK: audio flows to and from SooperLooper. No audio: the pedal_bridge client runs but the audio link is not up. Bridge absent: the pedal_bridge client is not running.",
                     enum(0, 2, [(0, "no bridge"), (1, "no audio"), (2, "OK")]), P)); i += 1

    return plugin_head(uri, name, label, DESC) + " ,\n".join(p) + " .\n\n"


ttl = head(GROUPS) + plugin("http://pedal-bridge.local/plugins/looper-mono", "Looper (Mono)", "DLS Looper", False) \
           + plugin("http://pedal-bridge.local/plugins/looper-stereo", "Looper (Stéréo)", "DLS Looper St.", True)
open(os.path.join(HERE, "looper.ttl"), "w", encoding="utf-8", newline="\n").write(ttl)

# ------------------------------------------------------------- DLS Drum ---
G = "http://pedal-bridge.local/plugins/drum#"
DRUM_GROUPS = [("drum_g", "Drums"), ("transport_g", "Transport"), ("pont_g", "Bridge")]

DRUM_DESC = '"""' + """Drums for Davis Loop Station: the Hydrogen drum machine, played in time with your loops and driven from PiPedal and your MIDI pedals.

The drums are mixed into your signal. Place it at the end of the chain so the drums go through none of your effects. Without this plugin in the preset, the drums are silent, but the tempo and the bar grid keep working for the loops.

Kit picks the drum kit and Song the style (a .h2song file of the "DLS Drums" folder): every song plays on the chosen kit. A new choice is loaded when the transport is stopped. Groove selects the Hydrogen pattern (1 to 160); Previous and Next step through them, for your pedals. The new groove starts at the next bar, and the length of a SooperLooper cycle follows the length of the groove. Play/Stop stops or restarts everything (drums and loops) from bar 1. BPM + and BPM - change the tempo by 1 (hold them to go faster). Tap sets the tempo from your last taps. Metronome turns the Hydrogen click on or off.

Needs the pedal_bridge client and Hydrogen running on the Raspberry Pi (scripts/session.sh start).""" + '"""'


SONG = G + "song"
# Liste Kit : les .kit de scripts/kits, en ordre alphabétique (le pont lit le
# même dossier dans le même ordre : numéro de la liste -> fichier .kit).
KITS = sorted(f[:-4] for f in os.listdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kits"))
              if f.endswith(".kit"))
KIT_DEFAULT = KITS.index("GM Rock Kit") if "GM Rock Kit" in KITS else 0


def drum_plugin(uri, name, label, stereo):
    p = audio_ports(stereo)
    i = 4 if stereo else 2
    D, T, P = "drum_g", "transport_g", "pont_g"
    # Ordre = enum de lv2/drum.c, rangé par section.
    p.append(ctl_in(i, "kit", "Kit", "Drum kit for every song. Loaded when the transport is stopped.",
                    enum(0, len(KITS) - 1, list(enumerate(KITS))).replace("lv2:default 0 ;", f"lv2:default {KIT_DEFAULT} ;", 1), D)); i += 1
    song_index = i
    p.append(ctl_in(i, "groove", "Groove", "Hydrogen pattern to play (1 = first pattern of the song), from the next bar.",
                    enum(1, 160, [(k, f"Groove {k}") for k in range(1, 161)]), D)); i += 1
    p.append(button(i, "precedent", "Previous", "Previous groove, from the next bar.", D)); i += 1
    p.append(button(i, "suivant", "Next", "Next groove, from the next bar.", D)); i += 1
    p.append(ctl_out(i, "groove_joue", "Playing", "Groove being played (- when unknown).",
                     enum(0, 160, [(0, "-")] + [(k, str(k)) for k in range(1, 161)]), D)); i += 1

    p.append(button(i, "play_stop", "Play / Stop", "Stop everything, or restart drums and loops together from bar 1.", T)); i += 1
    p.append(ctl_in(i, "tempo", "Tempo", "Tempo in BPM, applied when you change it. Set it before recording: loops do not stretch.",
                    "lv2:default 120 ; lv2:minimum 40 ; lv2:maximum 240 ; lv2:portProperty lv2:integer ; units:unit units:bpm", T)); i += 1
    for sym, nm, what in (("bpm_plus", "BPM +", "Raise"), ("bpm_moins", "BPM -", "Lower")):
        p.append(ctl_in(i, sym, nm, f"{what} the tempo by 1 BPM. Hold it to keep going, faster after 2 seconds.",
                        "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled, mod:preferMomentaryOffByDefault", T)); i += 1
    p.append(button(i, "tap", "Tap", "Tap the beat: the tempo follows your last taps (3 taps or more).", T)); i += 1
    p.append(ctl_in(i, "metronome", "Metronome", "Hydrogen metronome click on or off.",
                    "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled", T)); i += 1
    p.append(ctl_out(i, "temps", "Beat", "Current beat in the bar (- when stopped).",
                     enum(0, 8, [(0, "-")] + [(k, str(k)) for k in range(1, 9)]), T)); i += 1
    p.append(ctl_out(i, "pulsation", "♩", "Beat: flashes on every beat.", LED, T, "green")); i += 1
    p.append(ctl_out(i, "sur_le_1", "1", "Flashes on the first beat of each bar.", LED, T, "red")); i += 1
    p.append(ctl_out(i, "lecture", "▶", "Lit while the transport is playing.", LED, T, "green")); i += 1
    p.append(ctl_out(i, "tempo_actuel", "BPM", "Tempo currently used by the bridge.",
                     enum(0, 240, [(0, "-")] + [(k, str(k)) for k in range(40, 241)]), T)); i += 1

    p.append(ctl_in(i, "gain", "Volume", "Level of the drums mixed into your signal.",
                    "lv2:default 0.0 ; lv2:minimum -60.0 ; lv2:maximum 12.0 ; units:unit units:db", P)); i += 1
    p.append(ctl_out(i, "liaison", "Link", "OK: drums arrive from Hydrogen through the bridge. No audio: the pedal_bridge client runs but the drum link is not up. No bridge: the pedal_bridge client is not running.",
                     enum(0, 2, [(0, "no bridge"), (1, "no audio"), (2, "OK")]), P)); i += 1
    # Messages patch du sélecteur de fichier (Song).
    p.append(f"""    [ a atom:AtomPort, lv2:InputPort ; atom:bufferType atom:Sequence ; atom:supports patch:Message ;
      lv2:designation lv2:control ; lv2:index {i} ; lv2:symbol "control" ; lv2:name "Control" ]"""); i += 1
    p.append(f"""    [ a atom:AtomPort, lv2:OutputPort ; atom:bufferType atom:Sequence ;
      lv2:designation lv2:control ; lv2:index {i} ; lv2:symbol "notify" ; lv2:name "Notify" ]"""); i += 1
    head_ = plugin_head(uri, name, label, DRUM_DESC).replace("    lv2:port\n", f"""    lv2:optionalFeature urid:map ;
    lv2:extensionData state:interface ;
    patch:readable <{SONG}> ;
    patch:writable <{SONG}> ;
    lv2:port
""")
    ui = f"""<{uri}> pipedal_ui:ui <{uri}#ui> .

<{uri}#ui>
    a pipedal_ui:ui ;
    pipedal_ui:fileProperties [
        a pipedal_ui:fileProperty ;
        rdfs:label "Song" ;
        pipedal_ui:directory "DLS Drums" ;
        lv2:index {song_index} ;
        lv2:portGroup <{G}drum_g> ;
        pipedal_ui:patchProperty <{SONG}> ;
        pipedal_ui:fileTypes [
            a pipedal_ui:fileType ; rdfs:label "Hydrogen song" ;
            pipedal_ui:fileExtension ".h2song" ; pipedal_ui:mimeType "application/octet-stream"
        ]
    ] .

"""
    return head_ + " ,\n".join(p) + " .\n\n" + ui


SONG_PARAM = f"""<{SONG}>
    a lv2:Parameter ;
    rdfs:label "Song" ;
    mod:fileTypes "h2song" ;
    rdfs:range atom:Path .

"""
ttl = head(DRUM_GROUPS) + SONG_PARAM + drum_plugin("http://pedal-bridge.local/plugins/drum-mono", "Drum (Mono)", "DLS Drum", False) \
                        + drum_plugin("http://pedal-bridge.local/plugins/drum-stereo", "Drum (Stéréo)", "DLS Drum St.", True)
open(os.path.join(HERE, "drum.ttl"), "w", encoding="utf-8", newline="\n").write(ttl)
print("ok")
