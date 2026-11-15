#!/usr/bin/env bash
# Étape (d) : mesures sur la vraie session, sans rien modifier (lecture seule).
#
#   bash scripts/mesure.sh [durée en secondes, défaut 600]
#
# Toutes les 10 s : stats du pont (dérive, remplissage, sous/sur-débits, xruns
# JACK, charge DSP), xruns vus par SooperLooper, CPU de chaque programme,
# température et bridage du processeur. Résumé à la fin.
# Journal : /tmp/pedal-bridge-session/mesure-<date>.log
set -u
HOST="${PB_HOST:-pi-dev}"
REMOTE="${PB_REMOTE_DIR:-pedal-bridge-build}"
if [ "$(uname -m)" != "aarch64" ]; then
  exec ssh "$HOST" "bash '$REMOTE/src/scripts/mesure.sh' $*"
fi

DUR="${1:-600}"
STEP=10
RUN=/tmp/pedal-bridge-session
LOG="$RUN/mesure-$(date +%Y%m%d-%H%M%S).log"
BLOG="$RUN/pedal_bridge.log"
SLOG="$RUN/sooperlooper.log"
HZ=$(getconf CLK_TCK)
PROCS="pipedald jackd pedal_bridge pb_timebase hydrogen sooperlooper slgui Xvfb x11vnc"

pids() {    # pipedald renomme son processus (« ppdl_main ») : on le cherche par commande
  if [ "$1" = pipedald ]; then pgrep -f '^/usr/sbin/pipedald'; else pgrep -x "$1"; fi
}

ticks() {   # temps CPU cumulé (utime + stime) de tous les processus d'un nom
  local t=0 p
  for p in $(pids "$1"); do
    [ -r "/proc/$p/stat" ] && t=$(( t + $(awk '{print $14 + $15}' "/proc/$p/stat") ))
  done
  echo "$t"
}
cpu_total() { awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8, $5+$6}' /proc/stat; }   # total, inactif
temp() { awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp; }
throttled() { vcgencmd get_throttled 2> /dev/null | cut -d= -f2; }
sl_xruns() { grep -c "got xrun" "$SLOG" 2> /dev/null || echo 0; }
last_stats() {   # dernières lignes de stats du pont : xruns JACK, DSP, puis lien 1 envoi et retour
  tail -n 40 "$BLOG" | awk '
    /JACK xruns/ { x=$4; sub(",","",x); d=$6 }
    /L1 envoi/   { se=$0 } /L1 retour/ { re=$0 }
    END { print x, d; print se; print re }'
}
num_after() { echo "$1" | grep -o "$2 *[-+0-9.]*" | head -1 | grep -o "[-+0-9.]*$"; }

{
echo "Mesure de $DUR s — $(date)"
echo "JACK : période $(jack_bufsize 2> /dev/null | grep -o '[0-9]\+' | head -1), tempo : $($HOME/$REMOTE/build/pb_timebase --get 2> /dev/null)"
echo "PiPedal : $(grep -o '"sampleRate": [0-9]*\|"bufferSize": [0-9]*\|"numberOfBuffers": [0-9]*' /var/pipedal/AudioConfig.json | tr '\n' ' ')"
echo "Processus présents :"
for n in $PROCS; do printf "  %-13s %s\n" "$n" "$(pids "$n" | tr '\n' ' ')"; done
echo
} | tee "$LOG"

declare -A T0 TPREV
for n in $PROCS; do T0[$n]=$(ticks "$n"); TPREV[$n]=${T0[$n]}; done
read -r CT0 CI0 < <(cpu_total)
SLX0=$(sl_xruns)
mapfile -t S0 < <(last_stats)
JX0=$(echo "${S0[0]}" | awk '{print $1}')
UND0=$(num_after "${S0[1]}" "sous-débits"); OVR0=$(num_after "${S0[2]}" "sur-débits")
UNDR0=$(num_after "${S0[2]}" "sous-débits")
TMAX=0; DRIFTS=()

printf "%6s %6s %8s %8s %9s %9s %7s %6s %6s %s\n" "t(s)" "temp" "Pi CPU%" "DSP%" "dérive→" "dérive←" "rempl→" "xJACK" "xSL" "CPU% par programme" | tee -a "$LOG"
t=0
while [ "$t" -lt "$DUR" ]; do
  sleep "$STEP"; t=$(( t + STEP ))
  read -r CT CI < <(cpu_total)
  busy=$(awk -v t="$CT" -v i="$CI" -v t0="$CT0" -v i0="$CI0" 'BEGIN{printf "%.1f", 100*(1-(i-i0)/(t-t0))}')
  CT0=$CT; CI0=$CI
  mapfile -t S < <(last_stats)
  jx=$(echo "${S[0]}" | awk '{print $1}'); dsp=$(echo "${S[0]}" | awk '{print $2}')
  ds=$(num_after "${S[1]}" "PiPedal"); dr=$(num_after "${S[2]}" "PiPedal")
  fs=$(num_after "${S[1]}" "rempl")
  per=""
  for n in $PROCS; do
    c=$(ticks "$n")
    per="$per $n=$(awk -v a="$c" -v b="${TPREV[$n]}" -v h="$HZ" -v s="$STEP" 'BEGIN{printf "%.1f", 100*(a-b)/h/s}')"
    TPREV[$n]=$c
  done
  tc=$(temp); TMAX=$(awk -v a="$tc" -v b="$TMAX" 'BEGIN{print (a>b)?a:b}')
  DRIFTS+=("$ds")
  printf "%6d %6s %8s %8s %9s %9s %7s %6s %6s%s\n" "$t" "$tc" "$busy" "$dsp" "$ds" "$dr" "$fs" \
         "$(( jx - JX0 ))" "$(( $(sl_xruns) - SLX0 ))" "$per" | tee -a "$LOG"
done

mapfile -t S < <(last_stats)
{
echo
echo "=== Résumé sur $DUR s"
echo "Dérive horloge PiPedal vs JACK (envoi) : $(printf '%s\n' "${DRIFTS[@]}" | awk '{s+=$1; if(NR==1||$1<m)m=$1; if(NR==1||$1>M)M=$1} END{printf "moyenne %+.1f ppm, min %+.1f, max %+.1f", s/NR, m, M}')"
echo "Sous-débits envoi : +$(( $(num_after "${S[1]}" "sous-débits") - UND0 ))   retour : +$(( $(num_after "${S[2]}" "sous-débits") - UNDR0 ))   sur-débits retour : +$(( $(num_after "${S[2]}" "sur-débits") - OVR0 ))"
echo "xruns JACK (vus par le pont) : +$(( $(echo "${S[0]}" | awk '{print $1}') - JX0 ))   xruns vus par SooperLooper : +$(( $(sl_xruns) - SLX0 ))"
echo "Température max : $TMAX °C   bridage (get_throttled) : $(throttled)"
echo "CPU moyen par programme (% d'un cœur) :"
for n in $PROCS; do
  printf "  %-13s %5.1f %%\n" "$n" "$(awk -v a="$(ticks "$n")" -v b="${T0[$n]}" -v h="$HZ" -v s="$DUR" 'BEGIN{print 100*(a-b)/h/s}')"
done
echo "Journal complet : $LOG"
} | tee -a "$LOG"
