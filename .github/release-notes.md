AIOC firmware v1.4.1 with a transmit equaliser added, for packet radio with the Quansheng UV-K5 and [quansheng-packet](https://github.com/M0LTE/quansheng-packet). With the equaliser off (the default) the audio is bit-for-bit what stock v1.4.1 sends. See the [README](https://github.com/M0LTE/aioc-packet#readme) for flashing and setup.

Only the red AIOC (rev 1.0 circuitry) has been measured. The `k5-red` profile is for that board with a UV-K5; it is probably wrong for other boards, such as rev 1.2, which has different output circuitry.

## Which file do I want?

| File | What it is |
|---|---|
| `aioc-packet-@VERSION@-keep-settings.bin` | **Most people.** Leaves your stored AIOC settings (PTT mapping and so on) as they are. The equaliser starts off; switch it on with `tools/aioc_eq.py apply k5-red --store`. |
| `aioc-packet-@VERSION@-k5-red.bin` | Red v1 AIOC with a UV-K5: the `k5-red` equaliser profile is on from the start, no tool needed. **Resets stored settings to the defaults.** |
| `aioc-packet-@VERSION@.bin` | The full image, equaliser off. **Resets stored settings to the defaults**, like the upstream releases. |
| `aioc-packet-@VERSION@.hex` | The full image as Intel HEX, for programmers that want it. Also resets stored settings. |
| `SHA256SUMS` | Checksums for the files above. |

Back up first: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:131072 -U aioc-backup.bin`. Then flash, for example: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-packet-@VERSION@-keep-settings.bin`.

The equaliser only runs while the host plays audio at 48000 Hz. It lowers the level by 5.7 dB at 1 kHz; the quansheng-packet firmware's default deviation allows for that. With a stock AIOC (no equaliser) use deviation 0x762 instead.

## Build

Built by GitHub Actions from this tag with:

```
@TOOLCHAIN@
```

## SHA256SUMS

```
@SHA256SUMS@
```
