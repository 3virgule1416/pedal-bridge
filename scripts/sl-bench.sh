#!/usr/bin/env bash
# Banc d'essai SooperLooper + pont, sur un serveur JACK séparé (ta session n'est
# pas touchée). Le faux PiPedal envoie un sinus CONTINU dans SooperLooper ; on
# enregistre une boucle par OSC (départ, puis stop au milieu d'une mesure) et on
# regarde dans ce qui ressort s'il y a des trous.
#
#   bash scripts/sl-bench.sh            réglages de ta session (compensation 544/544)
#   SL_IN=0 SL_OUT=0 SL_TRIG=128 bash scripts/sl-bench.sh   sans compensation
#   SL_STOP_BARS=2.5   moment du stop, en mesures après le début de l'enregistrement
set -u

HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "SL_IN='${SL_IN:-}' SL_OUT='${SL_OUT:-}' SL_TRIG='${SL_TRIG:-}' SL_STOP_BARS='${SL_STOP_BARS:-}' SL_QUANT='${SL_QUANT:-}' SL_ROUND='${SL_ROUND:-}' bash '$REMOTE/src/scripts/sl-bench.sh' $*"
fi

B="$HOME/$REMOTE/build"
SO="$B/lv2/pedal-bridge.lv2/pedal_bridge.so"
BUNDLE="$B/lv2/pedal-bridge.lv2/"
SRV=pbtest
OSC=9970
PY="$HOME/bridge-env/bin/python"
SL_IN="${SL_IN:-544}"; SL_OUT="${SL_OUT:-544}"; SL_TRIG="${SL_TRIG:-7744}"
SL_STOP_BARS="${SL_STOP_BARS:-2.5}"; SL_QUANT="${SL_QUANT:-1}"; SL_ROUND="${SL_ROUND:-1}"

export PB_SHM_DIR="$(mktemp -d /tmp/pb-sl-XXXXXX)"
PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2> /dev/null; done
  wait 2> /dev/null
  rm -f "$PB_SHM_DIR"/pedal-bridge-L* "$PB_SHM_DIR"/*.log "$PB_SHM_DIR"/out.wav
  rmdir "$PB_SHM_DIR" 2> /dev/null
}
trap cleanup EXIT

jackd -n "$SRV" -R -d dummy -r 48000 -p 128 > "$PB_SHM_DIR/jackd.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do jack_lsp -s "$SRV" > /dev/null 2>&1 && break; sleep 0.1; done

"$B/pb_fakehost" "$SO" "$BUNDLE" --seconds 32 --warmup 100 --report 100 --wav "$PB_SHM_DIR/out.wav" \
  > "$PB_SHM_DIR/fakehost.log" 2>&1 & FH=$!; PIDS+=($FH)
"$B/pedal_bridge" --server "$SRV" --links 1 --stats 0 > "$PB_SHM_DIR/bridge.log" 2>&1 & PIDS+=($!)
sooperlooper -S "$SRV" -j sltest -p "$OSC" -l 1 -c 2 > "$PB_SHM_DIR/sl.log" 2>&1 & PIDS+=($!)

for _ in $(seq 100); do
  jack_lsp -s "$SRV" 2> /dev/null | grep -q sltest:common_in_1 && \
  jack_lsp -s "$SRV" 2> /dev/null | grep -q pedal_bridge:send1_L && break
  sleep 0.1
done
for c in 1 2; do
  ch=$([ $c = 1 ] && echo L || echo R)
  jack_connect -s "$SRV" "pedal_bridge:send1_$ch" "sltest:common_in_$c"
  jack_connect -s "$SRV" "sltest:common_out_$c" "pedal_bridge:return1_$ch"
done

echo "Réglages SooperLooper : quantize=$SL_QUANT round=$SL_ROUND, latence entrée $SL_IN, sortie $SL_OUT, déclencheur $SL_TRIG"
"$PY" - "$OSC" "$SL_IN" "$SL_OUT" "$SL_TRIG" "$SL_STOP_BARS" "$SL_QUANT" "$SL_ROUND" <<'PY'
import sys, time
from pythonosc.udp_client import SimpleUDPClient
from pythonosc.dispatcher import Dispatcher
from pythonosc.osc_server import BlockingOSCUDPServer
port, lin, lout, trig, stop_bars, quant, rnd = int(sys.argv[1]), *map(float, sys.argv[2:8])
c = SimpleUDPClient("127.0.0.1", port)
got = {}
d = Dispatcher(); d.set_default_handler(lambda a, *v: got.__setitem__(v[1], v[2]))
srv = BlockingOSCUDPServer(("127.0.0.1", port + 1), d); srv.timeout = 0.05
def get(p):
    got.pop(p, None)
    c.send_message("/sl/0/get", [p, f"osc.udp://127.0.0.1:{port+1}/", "/r"])
    t = time.time()
    while p not in got and time.time() - t < 0.5: srv.handle_request()
    return got.get(p)
time.sleep(1.0)
c.send_message("/set", ["sync_source", -3.0])      # horloge interne
c.send_message("/set", ["tempo", 130.0])
c.send_message("/set", ["eighth_per_cycle", 8.0])
for k, v in (("sync", 1), ("quantize", quant), ("round", rnd), ("dry", 0.0), ("wet", 1.0),
             ("autoset_latency", 0), ("input_latency", lin), ("output_latency", lout),
             ("trigger_latency", trig)):
    c.send_message("/sl/0/set", [k, float(v)])
time.sleep(4.0)                                     # laisser le pont démarrer
bar = 8 * 60.0 / 130.0 / 2                          # 1 mesure = 8 croches
t0 = time.time()
c.send_message("/sl/0/hit", ["record"])
while get("state") != 2.0 and time.time() - t0 < 5: pass
t_rec = time.time()
print(f"enregistrement commencé {t_rec - t0:.2f} s après l'appui")
time.sleep(stop_bars * bar)
c.send_message("/sl/0/hit", ["record"])
t_stop = time.time()
while get("state") not in (4.0,) and time.time() - t_stop < 10: pass
print(f"lecture commencée {time.time() - t_stop:.2f} s après l'appui sur stop "
      f"(stop appuyé à {stop_bars} mesures)")
print(f"longueur de boucle : {get('loop_len'):.3f} s = {get('loop_len') / bar:.2f} mesures")
PY

wait "$FH"
grep -E "enregistrée" "$PB_SHM_DIR/fakehost.log"

"$PY" - "$PB_SHM_DIR/out.wav" <<'PY'
import sys, array, math
raw = open(sys.argv[1], "rb").read()[44:]
a = array.array("f"); a.frombytes(raw)
L = a[0::2]
rate, win = 48000, 480                      # fenêtres de 10 ms
env = [max(abs(x) for x in L[i:i + win]) for i in range(0, len(L) - win, win)]
on = [e > 0.2 for e in env]
first = next((i for i, v in enumerate(on) if v), None)
if first is None:
    print("Aucun son en sortie de SooperLooper."); sys.exit()
print(f"Sortie de SooperLooper (sinus continu attendu) à partir de {first * 0.01:.2f} s :")
i, segs = first, []
while i < len(on):
    j = i
    while j < len(on) and on[j] == on[i]: j += 1
    segs.append((on[i], i * 0.01, (j - i) * 0.01)); i = j
for s, t, dur in segs[:20]:
    print(f"  {'SON ' if s else 'VIDE'} à {t:6.2f} s pendant {dur:5.2f} s")
PY
