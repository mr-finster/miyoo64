# Sleep and wake

How the games keep their speed when the Miyoo sleeps and wakes (fixed in 1.1.1, all ten games).

## The symptom

Up to 1.1.0, putting the Mini to sleep in any game and waking it made the game slow, with choppy sound,
until the game was restarted.

## What Onion's sleep does

A short press of POWER in Onion runs its sleep (keymon):

1. It stops every running program with SIGSTOP, the game included, and turns the sound down.
2. It asks `cpuclock` for the current clock and saves it with the CPU governor. It then sets
   `scaling_min_freq` to 0 and the governor to `powersave`, and turns the screen off.
3. On wake it runs `cpuclock <saved clock>`, writes the saved governor back, turns the screen on and sends
   SIGCONT to everything it stopped.

The games run their CPU faster than the stock 1.2 GHz: 1500 MHz on the Mini and 1800 MHz on the Mini Plus. To do
that they set the governor to `userspace`, write `scaling_setspeed` and program the CPU's PLL directly,
in the same way as Onion's `cpuclock`. Onion's restore did not bring that back. On a Mini the game came back
from sleep with `scaling_setspeed` at 400000 and the PLL at 400 MHz, still under `userspace`, so nothing
raised the clock again.

## What the device log showed

A diagnostic build of Mario Kart 64 logged the clock and the frame times around a sleep:

    WAKE before: gov userspace cur 1200000 min 400000 max 1200000 setspeed 1200000 kHz, PLL 1500 MHz
    WAKE after:  gov userspace cur 400000 min 400000 max 1200000 setspeed 400000 kHz, PLL 400 MHz

After the wake, a frame took 25 ms to draw instead of 9, both cores were at 100%, and the sound got about
24,000 samples a second out of the 32,000 it needs. The governor, the second core and the screen mode were
unchanged.

## The fix

The shared kit's `miyoo_clock.c` (in every game) starts a small watch thread when it raises the clock:

- The thread wakes every 250 ms.
- Once a second, it checks that the governor, `scaling_setspeed` and the PLL are still what the game set,
  and that cpu1 is online where the system has `cpu1/online`. If the boost was left off, it keeps the
  governor that `launch.sh` set.
- It runs the check at once when it sees a wake: its own 250 ms tick came more than 1 s late (the process
  was stopped), or `CLOCK_BOOTTIME` ran more than 1 s ahead of `CLOCK_MONOTONIC` (the system slept).
- It puts back only what drifted, in the same order as the boost: the governor, then `scaling_setspeed`
  (cpufreq's own step to the top of its table), then the PLL. It logs one line:

      wake: setspeed 400000->1200000 kHz, clock 400->1500 MHz

The game's exit puts the clock back as before (`miyoo_clock_restore`), and the watch stops there.

Mario Kart 64 and Diddy Kong Racing also have a stall watchdog, which writes a report when no picture comes
for 3 s. It now knows a sleep from a stall: after a wake it logs `WATCHDOG: a sleep or stop (... ms late),
not a stall` and starts counting again. Before, it reported a stall on every wake, and after a long one it
could unpin the game's threads from their cores.

## Switches

Set them in the game's `launch.sh` (`<GAME>` is the game's prefix: MK64, DKR, OOT, GE, FZX, SF64, PM64,
SSB64, SM64, PD):

| Variable | Effect |
|---|---|
| `<GAME>_WATCH` | Unset or `2`: put back what drifted (the default). `1`: only log the drift. `0`: no watch. |
| `<GAME>_CPUCLOCK` | The clock in MHz (the default is 1500 on the Mini, 1800 on the Mini Plus). `0` leaves the clock alone; the watch then keeps only the governor. |
| `MIYOO_SYSFS_ROOT` | Tests: read and write every sysfs path under this folder (a fake tree), so the boost and the watch run off the device. The PLL is never touched off a Mini. |

Mario Kart 64 also has `MK64_SLEEPDIAG=1` (one diagnostic line every 10 s) and `=2` (full detail), off by
default.

## Checking it on a device

Start a game, play for a bit, sleep the Mini for a few seconds, wake it and play on. Then look at
`console.log` in the game's folder (`App/<Game>/data/console.log`; Mario Kart 64:
`App/MarioKart64/console.log`). Near the top you should see:

    clock: 1200 -> 1500 MHz (model 283; MK64_CPUCLOCK=<MHz> to change, 0 to leave it)
    watch: keeping governor userspace, setspeed 1200000 kHz, clock 1500 MHz (re-applied after a sleep; MK64_WATCH=1 reports only, 0 off)

and after the wake:

    wake: setspeed 400000->1200000 kHz, clock 400->1500 MHz

On the Mini that confirmed the fix, frames drew in 7.8 ms again and the sound was back to 62.5 callbacks
and 32,013 samples a second with no underruns, 13 s after waking. If the clock was still right after a wake
(another OS, or Onion behaving differently), the line reads `wake: clock, governor and cores as set`.
