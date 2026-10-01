#!/usr/bin/env python3
"""Switch the aioc-packet transmit and receive equalisers on or off, or show what they do.

    aioc_eq.py status               show the transmit equaliser registers and whether it is running
    aioc_eq.py apply k5-red         load the "UV-K5 on a red v1 AIOC" profile
    aioc_eq.py apply custom W0 .. W15
                                    load your own set: 16 words for 0xB0 to 0xBF, in order
                                    (bench/eq.py prints this line for a set it designs)
    aioc_eq.py off                  switch the transmit equaliser off (bit-exact stock audio)
    aioc_eq.py profiles             list the built-in profiles

    aioc_eq.py rx status            show the receive equaliser and whether it is running
    aioc_eq.py rx on                switch the receive equaliser on (UV-K5 profile, the default)
    aioc_eq.py rx off               switch it off (bit-exact stock receive audio)

    aioc_eq.py diag                 why the AIOC last reset (watchdog, fault, power-on ...),
                                    from its reset diagnostics, and how much of the processor
                                    the audio takes; read only

Changes go to the AIOC's RAM only. At the next power-up the AIOC loads its stored settings
again, or its defaults if nothing is stored (aioc-packet's defaults: k5-red on, receive
equaliser on), unless you add --store to apply, off, rx on or rx off. Storing writes the
AIOC's whole settings page to flash as it stands in RAM, so any other setting changed since
power-up (by this or any other tool) is stored too. The tool lists those first and asks before it stores.

The tool only ever touches the equaliser registers: 0xB0 to 0xBF for transmit, 0xAF for
receive. It never changes the PTT mapping or any other setting.

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
REG_RXCTRL = 0xAF               # receive equaliser: 0 off, 1 the UV-K5 profile, others reserved (off)
REG_RXINFO = 0xCA               # read-only receive status, updated only while recording
REG_RXPAGE = 0xCB               # read-only, "RXEQ" on firmware that has the receive equaliser
REG_RXCYC = 0xCC                # read-only receive equaliser CPU cycles per sample: max, average
RXPAGE_MARKER = 0x51455852      # "RXEQ", little-endian
RX_OFF, RX_K5 = 0, 1
RX_REGS = (REG_RXCTRL,)
REG_AUDIO0, REG_AUDIO2 = 0xD0, 0xD2   # play/record state, recording rate
# Audio processing cost (read only, firmware with DMA block audio): cycles per 1 ms block of the
# recording and the playback interrupt, bits 0-15 the most, bits 16-31 the average
REG_RXBLK, REG_TXBLK = 0xCD, 0xCE
BLOCK_CYCLES = 72000            # one 1 ms block at 72 MHz

# Reset diagnostics (read only, settings.h SETTINGS_REG_INFO_DIAG*): 0xE0 to 0xEE describe the
# last reset, 0xEF is live
REG_DIAG = 0xE0
DIAG_REGS = range(0xE0, 0xF0)
DIAG_MARKER = 0x47414944        # "DIAG", little-endian
(D_MARKER, D_RESET, D_FAULT, D_PC, D_LR, D_XPSR, D_CFSR, D_HFSR, D_MMFAR, D_BFAR, D_EXCRET, D_UPTIME,
 D_AGE0, D_AGE1, D_STACK, D_LOOPS) = range(0xE0, 0xF0)
CSR_FLAGS = [(31, "LPWRRSTF", "low-power reset"), (30, "WWDGRSTF", "window watchdog"),
             (29, "IWDGRSTF", "independent watchdog"), (28, "SFTRSTF", "software reset"),
             (27, "PORRSTF", "power-on"), (26, "PINRSTF", "reset pin"), (25, "OBLRSTF", "option byte load"),
             (23, "V18PWRRSTF", "1.8 V domain reset")]
EXCEPTIONS = {2: "NMI", 3: "HardFault", 4: "MemManage fault", 5: "BusFault", 6: "UsageFault",
              11: "SVCall", 14: "PendSV", 15: "SysTick"}
IRQS = {11: "the DMA1 channel 1 interrupt (recording audio, ADC1)",
        18: "the ADC interrupt (recording audio)", 19: "USB_HP_CAN_TX", 20: "USB_LP_CAN_RX0",
        23: "the EXTI9_5 interrupt (inputs)", 24: "the TIM15 interrupt (fox hunt)",
        25: "the TIM16 interrupt (virtual PTT)", 26: "the TIM17 interrupt (virtual COS)",
        29: "TIM3", 30: "the TIM4 interrupt (LEDs)", 37: "the USART1 interrupt (serial)",
        54: "the TIM6/DAC interrupt (playing audio)", 56: "the DMA2 channel 1 interrupt (recording audio)",
        58: "the DMA2 channel 3 interrupt (playing audio)", 74: "the USB interrupt (high priority)",
        75: "the USB interrupt", 76: "USB wakeup"}
CFSR_BITS = [(0, "IACCVIOL: instruction fetch from a forbidden address"),
             (1, "DACCVIOL: data access to a forbidden address"),
             (3, "MUNSTKERR: fault unstacking on exception return"),
             (4, "MSTKERR: fault stacking on exception entry"),
             (5, "MLSPERR: fault during lazy FPU state save"),
             (8, "IBUSERR: bus error fetching an instruction"),
             (9, "PRECISERR: precise data bus error (BFAR holds the address)"),
             (10, "IMPRECISERR: imprecise data bus error (the PC is only near the cause)"),
             (11, "UNSTKERR: bus error unstacking on exception return"),
             (12, "STKERR: bus error stacking on exception entry (stack overflow?)"),
             (13, "LSPERR: bus error during lazy FPU state save"),
             (16, "UNDEFINSTR: undefined instruction"),
             (17, "INVSTATE: invalid state (a branch to an even address, not Thumb)"),
             (18, "INVPC: invalid exception return"),
             (19, "NOCP: coprocessor (FPU) not enabled"),
             (24, "UNALIGNED: unaligned access"),
             (25, "DIVBYZERO: division by zero")]
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

# Firmware defaults outside the equaliser block (settings.h, unchanged from v1.4.1). Every
# other writable register outside that block defaults to 0. Used only to point out what else
# a store would persist.
DEFAULTS = {
    0x00: MAGIC, 0x08: 0x73881209, 0x24: 0x00000404, 0x25: 0x00000008,
    0x44: 0x00020000, 0x45: 0x01000000, 0x60: 0x00010100, 0x64: 0x01000000,
    0x82: 0x00000010, 0x84: 0x00000140, 0x92: 0x00000100, 0x94: 0x00000C80,
    0xA0: 0x80001400, REG_RXCTRL: RX_K5,
}
NAMES = {
    0x00: "magic", 0x08: "USB VID/PID", 0x24: "PTT1 source", 0x25: "PTT2 source",
    0x44: "CM108 button 1 source", 0x45: "CM108 button 2 source", 0x46: "CM108 button 3 source",
    0x47: "CM108 button 4 source", 0x60: "serial control", 0x64: "serial DCD source",
    0x65: "serial DSR source", 0x66: "serial RI source", 0x67: "serial break source",
    0x72: "audio RX", 0x78: "audio TX", 0x82: "virtual PTT level", 0x84: "virtual PTT timeout",
    0x92: "virtual COS level", 0x94: "virtual COS timeout", 0xA0: "fox hunt control",
    0xA2: "fox hunt message", 0xA3: "fox hunt message", 0xA4: "fox hunt message",
    0xA5: "fox hunt message", REG_RXCTRL: "receive equaliser",
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
        if addr not in EQ_REGS and addr not in RX_REGS:
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


def other_changes(snapshot, own=EQ_REGS):
    """Writable registers outside the ones being changed (own) that differ from the firmware
    defaults."""
    return {a: v for a, v in snapshot.items()
            if a not in own and v != DEFAULTS.get(a, 0)}


def store(aioc, assume_yes, own=EQ_REGS):
    snapshot = {a: aioc.read(a) for a in WRITABLE}
    others = other_changes(snapshot, own)
    print()
    print("About to store to the AIOC's flash. This writes the whole settings page as it is in")
    print("RAM right now, not just this equaliser, and it is what the AIOC will load at every")
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
    if has_rx(aioc):
        print(f"Receive equaliser: {describe_rx(aioc.read(REG_RXCTRL))} (details: rx status)")
    print("These are the values in RAM. At power-up the AIOC loads its stored settings, or the")
    print("firmware defaults if nothing is stored.")


def has_rx(aioc):
    """True if the firmware has the receive equaliser (it holds the "RXEQ" marker)."""
    return aioc.read(REG_RXPAGE) == RXPAGE_MARKER


def describe_rx(ctrl):
    if ctrl == RX_K5:
        return "on (UV-K5 profile)"
    if ctrl == RX_OFF:
        return "off"
    return f"off (reserved value 0x{ctrl:08X})"


def rx_set(aioc, value):
    """Write the receive equaliser's control register. Returns True if it changed."""
    if aioc.read(REG_RXCTRL) == value:
        return False
    aioc.write(REG_RXCTRL, value)
    if aioc.read(REG_RXCTRL) != value:
        raise SystemExit("the AIOC did not take the new value (read-back differs); nothing was stored")
    return True


def rx_status(aioc):
    ctrl = aioc.read(REG_RXCTRL)
    info = aioc.read(REG_RXINFO)
    cyc = aioc.read(REG_RXCYC)
    print(f"Receive equaliser: {describe_rx(ctrl)} (register 0xAF = 0x{ctrl:08X})")
    try:
        recording = (aioc.read(REG_AUDIO0) >> 24 & 0xF) == 2   # INFO_AUDIO0 record state: 2 = running
        rate = aioc.read(REG_AUDIO2)
    except Exception:
        recording, rate = True, 0                              # unknown: say nothing extra
    if not recording:
        print("Not recording now: the running state below is as of the last recording since power-up,")
        print("and a change takes effect when recording next starts.")
    if info & 0x100:
        clips = info >> 16
        cmax, cavg = cyc & 0xFFFF, cyc >> 16
        print("Running now: yes" + (", changing over" if info & 0x200 else "")
              + (f", {clips} clipped samples since recording started or the last change" if clips
                 else ", no clipping"))
        print(f"CPU: {cavg} cycles per sample on average, {cmax} at most "
              f"({100 * cmax / 1500:.0f}% of a sample period at 48000 Hz)")
    elif info & 0x200:
        print("Running now: changing over")
    elif info & 0x400:
        print("Running now: no. Its overload guard switched it off, because it was taking too much of the")
        print("AIOC's processor time (enough to starve the rest of the firmware). It stays off until recording")
        print("next starts. If this keeps happening, switch it off (rx off) and report it.")
    elif ctrl == RX_K5:
        if recording and rate and rate != 48000:
            print(f"Running now: no. Recording runs at {rate} Hz, and the equaliser runs only at 48000 Hz.")
        else:
            print("Running now: not seen running. The status only updates while the AIOC is recording,")
            print("and the equaliser runs only at 48000 Hz. Record something and run rx status again.")
    else:
        print("Running now: no (off)")
    overloads = info >> 11 & 0x1F
    if overloads:
        print(f"Overload guard: switched it off {overloads}{'+' if overloads == 31 else ''} time(s) since power-up.")
    print("This is the value in RAM. At power-up the AIOC loads its stored settings, or the")
    print("firmware default (on) if nothing is stored.")


def exception_name(n):
    if n >= 16:
        return IRQS.get(n - 16, f"interrupt IRQ {n - 16}")
    return EXCEPTIONS.get(n, f"exception {n}")


def describe_diag(r):
    """Plain-words lines for the reset diagnostics registers r (a dict address: value)."""
    if r[D_MARKER] != DIAG_MARKER:
        return ["This AIOC's firmware has no reset diagnostics (it needs an aioc-packet build that has them)."]
    reset, fault = r[D_RESET], r[D_FAULT]
    flags = [(bit, name, words) for bit, name, words in CSR_FLAGS if reset >> bit & 1]
    has = {name for _, name, _ in flags}
    valid = bool(reset >> 18 & 1)
    has_fault = bool(reset >> 16 & 1)
    reboot = bool(reset >> 17 & 1)
    exc = fault & 0x1FF
    lines = []

    if "PORRSTF" in has:
        cause = "power-on (plugged in, or the supply dipped). Nothing from before it survives"
    elif "LPWRRSTF" in has:
        cause = "low-power reset"
    elif "WWDGRSTF" in has:
        cause = "window watchdog: the firmware uses it to start the DFU bootloader (a firmware update)"
    elif "IWDGRSTF" in has and reboot:
        cause = "the host asked the AIOC to reboot (it reboots through the watchdog)"
    elif "IWDGRSTF" in has and has_fault:
        cause = f"independent watchdog, after a {exception_name(exc)} (the fault handler stops, the watchdog resets)"
    elif "IWDGRSTF" in has:
        cause = "independent watchdog: the main loop stopped refreshing it for about 150 ms, and no fault was recorded"
    elif "SFTRSTF" in has:
        cause = "software reset"
    elif "OBLRSTF" in has:
        cause = "option byte load"
    elif "PINRSTF" in has:
        cause = "the reset pin (NRST)"
    else:
        cause = "unknown"
    lines.append(f"Last reset: {cause}.")
    lines.append("  Reset flags: " + (", ".join(name for _, name, _ in flags) or "none")
                 + f" (0x{reset & 0xFF800000:08X})")
    if not valid:
        if "PORRSTF" not in has:
            lines.append("  The record from before the reset did not survive (or the firmware before it had none), "
                         "so there is nothing more to say.")
        return lines + live_lines(r)

    lines.append(f"  Resets since power-on: {reset & 0xFFFF}. The run before it lasted {fmt_ms(r[D_UPTIME])}.")

    if has_fault:
        lines.append(f"  Fault: {exception_name(exc)}, {fault >> 16} fault(s) since power-on.")
        pc, lr = r[D_PC], r[D_LR]
        if pc == 0 and lr == 0:
            lines.append("  The stack pointer was not in RAM, so there is no PC (a stack overflow, most likely).")
        else:
            running = r[D_XPSR] & 0x1FF
            where = "the main loop (thread mode)" if running == 0 else exception_name(running)
            lines.append(f"  At PC 0x{pc:08X}, LR 0x{lr:08X}, while running {where}.")
            lines.append(f"  Find the function: arm-none-eabi-addr2line -f -e aioc-fw.elf 0x{pc:08X} 0x{lr:08X}")
        cfsr, hfsr = r[D_CFSR], r[D_HFSR]
        for bit, words in CFSR_BITS:
            if cfsr >> bit & 1:
                lines.append(f"  CFSR {words}")
        if cfsr >> 7 & 1:
            lines.append(f"  MMFAR (the address it tried to use): 0x{r[D_MMFAR]:08X}")
        if cfsr >> 15 & 1:
            lines.append(f"  BFAR (the address it tried to use): 0x{r[D_BFAR]:08X}")
        if hfsr >> 30 & 1:
            lines.append("  HFSR FORCED: a configurable fault escalated to HardFault (the CFSR bits say which)")
        if hfsr >> 1 & 1:
            lines.append("  HFSR VECTTBL: bus fault reading the vector table")
        lines.append(f"  (CFSR 0x{cfsr:08X}, HFSR 0x{hfsr:08X}, xPSR 0x{r[D_XPSR]:08X}, "
                     f"EXC_RETURN 0x{r[D_EXCRET]:08X})")
    elif fault >> 16:
        lines.append(f"  No fault before this reset ({fault >> 16} since power-on, reported at the time).")
    else:
        lines.append("  No fault recorded.")

    ages = {"main loop": r[D_AGE0] & 0xFFFF, "USB interrupt": r[D_AGE0] >> 16,
            "recording interrupt": r[D_AGE1] & 0xFFFF, "playback interrupt": r[D_AGE1] >> 16}
    lines.append("  Last ran, before the last 1 ms tick: " + "; ".join(
        f"{k} {'not since boot' if v == 0xFFFF else f'{v} ms'}" for k, v in ages.items()))
    if "IWDGRSTF" in has and not has_fault and not reboot:
        main_age = ages["main loop"]
        isr = [v for k, v in ages.items() if k != "main loop" and v != 0xFFFF]
        if main_age >= 50 and isr and min(isr) <= 5:
            lines.append(f"  So the interrupts and the 1 ms tick kept running while the main loop did not, for "
                         f"{main_age} ms: the main loop was stuck, or the interrupts left it no CPU time "
                         "(watchdog starvation).")
        elif main_age <= 5 and all(v <= 5 for v in isr):
            lines.append("  So everything stopped at once, the 1 ms tick included: interrupts disabled, or the "
                         "CPU stuck inside an interrupt at or above the tick's priority (the audio interrupts "
                         "are), with no fault.")

    stack = r[D_STACK]
    unused, size = stack & 0xFFFF, stack >> 16
    if unused != 0xFFFF and size:
        if unused == 0:
            lines.append(f"  Stack: the run before used all {size} bytes, so it most likely overflowed into "
                         "the RAM variables below it (that does not fault on this chip; it corrupts).")
        else:
            lines.append(f"  Stack: the run before used at most {size - unused} of {size} bytes "
                         f"({unused} never touched).")
    return lines + live_lines(r)


def live_lines(r):
    return [f"Now: the main loop made {r[D_LOOPS]} passes in the last second."]


def fmt_ms(ms):
    if ms < 1000:
        return f"{ms} ms"
    s = ms / 1000
    if s < 120:
        return f"{s:.1f} s"
    h, m = int(s // 3600), int(s % 3600 // 60)
    return (f"{h} h " if h else "") + f"{m} min {int(s % 60)} s"


def cpu_lines(rx, tx):
    """Plain-words lines for the audio block cycle registers (REG_RXBLK, REG_TXBLK)."""
    if not rx and not tx:
        return ["Audio processing: no figures (no audio since power-up, or firmware that processes audio "
                "sample by sample and does not count it)."]
    lines = []
    for name, w in (("recording", rx), ("playback", tx)):
        cmax, cavg = w & 0xFFFF, w >> 16
        if not w:
            lines.append(f"Audio processing, {name}: no figures yet (not since power-up).")
            continue
        most = f"{cmax}{' or more' if cmax == 0xFFFF else ''}"
        lines.append(f"Audio processing, {name}: {cavg} cycles per 1 ms block on average "
                     f"({100 * cavg / BLOCK_CYCLES:.0f}% of the processor), {most} at most "
                     f"({100 * cmax / BLOCK_CYCLES:.0f}%).")
    if rx and tx:
        both = (rx >> 16) + (tx >> 16)
        lines.append(f"Both directions together: {100 * both / BLOCK_CYCLES:.0f}% on average (each figure is "
                     "from its last 85 ms of running, kept after it stops).")
    return lines


def diag(aioc):
    regs = {a: aioc.read(a) for a in DIAG_REGS}
    for line in describe_diag(regs):
        print(line)
    if regs[D_MARKER] == DIAG_MARKER:
        for line in cpu_lines(aioc.read(REG_RXBLK), aioc.read(REG_TXBLK)):
            print(line)


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
    ap.add_argument("command", choices=("status", "apply", "off", "profiles", "rx", "diag"))
    ap.add_argument("args", nargs="*", help="for apply: a profile name, or custom and 16 words; "
                                            "for rx: status, on or off")
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
    if a.command == "rx":
        if len(a.args) != 1 or a.args[0] not in ("status", "on", "off"):
            raise SystemExit("rx takes one of: status, on, off")
        if a.store and a.args[0] == "status":
            raise SystemExit("--store goes with rx on or rx off")
    elif a.store and a.command not in ("apply", "off"):
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
    elif a.args and a.command != "rx":
        raise SystemExit(f"{a.command} takes no arguments")

    try:
        vid, pid = (int(x, 16) for x in a.usb_id.split(":"))
    except ValueError:
        raise SystemExit("--usb-id must look like 1209:7388")

    aioc = open_aioc(vid, pid, a.serial)
    try:
        if a.command == "status":
            status(aioc)
        elif a.command == "diag":
            diag(aioc)
        elif a.command == "apply":
            _, changed = load(aioc, *target)
            print(f"Equaliser: {label} " + ("loaded" if changed else "was already loaded") + " (RAM).")
            if target[1] >> 16:
                print(f"It runs only while the host plays audio at {target[1] >> 16} Hz.")
            if a.store:
                store(aioc, a.yes)
            else:
                print("Not stored: at the next power-up the AIOC goes back to its stored setting (or its "
                      "default, k5-red). Add --store to keep this.")
        elif a.command == "rx":
            if not has_rx(aioc):
                raise SystemExit("this AIOC's firmware has no receive equaliser (it needs an aioc-packet "
                                 "release that has one)")
            if a.args[0] == "status":
                rx_status(aioc)
            else:
                on = a.args[0] == "on"
                changed = rx_set(aioc, RX_K5 if on else RX_OFF)
                print("Receive equaliser: " + ("switched " if changed else "was already ")
                      + ("on" if on else "off") + " (RAM).")
                if on:
                    print("It runs only while the host records at 48000 Hz.")
                if a.store:
                    store(aioc, a.yes, own=RX_REGS)
                else:
                    print("Not stored: at the next power-up the AIOC goes back to its stored setting (or its "
                          "default, on). Add --store to keep this.")
        elif a.command == "off":
            changed = switch_off(aioc)
            print("Equaliser: " + ("switched off" if changed else "was already off") + " (RAM).")
            if a.store:
                store(aioc, a.yes)
            else:
                print("Not stored: the equaliser comes back on at the next power-up unless it was stored off. "
                      "Add --store to keep it off.")
    finally:
        aioc.close()


if __name__ == "__main__":
    main(sys.argv[1:])
