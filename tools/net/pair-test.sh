#!/bin/bash
# Two Star Fox copies on this Mac sharing one game over netplay (lockstep), both driven by scripted bots, then
# the sync checks and both sides' per-tick state hashes are compared.
# Usage: tools/net/pair-test.sh  (env: FRAMES=1801 SEED=7 DELAY=2 HOST_BOT=5 GUEST_BOT=105 (0 = play it yourself);
#        DESYNC_TEST=tick (the guest's RNG is nudged: must report DESYNC); SERVER=local [ROOM=SFTS] [RELAY=1];
#        EXTERNAL=1 = the connection belongs to a stand-in for i64; GUEST_FRAMES=n = the guest quits early;
#        VS=1 = scripted menus into a 2-player Versus match, bots fly from BOT_FROM=1500)
set -uo pipefail
cd "$(dirname "$0")/../../build-cmake"
FRAMES=${FRAMES:-1801}; SEED=${SEED:-7}; DELAY=${DELAY:-2}; HOST_BOT=${HOST_BOT:-5}; GUEST_BOT=${GUEST_BOT:-105}
out=net-test/$(date +%Y%m%d-%H%M%S); mkdir -p "$out"
common=(SDL_AUDIODRIVER=dummy SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1 SF64_SEED=$SEED
        SF64_REPLAY_FRAMES=$FRAMES SF64_NET_DELAY=$DELAY SF64_NET_WAIT_SECS=${WAIT_SECS:-60})
hostbot=(); [ "$HOST_BOT" != 0 ] && hostbot=(SF64_NET_BOT=$HOST_BOT)
guestbot=(); [ "$GUEST_BOT" != 0 ] && guestbot=(SF64_NET_BOT=$GUEST_BOT)
# VS=1: scripted menus into a 2-player Versus Point Match (tools/net/vs-*.txt), then the bots fly from BOT_FROM
if [ "${VS:-0}" = 1 ]; then
    hostbot+=(SF64_REPLAY_PLAY=$PWD/../tools/net/vs-host.txt SF64_NET_BOT_FROM=${BOT_FROM:-1500})
    guestbot+=(SF64_REPLAY_PLAY=$PWD/../tools/net/vs-guest.txt SF64_NET_BOT_FROM=${BOT_FROM:-1500})
fi
spid=
if [ "${EXTERNAL:-0}" = 1 ]; then
    hostnet=(SF64_NET_EXTERNAL_TEST=1 SF64_NET_EXTERNAL_BLOB=$PWD/$out/blob.bin)
    guestnet=("${hostnet[@]}")
elif [ "${SERVER:-}" = local ]; then
    python3 ../external/n64net/server/rendezvous.py --bind 127.0.0.1 --port 7790 > "$out/server.log" 2>&1 &
    spid=$!
    sleep 1
    hostnet=(SF64_NET_SERVER=127.0.0.1:7790 SF64_NET_ROOM=${ROOM:-SFTS} ${RELAY:+SF64_NET_RELAY=$RELAY})
    guestnet=("${hostnet[@]}")
else
    hostnet=()
    guestnet=(SF64_NET_PEER=127.0.0.1:7401)
fi
env "${common[@]}" ${hostbot[@]+"${hostbot[@]}"} ${hostnet[@]+"${hostnet[@]}"} SF64_NET_HOST=1 \
    SF64_REPLAY_CHECK=$PWD/$out/host.hash ./Starship -ApplePersistenceIgnoreState YES > "$out/host.log" 2>&1 &
hpid=$!
sleep 1
env "${common[@]}" ${guestbot[@]+"${guestbot[@]}"} ${guestnet[@]+"${guestnet[@]}"} \
    ${DESYNC_TEST:+SF64_NET_TEST_DESYNC=$DESYNC_TEST} ${GUEST_FRAMES:+SF64_REPLAY_FRAMES=$GUEST_FRAMES} \
    SF64_REPLAY_CHECK=$PWD/$out/guest.hash ./Starship -ApplePersistenceIgnoreState YES > "$out/guest.log" 2>&1 &
gpid=$!
trap 'kill $hpid $gpid $spid 2>/dev/null' INT TERM
wait $hpid; wait $gpid
[ -n "$spid" ] && kill $spid 2>/dev/null
for side in host guest; do
    echo "== $side"; grep -E 'SfNet: (connected|tick|DESYNC|room|the other|Lost|.*Playing)|ReplayCheck' "$out/$side.log" | tail -5
done
for side in host guest; do
    if ! grep -q 'SfNet: connected\|SfNet: running' "$out/$side.log"; then echo "RESULT: FAILED ($side never connected)"; exit 1; fi
done
if grep -q DESYNC "$out/host.log" "$out/guest.log"; then echo "RESULT: DESYNC"; exit 1; fi
n=$(wc -l < "$out/guest.hash"); same=$(head -n "$n" "$out/host.hash" | cmp -s - "$out/guest.hash" && echo yes || echo no)
echo "per-tick state hashes over $n ticks identical: $same"
awk '$3==7' "$out/host.hash" | wc -l | xargs echo "gameplay ticks:"
echo "RESULT: IN SYNC (logs in build-cmake/$out)"
