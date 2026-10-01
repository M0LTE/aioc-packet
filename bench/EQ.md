# TX equaliser

A tunable equaliser in the AIOC's transmit path (USB audio OUT to the DAC, which drives the radio's mic input). It is up to three biquad filters in a row, run on every playback sample, set through new settings registers. It comes on by default with the `k5-red` profile below. Switched off, the audio is bit-for-bit what stock v1.4.1 sends. The receive equaliser is [further down](#rx-equaliser), and how the audio moves through the AIOC in 1 ms blocks is at [the end](#audio-path-dma-blocks).

Why: with an AIOC rev 1.0 into a UV-K5 (DIG path) the over-the-air response is +6.8 dB around 60 Hz and -5.5 dB at 6 kHz relative to 1 kHz. The low-end hump is the AIOC's output network, the top-end droop mostly the K5. The equaliser takes that out.

## Registers

All are ordinary AIOC settings registers, read and written through the HID feature report like the others. Writes go to RAM and are lost at power-off unless the host sets the store bit (`tools/aioc_eq.py` does that only when you add `--store`).

| Address | Name | Default | Contents |
|---|---|---|---|
| 0xB0 to 0xB4 | TXEQ section 0 | k5-red | b0, b1, b2, a1, a2 |
| 0xB5 to 0xB9 | TXEQ section 1 | k5-red | b0, b1, b2, a1, a2 |
| 0xBA to 0xBE | TXEQ section 2 | k5-red | b0, b1, b2, a1, a2 |
| 0xBF | TXEQ_CTRL | 0xBB801303 | bits 0-1 NSECT, bits 8-15 GEN, bits 16-31 FS |
| 0xC8 | INFO_TXEQ (read only) | 0 | bits 0-1 sections in use, bit 2 running, bit 3 change in progress, bits 8-15 GEN in use, bits 16-31 saturation count |
| 0xC9 | INFO_TXEQPAGE (read only) | "TXEQ" (0x51455854) | settings page marker, see below |

- **Coefficients**: signed 32 bit, Q3.29, so 1.0 is 0x20000000 and the range is -4 to +4. Each section computes `y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2` (a0 is 1; a1 and a2 have the usual sign, as scipy and the RBJ cookbook give them).
- **NSECT**: how many sections are used, from section 0 up. 0 means off.
- **GEN**: a tag with no meaning of its own. The firmware takes over a new coefficient set only when TXEQ_CTRL changes value, so change GEN every time you apply a new set.
- **FS**: the sample rate in Hz the coefficients were designed for. If playback runs at any other rate the equaliser stays off. 0 means use them at any rate.
- **Saturation count**: how many samples were clipped (inside the filter or at the output) since the last change. It stops at 65535. 0xC8 is only updated while playback is running.

The addresses were unused in v1.4.1 and in every upstream branch (development, autoptt, hwcos, the k1-aioc-rev1.x branches). A settings page stored by v1.4.1 holds zero at all of them.

## Stored settings and the equaliser

Which EQ registers the AIOC loads at power-up depends on the settings page in flash (`settings_page.c`):

- **Nothing stored** (the page is erased, as after flashing a full image): the firmware defaults, so `k5-red` on.
- **A page with 0xC9 = "TXEQ"**: this firmware always holds that marker in RAM, so every page it stores carries it. The EQ registers load exactly as stored, off included.
- **A page with a nonzero TXEQ_CTRL but no marker**: stored by v1.4.1-packet.1 with a set loaded. Loads as stored.
- **Any other page**: stored by firmware without the equaliser (stock v1.4.x), or by v1.4.1-packet.1 with it off. The rest of the page loads as stored and the EQ registers get the defaults, so `k5-red` comes on.

The marker is read only, so the host cannot clear or forge it. The page is copied into RAM from address 0 upwards, so TXEQ_CTRL is always written after the coefficients, as when the host loads a set. A developer build with `make TXEQ_DEFAULT=off` has off as its defaults, and the same rules apply.

## How a change is applied

1. The host writes the coefficient registers, in any order. Nothing happens yet.
2. The host writes TXEQ_CTRL with a new GEN. At the start of the next 1 ms block the firmware copies all 15 coefficients at once, so it never runs a half-written set.
3. The new set runs unheard alongside the old one for 4096 samples (85 ms at 48 kHz), so its start-up transient dies away, then the output crossfades from old to new over 512 samples (10.7 ms).

Switching on (from off) and switching off go through the same crossfade. While off, the output is the input, untouched. At the start of every playback stream the set in the registers is installed straight away, with no crossfade and a clean history. If a new set is committed while a change is still in progress, it waits for that change to finish; words written after a commit never leak into the committed set.

Virtual PTT still looks at the unfiltered audio, so its threshold behaves exactly as before.

## Numbers inside

The filters are direct form I in fixed point: 64-bit accumulate (single-cycle SMLAL on the Cortex-M4, no FPU use), 12 bits below the 16-bit sample LSB, and 12 dB of headroom between sections. Every section carries its rounding remainder into the next sample, which stops low-frequency sections from holding a stuck offset or a limit cycle. Every intermediate value and the output saturate instead of wrapping.

The firmware runs each section over a whole 1 ms block in turn, with its coefficients and state in registers; the result is bit for bit that of running the sections sample by sample. From the disassembly that is about 135 cycles per sample for three sections, about 6500 cycles per block at 48 kHz (9% of the 72 MHz core). During a change it goes sample by sample through both sets, about three times that for the 95 ms the change takes. Register 0xCE shows what the whole playback block really takes.

## Designing and applying a set

`bench/eq.py` (needs numpy) fits three sections to the measured response, scales the result so the equaliser never has gain above -0.1 dB at any frequency (it only cuts, so a full-scale input cannot clip), quantises, prints the predicted response, the register values and a `tools/aioc_eq.py apply custom` command that loads them.

```
python3 bench/eq.py                              # fit to the built-in measured K5 response
python3 bench/eq.py --measured my.csv            # fit to your own "hz,db" table
python3 bench/eq.py --section peak:63:-6:0.42 --section peak:6500:6.4:0.9
python3 bench/eq.py --gen 42                     # choose the GEN tag
python3 bench/eq.py --bypass                     # the one write that switches it off
```

Because it only cuts, the level in the passband drops (about 5.7 dB for the proposed set). Make that up with the K5's deviation register or by driving the AIOC harder from the host.

Check it took: `tools/aioc_eq.py status` while audio is playing should say it is running with the GEN you wrote and no clipping. In register terms, 0xC8 shows the GEN in bits 8-15, bit 2 set, and a saturation count of 0.

## The k5-red profile

This is what `tools/aioc_eq.py apply k5-red` loads and what the firmware has as its default. It came from `eq.py` with no options, for a red rev 1.0 AIOC into a UV-K5 running the packet firmware, measured over the air on 2026-09-28 (deviation 0x862, -12 dBFS):

| Section | Type | Frequency | Gain | Q |
|---|---|---|---|---|
| 0 | peak | 63.5 Hz | -6.0 dB | 0.42 |
| 1 | peak | 6500 Hz | +6.4 dB | 0.91 |
| 2 | peak | 1076 Hz | +0.8 dB | 1.2 |

then scaled by -6.55 dB overall. Gain at 1 kHz is -5.74 dB.

| Hz | Measured | EQ | Predicted |
|---|---|---|---|
| 20 | +3.0 | -3.31 | -0.31 |
| 31.5 | +5.5 | -5.05 | +0.45 |
| 50 | +6.7 | -6.55 | +0.15 |
| 63 | +6.8 | -6.80 | 0.00 |
| 100 | +5.9 | -5.96 | -0.06 |
| 160 | +4.3 | -4.14 | +0.16 |
| 250 | +2.7 | -2.59 | +0.11 |
| 315 | +2.1 | -2.00 | +0.10 |
| 500 | +1.2 | -1.17 | +0.03 |
| 800 | +0.6 | -0.40 | +0.20 |
| 1000 | 0 | 0 | 0 |
| 1600 | +0.1 | +0.01 | +0.11 |
| 2000 | +0.1 | +0.12 | +0.22 |
| 2500 | -0.5 | +0.48 | -0.02 |
| 3150 | -1.1 | +1.19 | +0.09 |
| 4000 | -2.4 | +2.45 | +0.05 |
| 5000 | -3.8 | +4.17 | +0.37 |
| 6000 | -5.5 | +5.46 | -0.04 |

Spread from 300 Hz to 3.2 kHz goes from 3.2 dB to 0.24 dB. Above 6 kHz the lift falls away again (+4.5 dB at 8 kHz, +1 dB at 12 kHz, back to about 0 at 16 kHz, all relative to 1 kHz). The third section mostly chases the 1 kHz reference point; `--sections 2` gives about 0.3 dB of spread with one section fewer.

Measured over the air with this set loaded, the response was within -0.64 dB and +0.42 dB of 1 kHz from 20 Hz to 6 kHz.

The register values, and what 0xC8 reads while it runs at 48 kHz:

```
0xB0 0x1FC74BD8   0xB5 0x12B8B2CD   0xBA 0x2029FDF6
0xB1 0xC0E42580   0xB6 0xF090A649   0xBB 0xC3FC2B77
0xB2 0x1F551D69   0xB7 0x04B022DF   0xBC 0x1C737807
0xB3 0xC0E42580   0xB8 0xDF2C6FBF   0xBD 0xC3FC2B77
0xB4 0x1F1C6941   0xB9 0x11C94E12   0xBE 0x1C9D75FC
0xBF 0xBB801303   (FS 48000, GEN 0x13, 3 sections)
0xC8 0x00001307   (read only, while playing)
```

## Loading a set from your own software

Everything goes through the AIOC's HID interface (USB 1209:7388 unless you changed it, interface 3), with the same feature report every AIOC setting uses. The report is 6 bytes, `[control, address, d0, d1, d2, d3]`, value little-endian; the device has no report IDs, so hidapi and most HID libraries want an extra 0 byte in front.

- **Write** a register (RAM only): control 0x01.
- **Read** a register: send control 0x00 with the address, then get the feature report; it comes back as `[0x00, address, d0, d1, d2, d3]`.
- **Store** everything to flash: control 0x80. Leave this to the user: it writes the whole settings page as it stands in RAM, other settings included.

To load a set: write 0xB0 to 0xBE first, then 0xBF. The control word commits the coefficients, and only when its value changes, so if 0xBF already holds the value you want but the coefficients differ, write 0 to 0xBF first. To switch off, write 0 to 0xBF. Read 0xB0 to 0xBF back to check, and read 0xC8 during playback to see it running. Leave every other register alone.

## Reverting

- Switch it off: `tools/aioc_eq.py off` (a 10 ms fade, then bit-exact stock audio). Add `--store` to keep it off over power-off, since it is on by default.
- Or flash stock AIOC firmware back (see the README).

## Tests

`make -C bench test` compiles the firmware's own `tx_eq.c` and `settings_page.c` with the host gcc (with the undefined-behaviour and address sanitizers) and checks: processing in blocks (of 48, as the firmware does, and of 1, 2, 7, 13, 22, 32, 47, 49 and 100 samples) gives bit for bit the output and the internal state of processing sample by sample, through bypass, set changes, saturation inside a section and at the output, and switching off and on; bypass is bit-exact (defaults, NSECT 0, wrong sample rate, and after switching off); the fixed-point output matches a double-precision reference to within the 16-bit rounding (gain within 0.01 dB, error under 0.36 LSB rms, 20 Hz to 12 kHz); saturation clamps with no wraparound, at the output and inside; extreme coefficients cannot overflow; silence decays to exact zero with no limit cycle; a set written word by word during playback never leaks before its commit, and switching sets, switching on and switching off cause no step or overshoot beyond the steady signals. For the settings page, built both ways (defaults `k5-red` and off), it checks the rules above for stock, v1.4.1-packet.1 and marked pages, that everything outside the EQ block loads as stored, and that store then recall gives back the same registers, a stored "off" included.

## Flashing

See the README for flashing, backing up and recovery. Two details for people building their own images:

- The full `.bin` is 128000 bytes and ends with the settings page at 0x0801F000 filled with 0xFF. Upstream does this on purpose: flashing it wipes the stored settings, and the AIOC boots with firmware defaults, the equaliser on included. That includes the PTT mapping, register 0x24, whose default adds serial DTR-and-not-RTS as a PTT source.
- An image without the settings page leaves the stored settings as they are, with the EQ registers following the rules in "Stored settings and the equaliser": `arm-none-eabi-objcopy -O binary -R .eeprom aioc-fw.elf aioc-fw-keep-settings.bin`, which `make` also produces. DFU only erases the pages it writes.

# RX equaliser

A fixed equaliser in the AIOC's receive path (the ADC, which hears the radio's speaker output, to USB audio IN). It undoes the UV-K5's receive audio high-pass, so that 9600 baud FSK decodes. It comes on by default. Switched off, the received audio is bit-for-bit what stock v1.4.1 sends.

Why: the K5's audio output stage rolls off below about 128 Hz like two first-order high-passes at the same frequency (second order, Q 0.5). Its loss is mild, but its phase shift smears 9600 baud FSK: in recordings of the K5's audio, 0 of 70 frames decoded, while the same transmissions decoded from a flat SDR. Run offline on those recordings (the model of the same filter), this equaliser gave 70 of 70 on three decoders, and left 1200 baud AFSK and 3600 baud QPSK decoding as before. Live on the bench, with this firmware in the AIOC, 9600 baud went from 0 of 60 frames to 60 of 60 on three decoders. The AIOC's input (rev 1.0: a high-pass at about 7 Hz) is not the cause, so the correction is the radio's and suits any AIOC board.

## Registers

| Address | Name | Default | Contents |
|---|---|---|---|
| 0xAF | RXEQ_CTRL | 1 | 0 off, 1 the UV-K5 profile; any other value is reserved and means off |
| 0xCA | INFO_RXEQ (read only) | 0 | bits 0-7 profile in use, bit 8 running, bit 9 change in progress, bit 10 switched off by the overload guard, bits 11-15 times the guard tripped since the AIOC last started (power-up or any reset), bits 16-31 saturation count |
| 0xCB | INFO_RXEQPAGE (read only) | "RXEQ" (0x51455852) | settings page marker, see below |
| 0xCC | INFO_RXEQCYC (read only) | 0 | bits 0-15 the most CPU cycles per sample in any one block, bits 16-31 the average per sample over the last 4096 samples or more |

- **Running** means the profile is in use and heard: it is on and the host records at 48000 Hz. At any other rate the equaliser stays off, and the profile field reads 0.
- **Saturation count**: output samples clipped to the 16-bit range since recording started or the last change. It stops at 65535. The equaliser boosts low frequencies (up to about +9 dB below 100 Hz), so a very loud, bassy input can clip.
- **Cycles**: measured on the Cortex-M4's cycle counter around the equaliser in every 1 ms block, at 72 MHz, and divided by the block's length, so the maximum is the most expensive block's average. One sample period at 48 kHz is 1500 cycles. The maximum restarts with each recording and stops at 65535.

The status registers change only while recording; they read zero after power-up until recording starts. 0xAF was unused in v1.4.1, in every upstream branch and in earlier aioc-packet releases. It sits directly below the TX block, as far as possible from the fox hunt registers (0xA0 to 0xA5).

`tools/aioc_eq.py rx status`, `rx on` and `rx off` (with `--store` to keep it) read and write these. From your own software, write 0xAF like any other register (see "Loading a set from your own software" above).

## Stored settings and the RX equaliser

At power-up, RXEQ_CTRL comes from the settings page in flash like this:

- **Nothing stored** (as after flashing a full image): the default, on.
- **A page with 0xCB = "RXEQ"**: stored by firmware with the RX equaliser, which always holds that marker in RAM. RXEQ_CTRL loads exactly as stored, off included.
- **Any other page**: stored by firmware without it (stock v1.4.x, or aioc-packet up to v1.4.1-packet.2, which hold 0 at 0xAF). The rest of the page loads as stored, the TX registers by the rules above, and RXEQ_CTRL gets the default, so the RX equaliser comes on.

The two markers are independent: a v1.4.1-packet.2 page has the TX marker but not the RX one, so its TX settings load as stored and the RX equaliser comes on. The status registers 0xCA and 0xCC always start at zero rather than whatever the page caught when it was stored. With `make TXEQ_DEFAULT=off` both equalisers default to off, and the same rules apply.

## How it works

```
y[n] = DC( x[n - 336] + up(F(down(x)))[n] )
```

The correction only matters at low frequencies, so it runs at an eighth of the rate, 6 kHz:

- **down**: a 97-tap linear-phase low-pass at 48 kHz, keeping every 8th output.
- **F**: a 61-tap FIR at 6 kHz: the inverse of the K5's high-pass (boost capped at 24 dB), minus 1, truncated to plus and minus 5 ms.
- **up**: zero-stuffing by 8 and the same low-pass, as a polyphase interpolator.
- **direct path**: the input delayed by 336 samples (7.0 ms), the delay of the correction path, so the two line up.
- **DC**: a first-order high-pass at about 1.9 Hz on the result. It is not in the design; see below.

So received audio is 7.0 ms later than before (plus the 1 ms block, see [Audio path](#audio-path-dma-blocks)). The result follows the inverse of the high-pass closely from about 300 Hz up and roughly down to 100 Hz (+7.8 dB at 100 Hz, +1.5 dB at 300 Hz, +0.1 dB at 1 kHz). Below 100 Hz the boost levels off at about +9 dB. Without the DC stage the gain at DC would be -2.9, which turned the ADC's own offset (about -95 LSB on the bench AIOC) into about +275 LSB. The DC stage takes that out, with an 85 ms time constant, far below anything the equaliser corrects; on the bench recording it changes nothing for 9600 baud (20 of 20 frames with Direwolf's atest, as without it).

It runs in fixed point, using the Cortex-M4's DSP instructions: the decimator multiplies two int16 taps by two int16 samples per instruction (SMLALD, 64-bit sum), F and the interpolator multiply 32-bit signals by 16-bit taps (SMLAWB and SMLAWT, two taps per coefficient load). The coefficients are int16: the low-pass round(h 2^18), F round(f 2^16). No intermediate can overflow even with full-scale input everywhere, and the response is within -72 dB of the float design. Apart from the DC stage it is all FIR, so nothing can drift, run away or hold a limit cycle. The work is spread evenly over each group of 8 samples: every sample runs one branch of the decimator, a slice of F and one branch of the interpolator, 35 multiply-adds in 29 instructions, with no burst every 8th sample. The coefficients come from uvk5-packet-bench `tools/rxeq_mr.py header rx_eq_k5.h --lp-taps 97` (`stm32/aioc-fw/Src/rx_eq_k5.h`); `python3 bench/rxeq_fixed.py header` quantises them into `rx_eq_k5_fixed.h`.

`bench/rxeq_fixed.py` is a bit-exact model of the firmware's arithmetic, in Python with numpy. To hear or decode what the AIOC will send, run a recording through it: `python3 bench/rxeq_fixed.py in.wav out.wav` (48 kHz mono 16-bit; `--no-dc` leaves out the DC stage; `response` prints its frequency response against the float design).

CPU: the first, float, version measured 815 cycles per sample on average from flash, then 640 from RAM, and in full duplex the audio interrupts took all the CPU, so the watchdog reset the AIOC. The fixed-point version runs from flash (code in RAM shares the RAM bus with every data load, which on this chip turned out slower) with its coefficients copied into RAM. The busiest sample is 155 instructions (538 bytes) with 59 loads, 29 of them multiply-accumulates: about 220 to 250 cycles, mostly bounded by the flash fetching 8 bytes per 3 cycles. On the bench 0xCC read about 323 cycles per sample on average while it still ran in a per-sample interrupt; even so, the per-sample interrupts left too little for full duplex, and the guard below had to switch it off there. Now the audio moves in 1 ms blocks (see [Audio path](#audio-path-dma-blocks)), the equaliser runs over each block in a tight loop, and it runs in full duplex. RAM: 2.7 kB of state and coefficients.

Overload guard: as a backstop, the equaliser switches itself off at once (no crossfade) if the main loop, which refreshes the watchdog, makes no pass for 50 ms while it runs, or if it takes over 600 cycles per sample for 1 ms in a row (checked once per block). It stays off until recording next starts, and 0xCA (bit 10, and a count in bits 11-15) says so, as does `aioc_eq.py rx status`. So a cost estimate that turns out wrong cannot starve the firmware into a watchdog reset.

## How a change is applied

A write to 0xAF takes effect at the next 1 ms block. Switching on, the filter first runs unheard for 1024 samples (21 ms) to fill its delay lines, then the output crossfades from the input to the equalised signal over 512 samples (10.7 ms). Switching off crossfades back. The DC stage starts from rest when switching on, so for a few hundred ms there is a slow offset dying away, at most about 1% of a 300 Hz signal: far below audio, and no step. The equalised signal is the input 7.0 ms later, so during the crossfade the two overlap; there is no click, but a packet arriving right then is likely lost. At the start of every recording the setting in 0xAF applies straight away, with clean delay lines. Virtual COS still looks at the unfiltered audio, so its threshold behaves exactly as before; the recording volume applies after the equaliser.

## Tests

`make -C bench test` checks with `python3 bench/rxeq_fixed.py check` that `rx_eq_k5_fixed.h` and the test data are the model's, then builds `bench/test_rx_eq.c` with the firmware's own `rx_eq.c`, twice: `test_rx_eq` as the firmware has it and `test_rx_eq_core` without the DC stage (`-DRXEQ_NO_DC_STAGE`). They check: 120000 samples of a real K5 recording of 9600 baud packets (`bench/data/rxeq_in_s16le.raw`) come out bit for bit as the model's (`bench/data/rxeq_fixed_out_s16le.raw`, and `rxeq_fixed_nodc_out_s16le.raw` without the DC stage), sample by sample and in blocks of 48 (as the firmware runs it), 1, 7, 13, 22, 32 and 47; switching, settling, crossfading and saturation in blocks give bit for bit the output and filter state of processing sample by sample; the guard and the cycle statistics count per block as specified; without the DC stage, within 1 LSB (0.35 LSB rms) of the float design (`rxeq_mr.py` with 97 taps, `bench/data/rxeq_ref_out_s16le.raw`); the overload guard trips after exactly 1 ms over budget or 50 ms without a main-loop pass, stays off bit for bit, and is lifted by the next recording; off, reserved values and other sample rates are bit-exact passthrough; switching on and off at 300 Hz, 1.2 kHz and 2.4 kHz makes no step bigger than the steady signals, and no peak beyond them except the DC stage's slow start; a change during the silent settle cancels cleanly, a change during a crossfade waits for it; DC input, up to full scale, settles to 0 (within the design's 6 kHz ripple, up to 14 LSB at full scale) and stays there, or without the DC stage to the design's -2.9 times the input; saturation clamps without wrapping and is counted, sticking at 65535. The settings page tests cover the cases above for stock, v1.4.1-packet.2 and new pages. If the design changes, regenerate `rx_eq_k5.h`, then run `python3 bench/rxeq_fixed.py header` and `python3 bench/rxeq_fixed.py reference`, and regenerate `bench/data/rxeq_ref_out_s16le.raw` with `rxeq_mr.py`.

# Audio path: DMA blocks

The AIOC moves audio in blocks of 1 ms, by DMA, instead of taking an interrupt for every sample. This is what lets it run both equalisers while the host plays and records at once (full duplex), as every packet modem does.

Why: with one interrupt per sample in each direction (96000 a second in full duplex at 48 kHz), each with its own call into the USB audio FIFO, the interrupt overhead took most of the processor before any filtering. With the receive equaliser added, the 1 ms tick and the main loop stopped in full duplex and the watchdog reset the AIOC; with the fixed-point equaliser and its guard nothing reset, but the guard had to switch it off in full duplex, and the AIOC stopped answering settings queries (HID feature reports) while it ran.

How:

- **Recording**: TIM3 still triggers the ADC at the sample rate. The DMA stores every result in a buffer of two halves of one block each (ADC2, the direct input, on DMA2 channel 1; ADC1, behind the PGA at the higher input gains, on DMA1 channel 1). When the DMA moves on from one half, one interrupt processes that half: the virtual COS level check, the receive equaliser, the volume, and one write of the whole block to the USB FIFO.
- **Playback**: TIM6 still triggers the DAC at the sample rate, and at every trigger the DMA (DMA2 channel 3) loads the next sample from a buffer of two halves. When the DMA moves on from one half, one interrupt refills it: one read of a whole block from the USB FIFO (silence if it runs dry, as before), the virtual PTT level check, the volume and the transmit equaliser. At the start of playback both halves are filled before the DMA starts.
- **Block length**: the sample rate / 1000, so 48 samples at 48000 Hz, 22 at 22050 Hz and 8 at 8000 Hz.
- **Priority**: the block interrupts keep the audio priority, above USB and the 1 ms tick, but each takes a fraction of its 1 ms. The receive equaliser's overload guard and the watchdog stay as backstops.

What changes for the host:

- **Delay**: recording gains 1 ms (a block goes to USB when it is complete); the receive equaliser's own 7.0 ms is on top of that when it runs. Playback gains 1 ms: the playback buffer the USB feedback keeps filled now counts the DMA buffer as well as the USB FIFO, and its target is upstream's 5 ms plus one block, so the FIFO keeps the same margin against late USB packets as before. Registers 0xDA to 0xDC (the buffer level average, minimum and maximum) count both, 576 bytes at 48 kHz on target.
- **Virtual PTT**: the level is checked per block, ahead of the DAC, so PTT comes on 1 to 2 ms before the audio reaches the radio and goes off the timeout less 1 to 2 ms after the last loud sample is played (18 to 19 ms with the default 20 ms timeout). So keep the virtual PTT timeout (0x84) at about 3 ms or more: below that, PTT can drop between two blocks of continuous audio, or before the last block is played.
- **Virtual COS**: checked per block, so it comes on and goes off up to 1 ms later than before, in step with the 1 ms the recorded audio gained.
- **HID reports**: the virtual COS timer (TIM17) and the input pins (EXTI) no longer send HID reports from their interrupts. They note the change, and the main loop sends it, within a pass (well under 1 ms). tinyusb is not re-entrant, and those interrupts could cut into it anywhere.
- **State bits**: the PTT1, PTT2, virtual PTT and virtual COS states are in register 0xC0 (bits 16, 17, 24 and 28), as their definitions in settings.h say. Upstream ORs them into 0xD0 instead, over its record mute bits and its record and play state fields, so there a running recording read as 3 rather than 2 while virtual PTT was on.
- **USB resets**: a USB bus reset (a host reboot, a port reset, some suspend and resume paths) closes no stream, so the AIOC stops both directions itself when the host configures it again, and each stream stops whatever is still running before it starts. Before, a playback DMA left running drained the new stream's buffer, so it never started.
- **Fox hunt**: with the fox hunt on, its timer (TIM15) triggers the DAC. While the host plays, playback takes the DAC (TIM6, at the host's rate, and the fox hunt's samples are not written) and gives it back when playback stops.
- **Underruns**: the DMA serves every conversion long before the next, so neither converter should ever miss one. If the ADC does, an overrun interrupt clears it and recording carries on one sample short; if the DAC does, an underrun interrupt restarts playback from two fresh blocks. Either way the stream does not stop for good.

Registers (read only, free in v1.4.1, every upstream branch and earlier aioc-packet releases):

| Address | Name | Contents |
|---|---|---|
| 0xCD | INFO_RXCYC | cycles the recording block interrupt takes: bits 0-15 the most since recording started, bits 16-31 the average over the last 4096 samples or more |
| 0xCE | INFO_TXCYC | the same for the playback block interrupt, since playback started |

A block is 1 ms, 72000 cycles at 72 MHz, so the average divided by 720 is the percentage of the processor. Both read zero after power-up and keep their last values when audio stops. `aioc_eq.py diag` shows them as percentages. Expected from the disassembly at 48 kHz with both equalisers on: recording about 16500 cycles per block (48 times the receive equaliser's measured 323 cycles per sample, which included some per-sample overhead that is now gone, plus about 1000 for the rest), playback about 7500, together about 33% of the processor. Earlier estimates in this project were 1.3 to 2 times low, so the registers are the figures to trust.

Tests: `bench/test_audio_block.c` checks the hardware-free parts in `audio_block.c` against the per-sample code they replace: the ADC and DAC data conversions and the volume for every value, the level check on blocks of every length and threshold, the block length for each rate, and the cycle statistics. It also simulates the DMA transfer by transfer, with the interrupt running late, to check that the interrupt always takes the half the DMA has left and that the playback level counts exactly the samples still queued. The equalisers' block tests are under their own sections above.
