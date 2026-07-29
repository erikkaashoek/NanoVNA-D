# tinyPFA

Phase/Frequency Analyzer firmware derived from NanoVNA-H4 / DiSlord's NanoVNA-D.
Target: **STM32F303xC** (Cortex-M4 @ 72 MHz), ChibiOS 19, TLV320AIC3204 codec over I2S.

## Build

The toolchain lives in ChibiStudio (same as tinyGPSDO):

```
PATH = C:\ChibiStudio\tools\GNU Tools ARM Embedded\11.3 2022.08\bin   (arm-none-eabi-gcc 11.3.1)
       C:\ChibiStudio\tools\msys2\usr\bin                             (GNU Make 4.3)
make -j8
```

**Run make from PowerShell, not Git Bash.** MSYS2's `make.exe` and Git Bash both load a
Cygwin runtime; mixing them aborts with `cygheap base mismatch detected`.

The Makefile forces `TARGET=F303`; artifacts land in `build/`.

## Code size and memory are the primary constraint

**Write extremely RAM- and ROM-efficient code. RAM and ROM are the binding limit
on every new feature — not CPU time, not elegance.**

Budget on this target, measured from `build/ch.map` (re-measure after any
sizeable change):

| Resource | Total | Used | Free |
|---|---|---|---|
| Flash (`flash0`) | 142 KB | 94,900 B (65.3%) | ~47 KB |
| SRAM (`ram0`) | 40 KB | 36,720 B | 4,240 B |
| USB packet memory (PMA) | 512 B | 500 B | 12 B |

The streaming modes cost ~1,900 B flash and 800 B of `.bss` (almost all of it
the shared `stream_buffer`). The PMA figure is exact and enforced by a
`_Static_assert` in [usbcfg.c](usbcfg.c).

Read the free-RAM figure as `__ram0_free__` in `build/ch.map`, **not** from the
linker's `ram0 100.00%` line or from `arm-none-eabi-size`. The heap section is
declared out to the end of RAM, so the linker always reports ram0 as full and
`size` folds that region into `bss` (it prints ~40 KB of bss for a 40 KB chip).
Neither number means anything here.

The heap is disabled (`CH_CFG_USE_HEAP FALSE`), so the region above `__ram0_free__`
is genuinely free for `.bss` growth — but it is the only slack there is, and two
things live at the top of it:

- `BOOT_FROM_SYTEM_MEMORY_MAGIC_ADDRESS` is **0x20009FF0**, 16 bytes below the
  end of RAM. `.bss` must never grow past it or the DFU handoff in
  `__early_init` breaks silently.
- The stacks are at the *bottom* of RAM (0x20000000–0x20000300), not the top, so
  they are not the thing that constrains `.bss`.

Before adding anything, assume it has to pay for itself in bytes. In practice:

- Prefer `const` tables in flash over generated code; prefer patch tables over
  duplicated full configuration blocks.
- Reuse existing buffers (`rx_buffer`, `spi_buffer`) rather than adding new ones.
  A new static array is a real cost — justify it.
- Prefer `uint8_t`/`uint16_t` over `int` for stored state. Bitfields and packed
  enums over one-flag-per-byte.
- Avoid `float`/`double` formatting paths and stdio; the project has its own
  compact `chprintf`/`my_atoi` replacements — use them.
- Guard optional features behind `#define` switches in [nanovna.h](nanovna.h) so
  they can be compiled out, and follow the existing `__FEATURE__` naming.
- Do not pull in new ChibiOS subsystems (`halconf.h`) casually — each one costs
  both flash and RAM.
- `-Og` is deliberate; do not change optimization flags to work around size.

Check the cost of a change with `arm-none-eabi-size build/ch.elf` and the
`--print-memory-usage` output the linker already emits.

## USB packet memory is the tightest resource of all

The STM32F303xC has **512 bytes** of USB PMA, and ChibiOS's allocator is a bump
allocator reset only at bus reset ([hal_usb_lld.c](ChibiOS/os/hal/ports/STM32/LLD/USBv1/hal_usb_lld.c)
`usb_pm_reset`). Consequences:

- Endpoints cannot be allocated lazily on alt-setting changes — `pmnext` only
  moves forward, so repeated `usbInitEndpointI()` calls leak PMA until the
  `"PMA overflow"` assert fires. Allocate every endpoint exactly once per bus
  reset; `usb_event()` guards this with a flag, because `SET_CONFIGURATION` can
  arrive more than once between resets.
- `usbDisableEndpointsI()` is **not** a way to rewind the allocator: it calls
  `usb_pm_reset()` without re-allocating EP0, so the next endpoint allocated
  lands on top of the control buffers.
- The current allocation is exact and has 12 bytes spare. Any new endpoint, or
  any increase to an existing `wMaxPacketSize`, must be paid for by shrinking
  another. See the table in [usbcfg.c](usbcfg.c).

## Streaming modes

Three runtime modes, selected from CONFIG → MODE (see `stream_mode` in
[main.c](main.c)). The mode is deliberately **not persisted** — the device always
boots into NORMAL so it can never come up looking hung with the DSP stopped.

- `STREAM_NORMAL` — codec at 192 kHz, DSP runs. Default.
- `STREAM_BULK` — codec at 192 kHz, DSP stopped, raw I2S blocks over the CDC
  with a sequence/drop header so the host can detect gaps.
  **`stream_process()` must send a burst per Thread1 iteration**, not one block.
  The ISR only ever holds one block, so one-per-iteration caps the stream at the
  UI loop rate; that sits near 500 Hz against a 1 kHz ISR, so every other block
  was dropped and no two blocks were ever contiguous — which silently destroys
  any FFT resolution better than 1 kHz. With `STREAM_BURST` 16: 983 blocks/s,
  63 gaps in ~20000 blocks, contiguous runs up to 5000 blocks (5 s, 0.2 Hz bins).
  Blocks are only contiguous when `seq` advances by one **and** `drops` is
  unchanged; `seq` counts sent blocks, not elapsed ones.
- `STREAM_AUDIO` — codec at 48 kHz, DSP stopped, raw samples out the USB Audio
  Class 1.0 isochronous endpoint. 48 kHz is the ceiling the PMA budget allows;
  192 kHz stereo would need 768 B/frame and cannot fit.

### The audio Input Terminal must NOT declare itself a microphone

`wTerminalType` on the UAC Input Terminal is **0x0603 (Line connector)**, not
0x0201 (Microphone). This is load-bearing, not cosmetic.

Windows applies microphone-class processing — AGC, noise suppression, echo
cancellation — to any capture endpoint that declares itself a microphone. With
0x0201 the captured level wandered continuously and the data was useless for
measurement. With 0x0603 Windows sets `PKEY_AudioEndpoint_FormFactor = 2`
(LineLevel), applies none of it, and the level is stable.

Nothing on the device was ever at fault: the AIC3204's own AGC block (page 0
registers 0x56–0x65) is never written and sits disabled at its reset default,
`do_agc()` is unreachable while streaming (both call sites are inside `sweep()`,
which does not run in a streaming mode), and `usb_audio_fill()` does not scale.

Related: the Input Terminal's `iTerminal` string names the endpoint. With
`iTerminal = 0` Windows falls back to calling it "Microphone" even when the form
factor is LineLevel.

**Any descriptor change that must be visible to Windows needs a `bcdDevice`
bump.** Windows caches per hardware ID (VID/PID/REV), including the capture
endpoint's cached name and form factor under `MMDevices\Audio\Capture`. Without a
new REV it serves the stale set and the change looks like it did nothing.
`idProduct` stays 0x5740 — [python/nanovna.py](python/nanovna.py) matches on it.

### IF offset: automatic per mode, and the measured audio response

`set_stream_mode()` swaps the IF automatically: entering AUDIO saves the user's
value and switches to `USB_AUDIO_RATE/8` (6 kHz), leaving restores it. NORMAL and
BULK both run the codec at 192 kHz where `rate/8` is 24 kHz, today's default, so
only the audio path actually moves. Without this, an IF picked for 192 kHz sits
at or beyond the 24 kHz audio Nyquist and the stream carries nothing usable.

**`cmd_offset` must retune the LO.** `si5351_set_frequency_offset()` only rebuilds
the DSP table and invalidates the si5351 cache; the LO keeps the old offset until
something calls `set_frequency()`, which in a streaming mode never happens. The
shell `offset` command lacked that call, which presented as "changing the IF does
nothing" — easily mistaken for a filter problem. `menu_offset_acb` in
[ui.c](ui.c) has always done it; keep the two in step.

Measured audio-chain response, 10 MHz input, gain pinned, stepping CW so the beat
walks the band (two independent methods agreed to 0.05 dB):

| beat | 1k | 4k | 8k | 12k | 16k | 24k | 32k | 48k | 64k | 88k |
|---|---|---|---|---|---|---|---|---|---|---|
| dB | −0.01 | −0.06 | −0.29 | −0.66 | −1.15 | −2.32 | −3.62 | −6.29 | −8.80 | −14.17 |

−3 dB at ≈28 kHz, 14.2 dB p-p over 1–88 kHz, S/N 94 dB falling to 88 dB at the
top. Flat within 0.3 dB below 8 kHz. So low IF costs nothing and high IF costs
level — but S/N barely moves, because the noise is filtered along with the
signal.

The low end was measured separately with 512 ms runs (1.95 Hz bins) and band
power rather than peak bins — single-bin peaks suffer up to 1.4 dB of Hann
scalloping, which shows up as ~0.5 dB of fake scatter below 1 kHz:

| beat | 10 Hz | 15 Hz | 20 Hz | 30 Hz | 100 Hz | 1 kHz | 2 kHz |
|---|---|---|---|---|---|---|---|
| dB | −0.83 | −0.34 | −0.09 | −0.03 | −0.01 | −0.01 | 0.00 |

The high-pass corner is around **4 Hz** (first-order fit), so there is no
low-frequency limit anywhere near the usable IF range — flat within 0.03 dB from
30 Hz up. The noise floor does rise towards DC (−101.8 dBFS at 2 kHz, −94.2 dBFS
at 10 Hz, 1/f), but S/N is still 97 dB at 10 Hz.

[python/pfa_stream.py](python/pfa_stream.py) divides this response out by
piecewise-linear interpolation (`FLATNESS_DB`, `--raw` to disable). Measured
across three runs it flattens a 14.2 dB tilt to 0.9 dB p-p. Run-to-run spread of
the response itself was 1.5 dB, so ±0.5 dB is about the floor without more
averaging — the table is the mean of two runs. Corrected levels can read above
0 dBFS; that is the level referred to a flat chain, not clipping.

The codec rate switch reprograms only the rate-dependent AIC3204 registers via a
patch table, and stops/restarts the I2S so channel (L/R) alignment cannot slip.

## DFU (F303): jump directly, never via reset

`enter_dfu()` on F303 jumps straight into the system bootloader ROM. The
magic-word-in-RAM plus `NVIC_SystemReset()` route that works on F072 does **not**
work here — proved with a boot breadcrumb: after a `dfu` the recorded value is
`0x00000000`, which `__early_init` only writes when it *does* take the branch. So
the magic survives, the branch is taken, the jump runs, and the device then
resets back into the application. Do not spend time on that path again.

The direct jump needs all of the following, in this order:

1. **`usbDisconnectBus()` and `chThdSleepMilliseconds(50)` while the kernel is
   still running.** Releasing the D+ pull-up (`SYSCFG_PMC_USB_PU` — this part has
   no `BCDR`) is what makes the host see a real detach and re-enumerate the ROM.
   Sleeping *after* `__disable_irq()` deadlocks: no SysTick, no scheduler, so the
   jump never executes and the board just vanishes off USB.
2. `__disable_irq()`, then stop SysTick and clear/disable every NVIC line.
3. **Reset the APB peripherals** (`APB1RSTR`/`APB2RSTR` pulsed, then clear
   `APB1ENR`/`APB2ENR`). The ROM's USB init assumes it starts from reset; leaving
   `USBEN` set with the peripheral configured stops it attaching. Leave AHB alone
   so PA11/PA12 keep their USB alternate function.
4. Clock tree back to HSI with the PLL off — the ROM reconfigures the PLL for USB
   itself and should not inherit 72 MHz.
5. Enable the SYSCFG clock again (step 3 cleared it), set `MEM_MODE` to map system
   memory at 0, set `SCB->VTOR`, `__enable_irq()`, load MSP from the ROM vector
   table and jump.

Interrupts must be **enabled** at the jump: the ROM services USB from them.

`dfu status` prints the boot breadcrumb and the ROM vector table. Known-good
values on this part: system memory `0x1FFFD800`, MSP `0x20001258`, entry
`0x1FFFF1BD`. If MSP/entry ever read `0xFFFFFFFF`, system memory is not readable
and no jump can work.

Flashing end to end, no manual button pressing:

```
dfu                                     # over the VCP
dfu-util -a 0 -s 0x08000000:leave -D build/ch.bin
```

## ADC operating point, and the AGC trap

**`do_agc()` overwrites the MICPGA gain at the end of every sweep.** Any `gain N`
issued in NORMAL mode is undone within milliseconds, silently. Set the gain
*after* entering a streaming mode, where `sweep()` and therefore `do_agc()` never
run. Symptom if you get this wrong: every gain setting measures identically,
because you are always measuring AGC's choice.

`ENABLE_GAIN_COMMAND` and `ENABLE_I2C_COMMAND` are now enabled. `i2c page reg`
reads a codec register back (`i2c page reg data` still writes) — use it to
confirm a setting landed before trusting a measurement. The shell answers
`gain?` for a command that is compiled out; that is not an argument error.

Measured C/N density vs MICPGA gain, IF at band centre (48 kHz), 192 kHz stream,
fixed −10 dBm mixer level, ENBW-corrected:

| gain | 0 | 24 | 36 | 40 | 48 | 54 | 56 | 58 |
|---|---|---|---|---|---|---|---|---|
| C/N dBc/Hz | 109.5 | ~117 | 115.6 | 115.7 | 115.8 | 115.8 | 116.9 | 117.5 ⚠clip |
| headroom | 26 dB | 15 | 11.3 | 9.5 | 4.2 | 1.3 | 0.7 | — |

**C/N is flat within 0.2 dB from gain 36 upwards.** The carrier and the noise
floor rise together, so above the knee (~gain 30–36) the PGA buys nothing — the
mixer's own output noise dominates the ADC. The apparent improvement at 56/58 is
a clipping artifact; do not trust a "best C/N" row that is flagged CLIP.

**Use `gain 40` (20 dB)**: within 0.2 dB of the best achievable C/N with 9.5 dB
of headroom. Implied single-channel floor ≈ −113 dBc/Hz. AGC's own choice sits
near −99 dBc/Hz because it rails (see the cross-wiring note below).

**The even/odd sample to physical channel mapping is NOT stable.** The two
channels swapped roles between runs (ref 120.4/sig 115.6, later ref 115.8/sig
120.3). In PCM/DSP mode the I2S DMA can start on either half of a frame, so an
I2S restart or a power cycle can swap L/R. Do not assume even = ref; check it.

### L/R swapped across an I2S restart: a stale half word in SPI2->DR

**Root cause.** `initI2S()` arms the DMA *before* configuring SPI2, then sets
`SPI_CR2_RXDMAEN`. If a received half word is still sitting in `SPI2->DR` with
`RXNE` set — `stopI2S()` disables I2S but cannot drain DR — it is transferred the
instant `RXDMAEN` is set, lands in `rx_buffer[0]`, and shifts every sample after
it by one. L and R swap. Whether a stale word is present depends on where in the
frame the I2S happened to be disabled, so it presents as a coin flip.

**Fix:** `rccResetSPI2()` in `initI2S()` immediately after `rccEnableSPI2()`,
before `RXDMAEN`. Verified: `i2s realigns 0` at boot and across repeated
AUDIO↔NORMAL round trips, and the mapping measured stable through the USB data
path over three restarts.

Do **not** rely on the I2S resynchronising by itself — this is a DMA/peripheral
state problem, not a frame-sync problem.

Historic (before the fix): three restarts gave even←left, even←left, even←right.

The `dsp.c` / `plot.c` / `do_agc()` wiring is *correct* (even = ref = `amp_a`,
corrected and driven by `l_gain` = reg 0x3B). But when the alignment lands the
other way, `do_agc()` drives each channel's gain from the *other* channel's
error — positive feedback — and the loop runs to 0/90 with one channel clipped
instead of settling. That is why the AGC misbehaved only sometimes, and it also
silently swaps ref and sig for measurement purposes.

`i2s_align()` in [main.c](main.c) pins it: drive the two MICPGAs 47 dB apart
(their own noise differs by ~36 dB, so no input signal is needed), see which
sample index follows, and restart the I2S until the alignment matches what
`dsp.c` assumes. Call it after **every** `initI2S()` — boot and the codec rate
switch in `set_stream_mode()`. Verified stable across restarts afterwards.

Do not "fix" this by swapping constants in `dsp.c` or `plot.c`; the wiring is
right and a fixed swap would be wrong half the time.

### AGC target and manual override

`ADC_TARGET_LEVEL` is **6** (was 12, which settled the loop at gain ~52 with
only ~3 dB of headroom). The loop drives `level_a` to this value and `level_a`
tracks gain 1:1, so 6 dB lower target = 12 fewer gain steps. With the alignment
pinned the AGC now settles at **gain 40/38**, measured C/N 115.5 (ref) and 119.6
(sig) dBc/Hz with 8–9 dB of headroom — within 0.4 dB of the best achievable.

`gain N` now also clears `agc_enabled`, so a deliberate operating point sticks in
NORMAL mode; `gain auto` hands control back. `gain` with no argument reports the
current gains and the AGC state.

`cmd_gain` had two bugs, now fixed: `argc == 0 && argc > 2` (never true, so a bare
`gain` read `argv[0]` out of bounds) and `argc == 3` where it meant `2`, so the
second per-channel argument was ignored.

`ENABLE_STAT_COMMAND` does **not** compile — `cmd_stat` uses `DSP_WAIT`, whose
expansion references `break_on_operation`, a parameter of `sweep()`.

## Conventions

- Follow the existing brace/indent style (2 spaces, K&R). `.clang-format` exists
  but the tree is not uniformly formatted — match the surrounding file.
- Shell commands: `VNA_SHELL_FUNCTION(cmd_x)` plus an entry in the command table
  in [main.c](main.c). Keep help strings short; they cost flash.
- Menus: `menuitem_t` tables in [ui.c](ui.c), `MT_ADV_CALLBACK` when the label is
  dynamic.
