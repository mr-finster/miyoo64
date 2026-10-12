# PSP audio diagnostics

The PSP audio backend has an optional live diagnostic stream for PSPLink. It is disabled in normal builds so
`printf` traffic cannot affect release audio timing.

Build it with:

```sh
./psp.sh PSP_AUDIO_DIAGNOSTICS=1
```

or, without the wrapper:

```sh
make -j"$(nproc)" psp-port PSP_AUDIO_DIAGNOSTICS=1
```

The separate output is `build/psp-port-audio-diag/ntsc-1.0/EBOOT.PBP`. Launch that build through PSPLink and
watch its normal stdout console. Diagnostic lines begin with `[audio]`; urgent events begin with `[audio!]`.

## What is reported

The logger runs in its own low-priority `OOT PSP AudioDiag` thread. It samples every 100 ms, prints a snapshot
once per second, and never calls `printf` from `OOT PSP AudioOut` or `OOT PSP AudioGen`.

The first snapshot line reports live execution state:

- `prod`: logical `AudioGen` phase. Important values are `WAIT_ME`, `PREPARE`, `SEQUENCE`, `SUBMIT`, `TIMER`,
  `IO_BACKOFF`, and `RING_FULL`.
- `out`: logical `AudioOut` phase. `WAIT_HW` is normally healthy: `sceAudio*OutputBlocking` is holding the output
  thread until the hardware needs its next buffer. `PRIME` means it is intentionally submitting silence while
  waiting for enough buffered audio.
- `id`, `k`, `w`, and `p`: PSP thread ID, kernel state, raw kernel wait type, and current priority. A logical phase
  plus `k=WAIT` identifies where a thread was blocked rather than merely using CPU.
- `cpu`: run-clock microseconds consumed by that thread during the preceding reporting window.
- `me`: Media Engine state and progress checkpoint.
- `active` and `pending`: whether ME mixing is enabled and whether an ME job is outstanding.
- `wait`, `waiter`, and `age`: whether an Allegrex thread is currently blocked waiting for the ME, that thread's
  ID, and the current wait duration.

The normal combined job is reported as `me=SYNTH`. Its progress checkpoints are:

| Progress | ME work |
| --- | --- |
| `1` | Entering the combined job and refreshing Allegrex-owned inputs |
| `2` | Constructing the ABI command list |
| `3` | Publishing the completed list and synthesis state |
| `4` | Executing the audio mixer command list |
| `5` | Copying generated PCM into the output ring |
| `6` | Writing mixer state back for Allegrex |

The older command-only path is reported as `me=RUN`; it skips progress `2` and `3`. `me=IDLE` with progress `6`
is normal after a completed audio-generation job. Progress `256` (`0x100`) means ME boot and VME setup completed
but no job checkpoint has replaced it yet.

The second line reports buffer pressure and one-second event counts. `buf=current/target` includes ring data, an
outstanding ME queue, and frames owned by the audio driver. `ready` excludes the outstanding ME queue and is the
reserve used for urgent scheduling and I/O arbitration. `start` and `recover` are the first-start and
post-underrun priming thresholds. `ring`, `meq`, and `driver` show the queue pieces separately. `underrun`, `err`,
`fallback`, and `timeout` should remain zero. `catchup`, `late`, `io`, and `full` explain why generation did not
follow its normal 60 Hz cadence. ME wait `avg`, `last`, and `max_all` are in microseconds; `max_all` is the worst
value since boot. `late` counts producer timer deadlines missed by at least 1 ms. The producer temporarily moves
from priority `32` to priority `31` while `ready` is urgent and returns to `32` after reaching the I/O watermark.

At the default 22.05 kHz source rate, the normal target is six chunks (about 104 ms), first startup requires four
chunks (about 70 ms), and recovery after an underrun requires two chunks (about 35 ms). I/O backoff begins at five
playable chunks and urgent scheduling begins below three. I/O backoff retries within the current 60 Hz deadline;
it no longer discards an entire audio update after a single 1 ms yield. Catch-up is limited to one additional
update and only occurs after a genuinely missed producer deadline. At or below the urgent watermark, AudioGen
waits on the ME completion semaphore after submission so completed PCM is published immediately rather than at
the next 60 Hz update; Allegrex is sleeping during this wait.

The third line breaks a completed audio update into average microseconds:

- `wait_me`: Allegrex synchronization with the previous ME job.
- `prepare`: command handling, audio loads, DMA completion, and buffer preparation.
- `synth` and `seq`: Allegrex sequence/control processing. Command construction is no longer included in normal
  Allegrex phase timing.
- `cmd`: Allegrex command construction time in the CPU fallback path; it should be zero while the ME is active.
- `submit`: bounded synthesis-state cache publication and submission of the new ME job.
- `abi` and `dma`: average mixer command and sample-DMA counts per update.

An update that needs a nonresident sample DMA is intentionally preflighted onto Allegrex so the ME never enters
an I/O path. Such an update increments the CPU-mix count and can produce a nonzero `cmd` without disabling the ME.

Two additional lines profile mixer work inside ME progress `4`:

- `me-job` reports completed ME mixer jobs, command count, average and last job cost, and the most expensive
  opcode in the last and worst jobs. `max_all` is the worst profiled job since boot.
- `me-op` ranks the three opcodes that consumed the most ME time during the reporting window. The percentage is
  that opcode's share of all measured command time; `n` is its call count, `avg` is average cost per call, and
  `max_all` is its slowest single call since boot.

ME command costs use raw `tick` units from the ME hardware counter. Compare ticks and percentages within the same
build; they are deliberately not converted to microseconds because the counter rate is ME-clock dependent. For
example:

```text
[audio] me-job 1s n=36 cmd=14802 avg=811234tick last=790115tick/411cmd max_all=1520031tick/489cmd last_hot=ENVMIXER/401882tick max_hot=ENVMIXER/790441tick
[audio] me-op 1s total=27710102tick top=ENVMIXER 61.8% n=1830 avg=9360 max_all=28120 | RESAMPLE 20.4% n=1220 avg=4635 max_all=12004 | ADPCM 9.7% n=610 avg=4407 max_all=9871
```

The live state and urgent event lines also include `op=NAME#index`. While `me=SYNTH/4` or `me=RUN/4`, this is the command the ME
is executing at the instant the diagnostic thread samples it. `op=IDLE` means the most recent command list has
finished. `snapshot=BUSY` only means the ME was publishing a completed profile at that instant; the logger retries
on the next report.

## Reading a stutter

- An `[audio!] UNDERRUN` with `ready` near zero confirms starvation rather than a bad sample or mixer artifact.
  A materially larger `buf` at the same event means the missing reserve was still pending on the ME.
- High `wait_me`, live `wait=1`, and `me=SYNTH/4` point to ME mixer execution as the bottleneck. Progress `2`, `5`,
  or `6` instead isolates command construction, PCM queueing, or state writeback.
- When `me=SYNTH/4` is slow, use `me-op` total percentage to choose the first function to optimize. Use `avg` to
  distinguish an intrinsically expensive command from one that dominates only because it is called often, and
  compare `last_hot`/`max_hot` against slow-job or underrun periods.
- High `synth`, with low `wait_me`, points to Allegrex-side sequence/control work. A nonzero `cmd` indicates that
  the backend fell back to Allegrex command construction.
- High `prepare`, a waiting producer kernel state, and elevated `dma` point to sample loading or DMA completion.
- Rising `io` means foreground asset reads are causing the producer's intentional I/O backoff.
- Rising `late`/`maxlate_all` and `catchup` with low phase times point to scheduling starvation elsewhere in the
  game rather than audio work itself.
- Any `cpu` mixes, `ME_FALLBACK`, `ME_TIMEOUT`, `active=0`, or `me=FAULT` means work left the normal ME path.
- `OUTPUT_ERROR` is a PSP audio driver submission failure. `out=WAIT_HW` by itself is expected and is not a
  bottleneck.

Disable the feature for normal timing comparisons by rebuilding without `PSP_AUDIO_DIAGNOSTICS=1`.
