# aioc-packet

Firmware for the [AIOC](https://github.com/skuep/AIOC) (the ham radio All-In-One Cable) that adds a transmit equaliser, so a UV-K5 running packet firmware sends a flat audio response over the air.

It is upstream AIOC v1.4.1 with one addition. With the equaliser off, which is the default, the audio is bit-for-bit what stock v1.4.1 sends, and everything else (sound card, serial port, CM108 PTT, settings) works exactly as upstream.

> **Setting up packet on a UV-K5?** The radio side lives at **[github.com/M0LTE/quansheng-packet](https://github.com/M0LTE/quansheng-packet)**: the radio firmware and the packet setup advice. Start there. This repo is only the AIOC half.

## Who it is for

You have a red AIOC and a Quansheng UV-K5 running [quansheng-packet](https://github.com/M0LTE/quansheng-packet). Out of the box that pair has a lumpy transmit response: measured over the air, +6.7 dB at 60 Hz, -1.1 dB at 3.15 kHz and -5.5 dB at 6 kHz, relative to 1 kHz. Part of that is the AIOC's output, which rises at low frequencies, and part is the radio. With the `k5-red` equaliser profile it measured within -0.64 dB and +0.42 dB from 20 Hz to 6 kHz.

## Which boards

| Board | Status |
|---|---|
| Red AIOC, printed rev 1.0 (v1 circuitry: no TX boost, no RX gain) | Tested. The `k5-red` profile was measured on this board. |
| Other AIOC boards | Untested. The firmware is v1.4.1 plus the equaliser, so it should run wherever v1.4.1 runs, but the `k5-red` profile is probably wrong for them. Rev 1.2 in particular has different output circuitry. Leave the equaliser off there. |

## Which file to download

Get these from the [releases page](https://github.com/M0LTE/aioc-packet/releases).

| File | Use it when |
|---|---|
| `aioc-packet-X.Y.Z-keep-settings.bin` | **Most people.** Keeps your stored AIOC settings (PTT mapping and so on). The equaliser starts off; switch it on with the tool below. |
| `aioc-packet-X.Y.Z-k5-red.bin` | Red AIOC with a UV-K5, and you want the profile on without installing anything. **Resets stored settings to the defaults.** |
| `aioc-packet-X.Y.Z.bin` | Full image, equaliser off. **Resets stored settings to the defaults**, like upstream releases. |
| `aioc-packet-X.Y.Z.hex` | The full image as Intel HEX, for programmers that want it. Also resets stored settings. |

Check your download against `SHA256SUMS` if you like.

If you use the `-k5-red` image, upgrade with the `-k5-red` image again, or store the profile with the tool: a `keep-settings` upgrade keeps what is stored, and a `-k5-red` image stores nothing by itself.

## Flashing

You need [dfu-util](https://dfu-util.sourceforge.net/). The AIOC switches itself into the chip's bootloader when dfu-util asks, so there is nothing to open up or short.

- **Linux**: `sudo apt install dfu-util` (or your distro's equivalent). Run the commands with `sudo`, or add the udev rule further down.
- **macOS**: `brew install dfu-util`.
- **Windows**: download dfu-util from its website. It needs the WinUSB driver for the bootloader: install it with [Zadig](https://zadig.akeo.ie/) for the "STM32 BOOTLOADER" device (and for the AIOC's DFU interface if dfu-util cannot find the AIOC). Upstream's notes are [here](https://yeswolf.github.io/dfu).

Flashing has been tested on Linux.

**1. Back up what is on the AIOC now.** This saves the whole flash, firmware and stored settings, so you can always go back exactly:

```
dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:131072 -U aioc-backup.bin
```

It leaves the AIOC in the bootloader, ready for step 2. Keep `aioc-backup.bin` somewhere safe. If this step fails with an error (some boards may have read-out protection on), you can skip it; stock firmware is still available from upstream.

**2. Flash the new firmware:**

```
dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-packet-X.Y.Z-keep-settings.bin
```

The AIOC restarts into the new firmware on its own. A warning about a missing DFU suffix is normal.

**If it does not come back:** unplug it, short the two outermost pins of the programming header ([photo](doc/images/k1-aioc-dfu.jpg)), plug it in with the short in place, and flash again with `-d 0483:df11` in place of `-d 1209:7388,0483:df11`. Then remove the short and replug. The bootloader lives in the chip's ROM, so you cannot brick it this way.

## Switching the equaliser on

The `-k5-red` image has it on already. Otherwise use `tools/aioc_eq.py` from this repo. It needs Python 3 and [hidapi](https://pypi.org/project/hidapi/):

```
pip install hidapi                      # on Debian/Ubuntu, do this inside a venv
python3 tools/aioc_eq.py apply k5-red   # on until the next power-up: try it first
python3 tools/aioc_eq.py status         # what it is set to, and whether it is running
python3 tools/aioc_eq.py apply k5-red --store   # keep it over power-off
python3 tools/aioc_eq.py off            # back to stock audio (add --store to keep it off)
```

Good to know:

- Without `--store`, changes last until the AIOC is unplugged. With `--store`, the AIOC saves its **whole** settings page as it stands, so anything else changed since power-up (by this or any other tool) is saved too. The tool lists those settings and asks before storing.
- The tool only touches the equaliser. It never changes the PTT mapping or any other setting.
- The equaliser runs only while the host plays audio at **48000 Hz**. Set your modem to 48 kHz.
- It lowers the level by 5.7 dB at 1 kHz. The quansheng-packet firmware's default deviation already allows for that.

On Linux the tool, like direwolf's CM108 PTT, needs access to the AIOC's hidraw device. Either use `sudo`, or add this as `/etc/udev/rules.d/99-aioc.rules` (it covers dfu-util too), then replug:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="7388", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="7388", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", TAG+="uaccess"
```

Writing your own software? The registers and the exact steps to load a profile are in [bench/EQ.md](bench/EQ.md), under "The k5-red profile" and "Loading a set from your own software".

## Packet setup, in short

The full advice is in [quansheng-packet](https://github.com/M0LTE/quansheng-packet). The main points:

- Drive the AIOC near full scale from your modem software, and set the deviation in the radio rather than turning the audio down.
- The quansheng-packet firmware's default deviation (0x856) assumes this equaliser with the `k5-red` profile.
- With a stock AIOC, or with the equaliser off, use deviation 0x762 instead.

For reference, the red AIOC's mic-level output measures 26 mV rms at 0 dBFS, and unloaded it rises at low frequencies (+8 dB at 100 Hz, +18 dB at 20 Hz).

## Going back to stock AIOC firmware

- To get back exactly what you had, settings included, flash your backup: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-backup.bin`.
- Or flash an upstream release, such as `aioc-fw-1.4.1.bin` from [skuep/AIOC releases](https://github.com/skuep/AIOC/releases), the same way. That resets stored settings to the defaults. The firmware a board shipped with is not necessarily identical to the upstream release, which is another reason to keep your backup.

## Building

```
git clone --recursive https://github.com/M0LTE/aioc-packet
cd aioc-packet
make                        # build/aioc-fw.bin, aioc-fw-keep-settings.bin, aioc-fw.hex
make TXEQ_DEFAULT=k5-red BUILD=build-k5-red   # the image with the profile on by default
make test                   # host unit tests for the equaliser
python3 tools/test_aioc_eq.py
```

You need `arm-none-eabi-gcc` and newlib. Releases are built by GitHub Actions (`.github/workflows/firmware.yml`) in an Ubuntu 26.04 container with gcc-arm-none-eabi 15:14.2.rel1-1 (GCC 14.2.1), binutils-arm-none-eabi 2.45.50.20251209-1ubuntu1+23build1 and libnewlib-arm-none-eabi 4.6.0.20260123-1. The same packages on your own machine give byte-identical images. The upstream STM32CubeIDE project in `stm32/aioc-fw` still works too.

How the equaliser works inside, and how to design your own set with `bench/eq.py`: [bench/EQ.md](bench/EQ.md).

## Credits and licence

The AIOC is Simon Kueppers' ([skuep](https://github.com/skuep)) design: hardware, firmware and the original documentation, which is kept in [doc/upstream-README.md](doc/upstream-README.md) (the current version is [upstream](https://github.com/skuep/AIOC)). This fork adds the transmit equaliser, its tools and the build and release setup. Hardware design files are in `kicad/`.

MIT licence, see [LICENSE.md](LICENSE.md).
