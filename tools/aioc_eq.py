#!/usr/bin/env python3
"""Switch the aioc-packet transmit equaliser on or off, or show what it is doing.

    aioc_eq.py status               show the equaliser registers and whether it is running
    aioc_eq.py apply k5-red         load the "UV-K5 on a red v1 AIOC" profile
    aioc_eq.py apply custom W0 .. W15
                                    load your own set: 16 words for 0xB0 to 0xBF, in order
                                    (bench/eq.py prints this line for a set it designs)
    aioc_eq.py off                  switch the equaliser off (bit-exact stock audio)
    aioc_eq.py profiles             list the built-in profiles

Changes go to the AIOC's RAM only and are gone at the next power-up, unless you add
--store to apply or off. Storing writes the AIOC's whole settings page to flash as it
stands in RAM, so any other setting changed since power-up (by this or any other tool)
is stored too. The tool lists those first and asks before it stores.

The tool only ever touches the equaliser registers 0xB0 to 0xBF. It never changes the
PTT mapping or any other setting.

Needs Python 3.8 or later and hidapi (pip install hidapi). On Linux your user needs access
to the AIOC's hidraw device (see README.md) or run it with sudo.
"""
import argparse
import sys

VID, PID = 0x1209, 0x7388

REG_MAGIC = 0x00
MAGIC = 0x434F4941              # "AIOC", little-endian
REG_COEF0 = 0xB0                # 15 coefficient words, 0xB0 to 0xBE
N_COEF = 15
REG_CTRL = 0xBF                 # control word, commits the coefficients when it changes
REG_INFO = 0xC8                 # read-only status, updated only while audio is playing
EQ_REGS = range(REG_COEF0, REG_CTRL + 1)
WRITABLE = range(0x00, 0xC0)    # the firmware refuses writes at 0xC0 and above

# HID feature report control byte (usb_hid.c): write strobe, recall from flash, store to flash.
CTRL_WRITE, CTRL_RECALL, CTRL_STORE = 0x01, 0x40, 0x80

PROFILES = {
    "k5-red": {
        "title": "Quansheng UV-K5 (packet firmware) on a red v1 AIOC",
        "coefs": [
            0x1FC74BD8, 0xC0E42580, 0x1F551D69, 0xC0E42580, 0x1F1C6941,   # peak 63.5 Hz -6.0 dB Q0.42
            0x12B8B2CD, 0xF090A649, 0x04B022DF, 0xDF2C6FBF, 0x11C94E12,   # peak 6500 Hz +6.4 dB Q0.91
            0x2029FDF6, 0xC3FC2B77, 0x1C737807, 0xC3FC2B77, 0x1C9D75FC,   # peak 1076 Hz +0.8 dB Q1.2
        ],
        "ctrl": 0xBB801303,     # FS 48000, GEN 0x13, 3 sections
        "notes": "Flat within about +/-0.6 dB from 20 Hz to 6 kHz over the air, measured on one red "
                 "rev 1.0 AIOC into a UV-K5. It cuts the level by 5.7 dB at 1 kHz; the K5 packet "
                 "firmware's default deviation already allows for that. Only works while the host "
                 "plays audio at 48000 Hz.",
    },
}

# Firmware defaults (settings.h, unchanged from v1.4.1). Every writable register not
# listed here defaults to 0. Used only to point out what else a store would persist.
DEFAULTS = {
    0x00: MAGIC, 0x08: 0x73881209, 0x24: 0x00000404, 0x25: 0x00000008,
    0x44: 0x00020000, 0x45: 0x01000000, 0x60: 0x00010100, 0x64: 0x01000000,
    0x82: 0x00000010, 0x84: 0x00000140, 0x92: 0x00000100, 0x94: 0x00000C80,
    0xA0: 0x80001400,
}
NAMES = {
    0x00: "magic", 0x08: "USB VID/PID", 0x24: "PTT1 source", 0x25: "PTT2 source",
    0x44: "CM108 button 1 source", 0x45: "CM108 button 2 source", 0x46: "CM108 button 3 source",
    0x47: "CM108 button 4 source", 0x60: "serial control", 0x64: "serial DCD source",
    0x65: "serial DSR source", 0x66: "serial RI source", 0x67: "serial break source",
    0x72: "audio RX", 0x78: "audio TX", 0x82: "virtual PTT level", 0x84: "virtual PTT timeout",
    0x92: "virtual COS level", 0x94: "virtual COS timeout", 0xA0: "fox hunt control",
    0xA2: "fox hunt message", 0xA3: "fox hunt message", 0xA4: "fox hunt message",
    0xA5: "fox hunt message",
}


class Aioc:
    """The AIOC's settings registers, over its HID feature report.

    The firmware's feature report is 6 bytes: [control, address, data0..data3], data
    little-endian. hidapi wants a report ID in front of it on every platform; this device
    has no report IDs, so that byte is 0. A read sends the address with control 0, then
    fetches the feature report, which comes back as [0, 0, address, data0..data3]."""

    def __init__(self, device):
        self.dev = device

    def read(self, addr):
        self.dev.send_feature_report([0x00, 0x00, addr, 0, 0, 0, 0])
        reply = list(self.dev.get_feature_report(0x00, 7))
        if len(reply) < 7 or reply[2] != addr:
            raise SystemExit(f"unexpected reply from the AIOC reading 0x{addr:02X}: {bytes(reply).hex()}")
        return reply[3] | reply[4] << 8 | reply[5] << 16 | reply[6] << 24

    def write(self, addr, value):
        # EQ registers only, and never together with the store or reboot bits.
        if addr not in EQ_REGS:
            raise AssertionError(f"refusing to write 0x{addr:02X}")
        self._send(CTRL_WRITE, addr, value)

    def restore(self, addr, value):
        # Used only to put back a value the AIOC held a moment ago (after a failed store).
        if addr not in WRITABLE:
            raise AssertionError(f"refusing to write 0x{addr:02X}")
        self._send(CTRL_WRITE, addr, value)

    def store(self):
        self._send(CTRL_STORE, 0, 0)

    def recall(self):
        self._send(CTRL_RECALL, 0, 0)

    def _send(self, ctrl, addr, value):
        self.dev.send_feature_report([0x00, ctrl, addr, value & 0xFF, value >> 8 & 0xFF,
                                      value >> 16 & 0xFF, value >> 24 & 0xFF])

    def close(self):
        self.dev.close()


def open_aioc(vid, pid, serial=None):
    try:
        import hid
    except ImportError:
        raise SystemExit("this tool needs hidapi: pip install hidapi")
    if not hasattr(hid, "device"):
        raise SystemExit("the installed 'hid' module is not hidapi: pip uninstall hid, then pip install hidapi")
    found = {}
    for info in hid.enumerate(vid, pid):
        found.setdefault(info.get("serial_number") or "", info)
    if serial is not None:
        found = {s: i for s, i in found.items() if s == serial}
    if not found:
        raise SystemExit(f"no AIOC found (USB ID {vid:04x}:{pid:04x}"
                         + (f", serial {serial}" if serial else "") + "). Is it plugged in? "
                         "On Linux, check the hidraw permissions or try sudo.")
    if len(found) > 1:
        raise SystemExit("more than one AIOC is plugged in; pick one with --serial:\n  "
                         + "\n  ".join(sorted(found)))
    info = next(iter(found.values()))
    device = hid.device()
    try:
        device.open_path(info["path"])
    except OSError as e:
        raise SystemExit(f"could not open the AIOC ({e}). On Linux, check the hidraw permissions "
                         "(README.md) or try sudo.")
    aioc = Aioc(device)
    if aioc.read(REG_MAGIC) != MAGIC:
        aioc.close()
        raise SystemExit("this device does not answer like an AIOC (magic register is wrong)")
    return aioc


def read_eq(aioc):
    return [aioc.read(a) for a in EQ_REGS]       # 15 coefficients, then the control word


def describe(words):
    coefs, ctrl = words[:N_COEF], words[N_COEF]
    if ctrl & 0x3 == 0:
        return "off"
    for name, p in PROFILES.items():
        if coefs == p["coefs"] and ctrl == p["ctrl"]:
            return f"{name} ({p['title']})"
    fs = ctrl >> 16
    return (f"custom set, {ctrl & 0x3} sections, GEN 0x{ctrl >> 8 & 0xFF:02X}, "
            + (f"for {fs} Hz" if fs else "any sample rate"))


def load(aioc, coefs, ctrl):
    """Write a coefficient set and commit it. Returns what the registers read back."""
    now = read_eq(aioc)
    if now == coefs + [ctrl]:
        return now, False
    if now[N_COEF] == ctrl and ctrl != 0:
        # The firmware only takes a new set when the control word changes. It already
        # holds this exact word with other coefficients, so switch off first.
        aioc.write(REG_CTRL, 0)
    for i, w in enumerate(coefs):
        aioc.write(REG_COEF0 + i, w)
    aioc.write(REG_CTRL, ctrl)
    after = read_eq(aioc)
    if after != coefs + [ctrl]:
        raise SystemExit("the AIOC did not take the new values (read-back differs); nothing was stored")
    return after, True


def switch_off(aioc):
    """Control word to 0 first (the firmware fades out), then clear the coefficients."""
    now = read_eq(aioc)
    if now == [0] * 16:
        return False
    aioc.write(REG_CTRL, 0)
    for i in range(N_COEF):
        aioc.write(REG_COEF0 + i, 0)
    if read_eq(aioc) != [0] * 16:
        raise SystemExit("the AIOC did not take the new values (read-back differs); nothing was stored")
    return True


def other_changes(snapshot):
    """Writable registers outside the EQ block that differ from the firmware defaults."""
    return {a: v for a, v in snapshot.items()
            if a not in EQ_REGS and v != DEFAULTS.get(a, 0)}


def store(aioc, assume_yes):
    snapshot = {a: aioc.read(a) for a in WRITABLE}
    others = other_changes(snapshot)
    print()
    print("About to store to the AIOC's flash. This writes the whole settings page as it is in")
    print("RAM right now, not just the equaliser, and it is what the AIOC will load at every")
    print("power-up from now on. Do it while no audio is playing and the radio is not transmitting.")
    if others:
        print("These other settings differ from the firmware defaults and will be stored too:")
        for a, v in sorted(others.items()):
            print(f"  0x{a:02X} {NAMES.get(a, 'register'):24s} 0x{v:08X} (default 0x{DEFAULTS.get(a, 0):08X})")
    else:
        print("Every other setting is at its firmware default.")
    if not assume_yes:
        if not sys.stdin.isatty():
            raise SystemExit("not storing: add --yes to store without asking")
        if input("Store? [y/N] ").strip().lower() not in ("y", "yes"):
            raise SystemExit("not stored; the change is still in RAM until the next power-up")
    aioc.store()
    # Check: load the page back from flash and compare with what was in RAM.
    aioc.recall()
    after = {a: aioc.read(a) for a in WRITABLE}
    bad = sorted(a for a in WRITABLE if after[a] != snapshot[a])
    if bad:
        for a in bad:                   # ascending, so the EQ control word goes back last
            aioc.restore(a, snapshot[a])
        raise SystemExit("the store did not verify (flash reads back different at "
                         + ", ".join(f"0x{a:02X}" for a in bad)
                         + "). The settings in RAM have been put back; try again, and if it keeps "
                         "failing, power-cycle the AIOC and check with 'status'.")
    print("Stored to flash and checked.")


def status(aioc):
    words = read_eq(aioc)
    info = aioc.read(REG_INFO)
    print(f"Equaliser registers: {describe(words)}")
    for i in range(3):
        print(f"  section {i}: " + " ".join(f"0x{w:08X}" for w in words[5 * i:5 * i + 5]))
    print(f"  control:   0x{words[N_COEF]:08X}")
    running = bool(info & 0x4)
    gen = info >> 8 & 0xFF
    try:
        playing = (aioc.read(0xD0) >> 28 & 0xF) == 2   # INFO_AUDIO0 play state: 2 = running
    except Exception:
        playing = True                                 # unknown: say nothing extra
    if not playing and (running or not (words[N_COEF] & 0x3)):
        print("Not playing audio now: the running state below is as of the last playback,")
        print("and a change takes effect when playback next starts.")
    if running:
        clips = info >> 16
        print(f"Running now: yes, GEN 0x{gen:02X}, {info & 0x3} sections"
              + (", changing over" if info & 0x8 else "")
              + (f", {clips} clipped samples since the last change" if clips else ", no clipping"))
    elif words[N_COEF] & 0x3:
        print("Running now: not seen running. The status only updates while the AIOC is playing audio,")
        print("and the equaliser runs only at the sample rate in its control word (48000 Hz for k5-red).")
        print("Play something and run status again; if it still says this, the firmware on the AIOC")
        print("may not be aioc-packet.")
    else:
        print("Running now: no (off)")
    print("These are the values in RAM. At power-up the AIOC loads its stored settings, or the")
    print("firmware defaults if nothing is stored.")


def parse_words(texts):
    if len(texts) != 16:
        raise SystemExit("apply custom needs exactly 16 words: 0xB0 to 0xBE, then the control word 0xBF")
    try:
        words = [int(t, 0) for t in texts]
    except ValueError:
        raise SystemExit("words must be numbers, for example 0x1FC74BD8")
    if any(not 0 <= w <= 0xFFFFFFFF for w in words):
        raise SystemExit("each word must fit in 32 bits")
    return words


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("status", "apply", "off", "profiles"))
    ap.add_argument("args", nargs="*", help="for apply: a profile name, or custom and 16 words")
    ap.add_argument("--store", action="store_true",
                    help="also store the settings to flash so they survive power-off (asks first)")
    ap.add_argument("--yes", action="store_true", help="with --store: do not ask")
    ap.add_argument("--serial", help="which AIOC, if more than one is plugged in")
    ap.add_argument("--usb-id", default=f"{VID:04x}:{PID:04x}",
                    help="USB VID:PID, if you changed the AIOC's (default %(default)s)")
    a = ap.parse_args(argv)

    if a.command == "profiles":
        for name, p in PROFILES.items():
            print(f"{name}: {p['title']}\n  {p['notes']}")
        return
    if a.store and a.command not in ("apply", "off"):
        raise SystemExit("--store goes with apply or off")

    if a.command == "apply":
        if not a.args:
            raise SystemExit("apply what? Try: apply k5-red (or run 'profiles')")
        if a.args[0] == "custom":
            words = parse_words(a.args[1:])
            target, label = (words[:N_COEF], words[N_COEF]), "custom set"
        elif a.args[0] in PROFILES and len(a.args) == 1:
            p = PROFILES[a.args[0]]
            target, label = (p["coefs"], p["ctrl"]), a.args[0]
        else:
            raise SystemExit(f"unknown profile {' '.join(a.args)!r}; known: {', '.join(PROFILES)}, custom")
    elif a.args:
        raise SystemExit(f"{a.command} takes no arguments")

    try:
        vid, pid = (int(x, 16) for x in a.usb_id.split(":"))
    except ValueError:
        raise SystemExit("--usb-id must look like 1209:7388")

    aioc = open_aioc(vid, pid, a.serial)
    try:
        if a.command == "status":
            status(aioc)
        elif a.command == "apply":
            _, changed = load(aioc, *target)
            print(f"Equaliser: {label} " + ("loaded" if changed else "was already loaded") + " (RAM).")
            if target[1] >> 16:
                print(f"It runs only while the host plays audio at {target[1] >> 16} Hz.")
            if a.store:
                store(aioc, a.yes)
            else:
                print("Not stored: it will be off again after the next power-up. Add --store to keep it.")
        elif a.command == "off":
            changed = switch_off(aioc)
            print("Equaliser: " + ("switched off" if changed else "was already off") + " (RAM).")
            if a.store:
                store(aioc, a.yes)
            else:
                print("Not stored: if the equaliser was stored on, it comes back at the next power-up. "
                      "Add --store to keep it off.")
    finally:
        aioc.close()


if __name__ == "__main__":
    main(sys.argv[1:])
