# Génère lv2/ttl-test/test.ttl et manifest.ttl (plugin DLS Test, mono et stéréo).
#   python scripts/gen_test_ttl.py
import io, os
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lv2", "ttl-test")

HEAD = """@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix foaf:  <http://xmlns.com/foaf/0.1/> .
@prefix epp:   <http://lv2plug.in/ns/ext/port-props#> .
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix pipedal_ui: <http://github.com/rerdavies/pipedal/ui#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .
@prefix mod:   <http://moddevices.com/ns/mod#> .

<http://pedal-bridge.local/plugins/test#fichier>
    a lv2:Parameter ;
    rdfs:label "Fichier" ;
    mod:fileTypes "groove,h2song" ;
    rdfs:range atom:Path .

"""

def audio(idx, sym, name, out):
    kind = "lv2:OutputPort" if out else "lv2:InputPort"
    return f"""    [ a lv2:AudioPort, {kind} ; lv2:index {idx} ; lv2:symbol "{sym}" ; lv2:name "{name}" ]"""

def plugin(uri, name, stereo):
    ports = []
    if stereo:
        ports += [audio(0, "in_l", "Entrée G", 0), audio(1, "in_r", "Entrée D", 0),
                  audio(2, "out_l", "Sortie G", 1), audio(3, "out_r", "Sortie D", 1)]
        b = 4
    else:
        ports += [audio(0, "in", "Entrée", 0), audio(1, "out", "Sortie", 1)]
        b = 2
    ports.append(f"""    [ a lv2:ControlPort, lv2:InputPort ; lv2:index {b} ; lv2:symbol "bascule" ; lv2:name "Bascule" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:InputPort ; lv2:index {b+1} ; lv2:symbol "momentane" ; lv2:name "Momentané" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled, epp:trigger ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:InputPort ; lv2:index {b+2} ; lv2:symbol "liste" ; lv2:name "Liste" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 3 ; lv2:portProperty lv2:integer, lv2:enumeration ;
      lv2:scalePoint [ rdfs:label "Choix A" ; rdf:value 0 ] , [ rdfs:label "Choix B" ; rdf:value 1 ] ,
                     [ rdfs:label "Choix C" ; rdf:value 2 ] , [ rdfs:label "Choix D" ; rdf:value 3 ] ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+3} ; lv2:symbol "voyant" ; lv2:name "Voyant" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+4} ; lv2:symbol "compteur" ; lv2:name "Appuis" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1000 ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+5} ; lv2:symbol "clignote" ; lv2:name "Clignote" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+6} ; lv2:symbol "etat" ; lv2:name "État" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 2 ; lv2:portProperty lv2:integer, lv2:enumeration ;
      lv2:scalePoint [ rdfs:label "vide" ; rdf:value 0 ] , [ rdfs:label "enregistre" ; rdf:value 1 ] ,
                     [ rdfs:label "lecture" ; rdf:value 2 ] ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+7} ; lv2:symbol "longueur_nom" ; lv2:name "Nom reçu" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1024 ]""")
    ports.append(f"""    [ a lv2:ControlPort, lv2:OutputPort ; lv2:index {b+8} ; lv2:symbol "dossier_ok" ; lv2:name "Dossier créé" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ; lv2:portProperty lv2:toggled ]""")
    ports.append(f"""    [ a atom:AtomPort, lv2:InputPort ; atom:bufferType atom:Sequence ; atom:supports patch:Message ;
      lv2:designation lv2:control ; lv2:index {b+9} ; lv2:symbol "control" ; lv2:name "Control" ]""")
    ports.append(f"""    [ a atom:AtomPort, lv2:OutputPort ; atom:bufferType atom:Sequence ;
      lv2:designation lv2:control ; lv2:index {b+10} ; lv2:symbol "notify" ; lv2:name "Notify" ]""")
    s = io.StringIO()
    s.write(f"""<{uri}>
    a lv2:Plugin, lv2:UtilityPlugin ;
    doap:name "{name}" ;
    doap:maintainer [ a foaf:Person ; foaf:name "Davis" ] ;
    rdfs:comment "Plugin de test de Davis Loop Station : contrôles, voyants et sélecteur de fichier dans PiPedal." ;
    doap:license <http://opensource.org/licenses/isc> ;
    lv2:minorVersion 1 ; lv2:microVersion 0 ;
    lv2:requiredFeature urid:map ;
    lv2:optionalFeature lv2:hardRTCapable ;
    lv2:extensionData state:interface ;
    patch:readable <http://pedal-bridge.local/plugins/test#fichier> ;
    patch:writable <http://pedal-bridge.local/plugins/test#fichier> ;
    lv2:port
""")
    s.write(" ,\n".join(ports))
    s.write(" .\n\n")
    s.write(f"""<{uri}> pipedal_ui:ui <{uri}#ui> .

<{uri}#ui>
    a pipedal_ui:ui ;
    pipedal_ui:fileProperties [
        a pipedal_ui:fileProperty ;
        rdfs:label "Fichier" ;
        pipedal_ui:directory "PedalBridgeTest" ;
        lv2:index {b+3} ;
        pipedal_ui:patchProperty <http://pedal-bridge.local/plugins/test#fichier> ;
        pipedal_ui:fileTypes [
            a pipedal_ui:fileType ; rdfs:label "Groove" ;
            pipedal_ui:fileExtension ".groove" ; pipedal_ui:mimeType "application/octet-stream"
        ] , [
            a pipedal_ui:fileType ; rdfs:label "Morceau Hydrogen" ;
            pipedal_ui:fileExtension ".h2song" ; pipedal_ui:mimeType "application/octet-stream"
        ]
    ] .

""")
    return s.getvalue()

ttl = HEAD + plugin("http://pedal-bridge.local/plugins/test-mono", "DLS Test (Mono)", False) \
           + plugin("http://pedal-bridge.local/plugins/test-stereo", "DLS Test (Stéréo)", True)
open(os.path.join(HERE, "test.ttl"), "w", encoding="utf-8", newline="\n").write(ttl)

manifest = """@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<http://pedal-bridge.local/plugins/test-mono>
    a lv2:Plugin ;
    lv2:binary <pedal_bridge_test.so> ;
    rdfs:seeAlso <test.ttl> .

<http://pedal-bridge.local/plugins/test-stereo>
    a lv2:Plugin ;
    lv2:binary <pedal_bridge_test.so> ;
    rdfs:seeAlso <test.ttl> .
"""
open(os.path.join(HERE, "manifest.ttl"), "w", encoding="utf-8", newline="\n").write(manifest)
print("ok")
