#!/usr/bin/env bash
# Network impairment for single-machine testing (needs root).
#
#   sudo tests/net/netem.sh on  <profile> [control ports...]
#   sudo tests/net/netem.sh off
#
# Applies netem to UDP on loopback (media uses ephemeral ports) and to the
# TCP control ports (default 47101 47102 = --instance 1 and 2), so the RTT
# the app measures on the control channel matches the media path. Other
# local UDP (e.g. the 127.0.0.53 DNS stub) is delayed too while this is on.
# Loopback packets pass netem once per direction: RTT = 2 x delay.
#
# Profiles:  lan    0.5ms +-0.2ms, 0.1% loss
#            wifi   4ms  +-3ms,    1% loss (25% correlated)
#            wan    20ms +-5ms,    0.5% loss
#            lossy  15ms +-5ms,    3% loss (25% correlated)
set -euo pipefail
DEV=lo
cmd=${1:-}; shift || true

off() { tc qdisc del dev $DEV root 2>/dev/null || true; }

case "$cmd" in
off) off; echo "netem off"; exit 0 ;;
on)  ;;
*)   sed -n '2,17p' "$0"; exit 1 ;;
esac

profile=${1:-lan}; shift || true
case "$profile" in
lan)   NETEM="delay 0.5ms 0.2ms distribution normal loss 0.1%" ;;
wifi)  NETEM="delay 4ms 3ms distribution normal loss 1% 25%" ;;
wan)   NETEM="delay 20ms 5ms distribution normal loss 0.5%" ;;
lossy) NETEM="delay 15ms 5ms distribution normal loss 3% 25%" ;;
*) echo "unknown profile $profile"; exit 1 ;;
esac
ports=("$@"); [ ${#ports[@]} -gt 0 ] || ports=(47101 47102)

off
# Bands 1-3 keep the default priomap; band 4 is only reached via filters.
tc qdisc add dev $DEV root handle 1: prio bands 4 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
tc qdisc add dev $DEV parent 1:4 handle 40: netem $NETEM limit 100000
tc filter add dev $DEV parent 1: protocol ip prio 1 u32 match ip protocol 17 0xff flowid 1:4
for p in "${ports[@]}"; do
    tc filter add dev $DEV parent 1: protocol ip prio 2 u32 match ip protocol 6 0xff match ip dport "$p" 0xffff flowid 1:4
    tc filter add dev $DEV parent 1: protocol ip prio 2 u32 match ip protocol 6 0xff match ip sport "$p" 0xffff flowid 1:4
done
echo "netem '$profile' on $DEV: $NETEM (UDP + TCP ports ${ports[*]})"
