#!/bin/sh
# The pacer A/B on a simulated clock (round 11): builds tools/pacer_sim.c around the pacer
# section of port_main.c (the exact code, cut between its "pacer" and "One line a second"
# comments), runs each scenario with FZX_PACER=0 (old) and 1 (new) and prints the pictures
# shown per second around VI 1500. Host cc; no ROM. Output in refs/pacer/sim/.
cd "$(dirname "$0")/.." || exit 1
O=refs/pacer/sim; mkdir -p $O
sed -n '/^\/\* -* pacer ---/,/^\/\* One line a second/p' game/src/port/port_main.c | sed '$d' > $O/pacer_snip.c
cc -O1 -I$O -o $O/pacer_sim tools/pacer_sim.c || exit 1
run() { # name picture_us hitch_from hitch_to hitch_us jitter_pct
    for P in 0 1 2; do # the round-1 pacer, round 11's, round 12's with the uneven cadences (FZX_CADENCE=1)
        FZX_PACER=$([ $P = 0 ] && echo 0 || echo 1) FZX_CADENCE=$([ $P = 2 ] && echo 1 || echo 0) \
            $O/pacer_sim $2 $3 $4 $5 $6 2400 > $O/$1_p$P.txt 2> $O/$1_p$P.sec
        awk -v n="$1" -v p=$P '
            { d[$1] = $2; v[$1] = $3 }
            END {
                s = sprintf("%-14s %s", n, p == 2 ? "unev" : p ? "new" : "old");
                for (k = 23; k < 33; k++) { c = 0; for (i = k * 60 + 1; i <= k * 60 + 60; i++) c += d[i]; s = s sprintf(" %2d/%-3s", c, v[k * 60 + 60]) }
                t = 0; for (i = 1441; i <= 2400; i++) t += d[i];
                print s "  shown(1441-2400)=" t }' $O/$1_p$P.txt
    done
}
echo "pictures a second / div (ticks per picture), seconds 23-32 (the hitch is at VI 1500)"
run dev36_steady 36000 0 0 0 10
run dev36_spike 36000 1500 1500 99000 10
run dev20_spike 20000 1500 1500 99000 10
run dev20_stretch 20000 1500 1800 40000 10
run dev12_spike 12000 1500 1500 60000 10
run edge28 28000 0 0 0 15      # at the div 2/3 edge, random costs
run alt30 30000 0 0 0 -15      # costs alternating 25.5/34.5 ms
run pic14 14000 0 0 0 10     # round 12: fits 60
run pic18 18000 0 0 0 10     # fits 45 (1, 1, 2)
run pic21 21000 0 0 0 10     # fits 40 (1, 2)
run pic25 25000 0 0 0 10     # fits only 30
# round 15: the fine cadences (52.5/50/48 fps) around the device's race picture cost
run pic15 15500 0 0 0 10
run pic16 16300 0 0 0 10
run pic17 17000 0 0 0 10
