# TX equaliser

A tunable equaliser in the AIOC's transmit path (USB audio OUT to the DAC, which drives the radio's mic input). It is up to three biquad filters in a row, run on every DAC sample, set through new settings registers. It comes on by default with the `k5-red` profile below. Switched off, the audio is bit-for-bit what stock v1.4.1 sends.

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
2. The host writes TXEQ_CTRL with a new GEN. At the next sample the firmware copies all 15 coefficients at once, so it never runs a half-written set.
3. The new set runs unheard alongside the old one for 4096 samples (85 ms at 48 kHz), so its start-up transient dies away, then the output crossfades from old to new over 512 samples (10.7 ms).

Switching on (from off) and switching off go through the same crossfade. While off, the output is the input, untouched. At the start of every playback stream the set in the registers is installed straight away, with no crossfade and a clean history. If a new set is committed while a change is still in progress, it waits for that change to finish; words written after a commit never leak into the committed set.

Virtual PTT still looks at the unfiltered audio, so its threshold behaves exactly as before.

## Numbers inside

The filters are direct form I in fixed point: 64-bit accumulate (single-cycle SMLAL on the Cortex-M4, no FPU use), 12 bits below the 16-bit sample LSB, and 12 dB of headroom between sections. Every section carries its rounding remainder into the next sample, which stops low-frequency sections from holding a stuck offset or a limit cycle. Every intermediate value and the output saturate instead of wrapping. Cost is roughly 100 cycles per sample for three sections (about 7% of the 72 MHz core at 48 kHz), double that during a crossfade.

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

`make -C bench test` compiles the firmware's own `tx_eq.c` and `settings_page.c` with the host gcc (with the undefined-behaviour and address sanitizers) and checks: bypass is bit-exact (defaults, NSECT 0, wrong sample rate, and after switching off); the fixed-point output matches a double-precision reference to within the 16-bit rounding (gain within 0.01 dB, error under 0.36 LSB rms, 20 Hz to 12 kHz); saturation clamps with no wraparound, at the output and inside; extreme coefficients cannot overflow; silence decays to exact zero with no limit cycle; a set written word by word during playback never leaks before its commit, and switching sets, switching on and switching off cause no step or overshoot beyond the steady signals. For the settings page, built both ways (defaults `k5-red` and off), it checks the rules above for stock, v1.4.1-packet.1 and marked pages, that everything outside the EQ block loads as stored, and that store then recall gives back the same registers, a stored "off" included.

## Flashing

See the README for flashing, backing up and recovery. Two details for people building their own images:

- The full `.bin` is 128000 bytes and ends with the settings page at 0x0801F000 filled with 0xFF. Upstream does this on purpose: flashing it wipes the stored settings, and the AIOC boots with firmware defaults, the equaliser on included. That includes the PTT mapping, register 0x24, whose default adds serial DTR-and-not-RTS as a PTT source.
- An image without the settings page leaves the stored settings as they are, with the EQ registers following the rules in "Stored settings and the equaliser": `arm-none-eabi-objcopy -O binary -R .eeprom aioc-fw.elf aioc-fw-keep-settings.bin`, which `make` also produces. DFU only erases the pages it writes.
