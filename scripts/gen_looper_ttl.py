# Génère lv2/ttl/looper.ttl (plugin DLS Looper, mono et stéréo).
#   python scripts/gen_looper_ttl.py
# L'ordre des ports doit suivre l'enum de lv2/looper.c.
import os
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lv2", "ttl")

HEAD = """@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix epp:   <http://lv2plug.in/ns/ext/port-props#> .
@prefix foaf:  <http://xmlns.com/foaf/0.1/> .
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix mod:   <http://moddevices.com/ns/mod#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .

"""

DESC = '"""' + """Live looper for Davis Loop Station, driven from PiPedal and your MIDI pedals.

Your signal is sent to SooperLooper through the Davis Loop Station bridge, then the loops are mixed back in. Place it at the start of the chain to record a dry guitar and run the loops through your effects, or at the end to record the loops with their effects. The signal is sent before the loops are added, so loops never record themselves.

Record, Overdub, Undo, Redo, Mute and Clear act on the selected loop. Starts and ends of recordings fall exactly on the beat grid of the tempo. Play/Stop stops or restarts everything (loops and drums) from bar 1.

Needs the pedal_bridge client and SooperLooper running on the Raspberry Pi (scripts/session.sh start). Without them, your signal passes through unchanged.""" + '"""'


def scale(points):
    return " ,\n                     ".join(f'[ rdfs:label "{l}" ; rdf:value {v} ]' for v, l in points)


def ctl_in(idx, sym, name, comment, rest):
    return f"""    [ a lv2:ControlPort, lv2:InputPort ; lv2:index {idx} ; lv2:symbol "{sym}" ; lv2:name "{name}" ;
      rdfs:comment "{comment}" ;
      {rest} ]"""


def ctl_out(idx, sym, name, comment, rest):
    return f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {idx} ; lv2:symbol "{sym}" ; lv2:name "{name}" ;
      rdfs:comment "{comment}" ;
      {rest} ]"""


def button(idx, sym, name, comment):
    return ctl_in(idx, sym, name, comment,
                  "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled, epp:trigger")


LED = "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled"
STATES = ["hors ligne", "vide", "attente", "enregistre", "overdub", "lecture", "muet", "pause", "autre"]


def plugin(uri, name, label, stereo):
    p = []
    if stereo:
        p += [f'    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol "in_l" ; lv2:name "Entrée G" ]',
              f'    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 1 ; lv2:symbol "in_r" ; lv2:name "Entrée D" ]',
              f'    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 2 ; lv2:symbol "out_l" ; lv2:name "Sortie G" ]',
              f'    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 3 ; lv2:symbol "out_r" ; lv2:name "Sortie D" ]']
        b = 4
    else:
        p += [f'    [ a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "Entrée" ]',
              f'    [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Sortie" ]']
        b = 2
    i = b
    p.append(ctl_in(i, "boucle", "Boucle", "Loop that the buttons act on (SooperLooper loop 1 to 8).",
                    "lv2:default 1 ; lv2:minimum 1 ; lv2:maximum 8 ; lv2:portProperty lv2:integer, lv2:enumeration ;\n      lv2:scalePoint "
                    + scale([(k, f"Boucle {k}") for k in range(1, 9)]))); i += 1
    for sym, nm, cm in [
        ("record", "Record", "Start recording at the next cycle; press again to stop at the end of the cycle."),
        ("overdub", "Overdub", "Add a layer over the loop; press again to stop."),
        ("undo", "Undo", "Remove the last layer."),
        ("redo", "Redo", "Put back the last removed layer."),
        ("mute", "Mute", "Silence the loop, or bring it back."),
        ("effacer", "Effacer", "Clear the loop completely."),
        ("play_stop", "Play / Stop", "Stop everything, or restart loops and drums together from bar 1."),
    ]:
        p.append(button(i, sym, nm, cm)); i += 1
    p.append(ctl_in(i, "tempo", "Tempo", "Tempo in BPM, applied when you change it. Set it before recording: loops do not stretch.",
                    "lv2:default 120 ; lv2:minimum 40 ; lv2:maximum 240 ; lv2:portProperty lv2:integer ; units:unit units:bpm")); i += 1
    p.append(ctl_in(i, "gain", "Volume boucles", "Level of the loops mixed into your signal.",
                    "lv2:default 0.0 ; lv2:minimum -60.0 ; lv2:maximum 12.0 ; units:unit units:db")); i += 1
    p.append(ctl_out(i, "etat", "État de la boucle", "State of the selected loop.",
                     f"lv2:default 0 ; lv2:minimum 0 ; lv2:maximum {len(STATES) - 1} ; lv2:portProperty lv2:integer, lv2:enumeration ;\n      lv2:scalePoint "
                     + scale(list(enumerate(STATES))))); i += 1
    p.append(ctl_out(i, "temps", "Temps de la mesure", "Current beat in the bar (- when stopped).",
                     "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 8 ; lv2:portProperty lv2:integer, lv2:enumeration ;\n      lv2:scalePoint "
                     + scale([(0, "-")] + [(k, str(k)) for k in range(1, 9)]))); i += 1
    p.append(ctl_out(i, "pulsation", "Pulsation", "Flashes on every beat.", LED)); i += 1
    p.append(ctl_out(i, "sur_le_1", "Sur le 1", "Flashes on the first beat of each bar.", LED)); i += 1
    p.append(ctl_out(i, "lecture", "En lecture", "Lit while the transport is playing.", LED)); i += 1
    p.append(ctl_out(i, "tempo_actuel", "Tempo actuel", "Tempo currently used by the bridge (BPM).",
                     "lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 240 ; lv2:portProperty lv2:integer, lv2:enumeration ;\n      lv2:scalePoint "
                     + scale([(0, "-")] + [(k, str(k)) for k in range(40, 241)]))); i += 1
    p.append(ctl_out(i, "pont", "Pont actif", "Lit when the pedal_bridge client is running.", LED)); i += 1
    p.append(ctl_out(i, "audio", "Audio connecté", "Lit when audio flows to and from SooperLooper through the bridge.", LED)); i += 1

    return f"""<{uri}>
    a lv2:Plugin, lv2:UtilityPlugin ;
    doap:name "Davis Loop Station {name}" ;
    mod:brand "DLS" ;
    mod:label "{label}" ;
    doap:maintainer [ a foaf:Person ; foaf:name "Davis" ] ;
    rdfs:comment {DESC} ;
    doap:license <http://opensource.org/licenses/isc> ;
    lv2:minorVersion 1 ; lv2:microVersion 0 ;
    lv2:optionalFeature lv2:hardRTCapable ;
    lv2:port
""" + " ,\n".join(p) + " .\n\n"


ttl = HEAD + plugin("http://pedal-bridge.local/plugins/looper-mono", "Looper (Mono)", "DLS Looper", False) \
           + plugin("http://pedal-bridge.local/plugins/looper-stereo", "Looper (Stéréo)", "DLS Looper St.", True)
open(os.path.join(HERE, "looper.ttl"), "w", encoding="utf-8", newline="\n").write(ttl)
print("ok")
