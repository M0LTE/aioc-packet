AIOC firmware v1.4.1 with a transmit and a receive equaliser added, for packet radio with the Quansheng UV-K5 and [quansheng-packet](https://github.com/M0LTE/quansheng-packet). Flash quansheng-packet on the radio and `aioc-packet-@VERSION@.bin` on the AIOC, and you are ready for packet. See the [README](https://github.com/M0LTE/aioc-packet#readme) for flashing (in your browser, or with dfu-util) and setup.

The equaliser is on out of the box with the `k5-red` profile, measured on a red AIOC (rev 1.0 circuitry) with a UV-K5. It is untested on other boards and radios; rev 1.2 boards have different output circuitry. If you are not sure, use [stock AIOC firmware](https://github.com/skuep/AIOC/releases), or switch the equaliser off with `tools/aioc_eq.py off --store`.

The receive equaliser is on out of the box too. It undoes the UV-K5's receive audio high-pass (about 128 Hz), which otherwise stops 9600 baud packet decoding, and adds 7.0 ms of delay to received audio. It runs only while the host records at 48000 Hz. With another radio, switch it off with `tools/aioc_eq.py rx off --store`.

## Which file do I want?

| File | What it is |
|---|---|
| `aioc-packet-@VERSION@.bin` | **The one to use.** Equalisers on. Resets the AIOC's stored settings to the defaults (PTT from the CM108 PTT and from serial DTR high, RTS low). |
| `aioc-packet-@VERSION@-keep-settings.bin` | For dfu-util upgrades that keep your stored settings. The equaliser comes on if your settings were stored by stock firmware (or none are stored), and stays as you stored it if they were stored by aioc-packet. The receive equaliser comes on unless your settings were stored by a release that has it. |
| `aioc-packet-@VERSION@.hex` | The recommended image as Intel HEX, for programmers that want it. |
| `SHA256SUMS` | Checksums for the files above. |

With dfu-util, back up first: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:131072 -U aioc-backup.bin`. Then flash: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-packet-@VERSION@.bin`.

The transmit equaliser only runs while the host plays audio at 48000 Hz. It lowers the level by 5.7 dB at 1 kHz; the quansheng-packet firmware's default deviation allows for that. With a stock AIOC (no equaliser) use deviation 0x762 instead.

## Build

Built by GitHub Actions from this tag with:

```
@TOOLCHAIN@
```

## SHA256SUMS

```
@SHA256SUMS@
```
