# aioc-packet

Firmware for the [AIOC](https://github.com/skuep/AIOC) (the ham radio All-In-One Cable) that makes it a good packet partner for a Quansheng UV-K5.

It is upstream AIOC v1.4.1 with two additions, both switched on out of the box: a transmit equaliser that flattens the audio response of a red AIOC into a UV-K5, and a receive equaliser that undoes the UV-K5's receive audio filter so 9600 baud packet decodes. Everything else (sound card, serial port, CM108 PTT, settings) works as upstream. One thing works differently inside: the AIOC moves audio in 1 ms blocks rather than sample by sample, which adds 1 ms of delay each way and leaves it plenty of processor time to run both equalisers while your software plays and records at once.

## Packet on a UV-K5 in two steps

**Easiest:** the [setup site](https://m0lte.github.io/quansheng-packet/) walks you through both steps in Chrome or Edge, with nothing to install and no pins to short: it flashes the radio, backs up and flashes the AIOC, sets up the radio, and helps you set up your TNC.

1. **The radio:** flash [quansheng-packet](https://github.com/M0LTE/quansheng-packet) on your UV-K5. Its README has the steps and the packet setup advice.
2. **The AIOC:** flash `aioc-packet-X.Y.Z.bin` from this repo's [releases page](https://github.com/M0LTE/aioc-packet/releases), as below.

That's it. Plug the AIOC into the radio and your computer, point your modem software (direwolf or similar) at the AIOC's sound card at **48000 Hz**, and you are on the air.

Flashing this file gives the AIOC a factory-fresh start: any settings stored on it go back to the defaults. The defaults key the radio from the CM108 PTT (what direwolf and most packet software use) and from the serial port with DTR high and RTS low, which suits most software.

## Which boards

The equaliser profile, `k5-red`, was measured on a red AIOC (printed rev 1.0, v1 circuitry) with a UV-K5. It is untested on anything else. Rev 1.2 boards in particular have different output circuitry, so the profile is probably wrong there. If you have a different board or radio, or you are not sure, use [stock AIOC firmware](https://github.com/skuep/AIOC/releases) instead, or switch the equaliser off with the tool below. The receive equaliser corrects the UV-K5 itself, not the AIOC; with another radio, switch it off too.

## The files

| File | What it is |
|---|---|
| `aioc-packet-X.Y.Z.bin` | **The one to use.** Equalisers on. Resets the AIOC's stored settings to the defaults. |
| `aioc-packet-X.Y.Z-keep-settings.bin` | For upgrading with dfu-util while keeping your stored settings (PTT mapping and so on). See [Keeping your settings](#keeping-your-settings). |
| `aioc-packet-X.Y.Z.hex` | The recommended image as Intel HEX, for programmers that want it. |
| `SHA256SUMS` | Checksums, if you want to check your download. |

## Flashing

### The easy way: in your browser

Use the [setup site](https://m0lte.github.io/quansheng-packet/) (step 2). It switches the AIOC into its bootloader for you, offers a backup first, and flashes the right file.

### Another browser option: the G1LRO AIOC toolkit

The [AIOC toolkit](https://g1lro.github.io/aioc-toolkit/) flashes from Chrome or Edge, with nothing to install.

1. Download `aioc-packet-X.Y.Z.bin` from the [releases page](https://github.com/M0LTE/aioc-packet/releases).
2. Put the AIOC in bootloader mode: unplug it, short the two outermost pins of the programming header ([photo](doc/images/k1-aioc-dfu.jpg)), and plug it in with the short in place. It shows up as "STM32 BOOTLOADER".
3. Open the toolkit's **Flash Firmware** tab, connect to the bootloader, pick the file you downloaded and flash it.
4. Unplug the AIOC, remove the short, and plug it back in.

The toolkit warns you to flash only official AIOC images. This firmware is built from the official AIOC v1.4.1 source with one feature added, so you can go ahead.

On Windows, if the page cannot see the bootloader, install the WinUSB driver for "STM32 BOOTLOADER" with [Zadig](https://zadig.akeo.ie/).

### With dfu-util, with a backup first

[dfu-util](https://dfu-util.sourceforge.net/) needs no pin short: its command switches the AIOC into the bootloader by itself. It can also back up what is on the AIOC now, which the browser route cannot.

- **Linux**: `sudo apt install dfu-util`. Run the commands with `sudo`, or add the udev rule further down.
- **macOS**: `brew install dfu-util`.
- **Windows**: get dfu-util from its website, and install the WinUSB driver for "STM32 BOOTLOADER" with [Zadig](https://zadig.akeo.ie/) (upstream's notes are [here](https://yeswolf.github.io/dfu)).

**1. Back up** the whole flash, firmware and stored settings, so you can always go back exactly:

```
dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:131072 -U aioc-backup.bin
```

Keep `aioc-backup.bin` somewhere safe. If this fails (some boards may have read-out protection on), you can skip it; stock firmware is still available from upstream.

**2. Flash:**

```
dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-packet-X.Y.Z.bin
```

The AIOC restarts into the new firmware on its own. A warning about a missing DFU suffix is normal.

**If it does not come back:** put it in bootloader mode with the pin short as in the browser steps, and flash again with `-d 0483:df11`. The bootloader lives in the chip's ROM, so you cannot brick the AIOC this way.

### Keeping your settings

If you have changed the AIOC's settings and want to keep them, flash `aioc-packet-X.Y.Z-keep-settings.bin` with dfu-util instead. The transmit equaliser then works like this:

- Settings stored by stock AIOC firmware, or none stored: the equaliser comes on with `k5-red`, as with the main file.
- Settings stored by aioc-packet: the equaliser stays as you stored it, on or off. The one exception is settings stored by the first release, 1.4.1-packet.1, with the equaliser off: those look like stock, so it comes on.

To keep it off, run `aioc_eq.py off --store` (below) after flashing.

The receive equaliser is new in this release, so whatever settings you keep, it comes on. Settings stored by this release or later keep it as you stored it. To keep it off, run `aioc_eq.py rx off --store` after flashing.

## Switching the equalisers

Use `tools/aioc_eq.py` from this repo. It needs Python 3 and [hidapi](https://pypi.org/project/hidapi/):

```
pip install hidapi                               # on Debian/Ubuntu, inside a venv
python3 tools/aioc_eq.py status                  # what it is set to, and whether it is running
python3 tools/aioc_eq.py off --store             # off, and stays off over power-off
python3 tools/aioc_eq.py apply k5-red --store    # back on
python3 tools/aioc_eq.py rx status               # the receive equaliser: on or off, and running?
python3 tools/aioc_eq.py rx off --store          # receive equaliser off, and stays off
python3 tools/aioc_eq.py rx on --store           # back on
```

Leave out `--store` to try a change until the next power-up. With `--store` the AIOC saves its whole settings page as it stands, so the tool shows any other changed settings and asks first. It never changes the PTT mapping or anything else.

Good to know: the transmit equaliser runs only while the host plays audio at 48000 Hz, and it lowers the level by 5.7 dB at 1 kHz, which the quansheng-packet firmware's default deviation already allows for. The receive equaliser runs only while the host records at 48000 Hz.

On Linux the tool, like direwolf's CM108 PTT, needs access to the AIOC's hidraw device. Use `sudo`, or add this as `/etc/udev/rules.d/70-aioc.rules` (it covers dfu-util and the serial port too), reload, and replug. The name must start with a number below 73: `uaccess` rules that run later (a `99-` name) silently grant nothing.

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="7388", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="7388", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="7388", TAG+="uaccess"
```

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`.

Writing your own software? The registers and how to load a profile are in [bench/EQ.md](bench/EQ.md).

## What the transmit equaliser does

Out of the box a red AIOC and a UV-K5 have a lumpy transmit response: measured over the air, +6.7 dB at 60 Hz, -1.1 dB at 3.15 kHz and -5.5 dB at 6 kHz, relative to 1 kHz. Part of that is the AIOC's output, which rises at low frequencies, and part is the radio. With `k5-red` on, it measured within -0.64 dB and +0.42 dB from 20 Hz to 6 kHz.

The quansheng-packet firmware's default deviation (0x856) assumes this equaliser is on. With a stock AIOC, or with the equaliser off, use deviation 0x762 instead. The rest of the packet advice is in [quansheng-packet](https://github.com/M0LTE/quansheng-packet).

## What the receive equaliser does

A UV-K5's receive audio loses its low frequencies: its audio output stage has a high-pass filter at about 128 Hz. The loss itself is mild, but the filter also shifts the timing of the low-frequency part of the signal, and that is what breaks 9600 baud packet. On the bench, a decoder got 0 of 70 packets from the K5's audio, while the same transmissions decoded fine from a flat receiver. The AIOC's own input is not the problem (it is flat down to about 7 Hz).

The receive equaliser in the AIOC undoes that filter, level and timing, so every modem on the computer gets a flat signal without needing to know about it. On the recordings of those 70 packets, the same filter as in this firmware let three different decoders get all 70. 1200 baud AFSK and 3600 baud QPSK decode as well as before.

- It is on by default, and it runs only while your software records at 48000 Hz. At any other rate the audio is untouched. It keeps running while your software transmits at the same time, as packet modems do.
- It delays received audio by 7.0 ms (8 ms with the AIOC's 1 ms blocks). That makes no difference to packet.
- If it ever takes too much of the AIOC's processor time, it switches itself off until recording restarts, and `aioc_eq.py rx status` says so.
- It is made for the UV-K5. With another radio, switch it off with `aioc_eq.py rx off --store`. Off, the received audio is bit-for-bit what stock firmware sends.

The details are in [bench/EQ.md](bench/EQ.md#rx-equaliser), and how the audio moves in 1 ms blocks, with the timing of the virtual PTT and COS, in [bench/EQ.md](bench/EQ.md#audio-path-dma-blocks). `aioc_eq.py diag` shows how much of the AIOC's processor the audio takes.

## Going back to stock

- To get back exactly what you had, settings included, flash your backup: `dfu-util -d 1209:7388,0483:df11 -a 0 -s 0x08000000:leave -D aioc-backup.bin`.
- Or flash an upstream release, such as `aioc-fw-1.4.1.bin` from [skuep/AIOC releases](https://github.com/skuep/AIOC/releases), either way above. That resets stored settings to the defaults. The firmware a board shipped with is not necessarily identical to the upstream release, which is another reason to keep your backup.

## Building

```
git clone --recursive https://github.com/M0LTE/aioc-packet
cd aioc-packet
make                        # build/aioc-fw.bin, aioc-fw-keep-settings.bin, aioc-fw.hex
make test                   # host unit tests
python3 tools/test_aioc_eq.py
```

You need `arm-none-eabi-gcc` and newlib. Releases are built by GitHub Actions (`.github/workflows/firmware.yml`) in an Ubuntu 26.04 container with gcc-arm-none-eabi 15:14.2.rel1-1 (GCC 14.2.1), binutils-arm-none-eabi 2.45.50.20251209-1ubuntu1+23build1 and libnewlib-arm-none-eabi 4.6.0.20260123-1. The same packages on your own machine give byte-identical images. The upstream STM32CubeIDE project in `stm32/aioc-fw` still works too.

How the equalisers work inside, and how to design your own transmit set with `bench/eq.py`: [bench/EQ.md](bench/EQ.md).

## Credits and licence

The AIOC is Simon Kueppers' ([skuep](https://github.com/skuep)) design: hardware, firmware and the original documentation, which is kept in [doc/upstream-README.md](doc/upstream-README.md) (the current version is [upstream](https://github.com/skuep/AIOC)). This fork adds the transmit and receive equalisers, their tools and the build and release setup. Hardware design files are in `kicad/`.

MIT licence, see [LICENSE.md](LICENSE.md).
