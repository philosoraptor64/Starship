#!/bin/bash
# Star Fox netplay between this Mac (host, build-cmake/Starship) and the iOS build in the iOS Simulator (guest,
# build-iossim/Starship.app): exercises the iOS code (Documents paths, netplay.cfg, Metal on iOS, the UIScene app)
# without signing. Bots drive both; then the sync checks and both per-tick hash logs are compared.
# Usage: tools/net/sim-test.sh  (env: FRAMES=1801 SEED=7 VS=1 [BOT_FROM=1500] LATENCY/JITTER/LOSS ROLLBACK=0
#        SIM=<udid> (default booted); AWAY=at:secs = the simulator app switches away for secs). Needs sf64.o2r/starship.o2r in build-cmake (copied to the app once).
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root/build-cmake"
SIM=${SIM:-booted}; BUNDLE=io.github.philosoraptor.starship; FRAMES=${FRAMES:-1801}; SEED=${SEED:-7}
out=net-test/sim-$(date +%Y%m%d-%H%M%S); mkdir -p "$out"
xcrun simctl install "$SIM" "$root/build-iossim/Starship.app" || { echo "RESULT: FAILED (install)"; exit 1; }
data=$(xcrun simctl get_app_container "$SIM" $BUNDLE data); docs="$data/Documents"; mkdir -p "$docs"
for f in sf64.o2r starship.o2r; do [ -f "$docs/$f" ] || cp "$f" "$docs/"; done
rm -f "$docs/guest.hash" "$docs/sfnet.log"
common=(SF64_SEED=$SEED SF64_REPLAY_FRAMES=$FRAMES SF64_NET_DELAY=${DELAY:-2} SF64_NET_ROLLBACK=${ROLLBACK:-1}
        SF64_NET_SIM_LATENCY=${LATENCY:-0} SF64_NET_SIM_JITTER=${JITTER:-0} SF64_NET_SIM_LOSS=${LOSS:-0})
hostx=(SF64_NET_BOT=5); guestx=(SF64_NET_BOT=105)
if [ "${VS:-0}" = 1 ]; then
    cp "$root/tools/net/vs-guest.txt" "$docs/vs-guest.txt"
    hostx+=(SF64_REPLAY_PLAY=$root/tools/net/vs-host.txt SF64_NET_BOT_FROM=${BOT_FROM:-1500})
    guestx+=(SF64_REPLAY_PLAY=$docs/vs-guest.txt SF64_NET_BOT_FROM=${BOT_FROM:-1500})
fi
{ echo "# written by tools/net/sim-test.sh"
  for kv in "${common[@]}" "${guestx[@]}" SF64_NET_PEER=127.0.0.1:7401 SF64_REPLAY_CHECK=$docs/guest.hash; do echo "$kv"; done
} > "$docs/netplay.cfg"
env SDL_AUDIODRIVER=dummy SDL_MAC_BACKGROUND_APP=1 "${common[@]}" "${hostx[@]}" SF64_NET_HOST=1 \
    SF64_REPLAY_CHECK=$PWD/$out/host.hash ./Starship -ApplePersistenceIgnoreState YES > "$out/host.log" 2>&1 &
hpid=$!
sleep 1
SIMCTL_CHILD_SDL_AUDIODRIVER=dummy xcrun simctl launch --terminate-running-process "$SIM" $BUNDLE > /dev/null
trap 'kill $hpid 2>/dev/null; xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null' INT TERM
# AWAY=at:secs: switch the simulator to the Settings app at `at` s for `secs` s (the Mac must wait, then go on)
if [ -n "${AWAY:-}" ]; then
    (sleep "${AWAY%:*}"; xcrun simctl launch "$SIM" com.apple.Preferences > /dev/null; sleep "${AWAY#*:}";
     xcrun simctl launch "$SIM" $BUNDLE > /dev/null) &
fi
wait $hpid
sleep 3
xcrun simctl terminate "$SIM" $BUNDLE 2>/dev/null
cp "$docs/sfnet.log" "$out/guest.log" 2>/dev/null; cp "$docs/guest.hash" "$out/guest.hash" 2>/dev/null
echo "# netplay off (sim-test.sh)" > "$docs/netplay.cfg"
for side in host guest; do echo "== $side"; grep -E 'SfNet: (connected|tick|DESYNC|.*Playing|away|back)|switched|is back' "$out/$side.log" | tail -5; done
for side in host guest; do
    grep -q 'SfNet: connected' "$out/$side.log" 2>/dev/null || { echo "RESULT: FAILED ($side never connected)"; exit 1; }
done
grep -q DESYNC "$out/host.log" "$out/guest.log" && { echo "RESULT: DESYNC"; exit 1; }
hn=$(wc -l < "$out/host.hash"); gn=$(wc -l < "$out/guest.hash"); n=$(( hn < gn ? hn : gn ))
same=$(cmp -s <(head -n "$n" "$out/host.hash") <(head -n "$n" "$out/guest.hash") && echo yes || echo no)
echo "per-tick state hashes over $n ticks identical: $same (Mac vs iOS)"
echo "RESULT: IN SYNC (logs in build-cmake/$out)"
