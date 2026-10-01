#!/usr/bin/env python3
"""Tests for aioc_eq.py against a simulated AIOC (no hardware, no hidapi needed).

    python3 tools/test_aioc_eq.py

The simulation follows the firmware's feature-report handling (usb_hid.c) and its
settings code (settings.c): 256 registers, writes refused at 0xC0 and above, a
settings page in flash that store writes whole and recall reads back."""
import contextlib
import io
import os
import re
import subprocess
import sys
import types
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import aioc_eq  # noqa: E402

K5 = aioc_eq.PROFILES["k5-red"]


class FakeAioc:
    """The firmware side of the HID feature report."""

    def __init__(self, rx=True, live=None):
        self.rx = rx                    # firmware with the receive equaliser
        self.live = rx if live is None else live    # updates 0xEF (main-loop passes) every second
        self.reads = []                 # addresses read, in order
        self.ram = self.defaults()
        self.flash = None               # nothing stored: the page is erased
        self.addr = 0
        self.log = []                   # (ctrl, addr, value) of every set-feature
        self.fail_store = False

    # hidapi device interface
    def send_feature_report(self, data):
        assert len(data) == 7 and data[0] == 0, data
        ctrl, addr = data[1], data[2]
        value = data[3] | data[4] << 8 | data[5] << 16 | data[6] << 24
        self.log.append((ctrl, addr, value))
        if ctrl & 0x01 and addr < 0xC0:
            self.ram[addr] = value
        if ctrl & 0x40:
            self.ram = list(self.flash) if self.flash else self.defaults()
            if self.rx:
                self.ram[aioc_eq.REG_RXPAGE] = aioc_eq.RXPAGE_MARKER   # set on every recall
        if ctrl & 0x80:
            self.flash = list(self.ram) if not self.fail_store else [0xFFFFFFFF] * 256
        assert not ctrl & 0x20, "reboot bit sent"
        assert not ctrl & 0x10, "load-defaults bit sent"
        self.addr = addr
        return 7

    def get_feature_report(self, report_id, size):
        self.reads.append(self.addr)
        v = self.ram[self.addr]
        return [0, 0, self.addr, v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24 & 0xFF]

    def open_path(self, path):
        pass

    def close(self):
        pass

    def defaults(self):
        ram = [0] * 256
        for a, v in aioc_eq.DEFAULTS.items():
            ram[a] = v
        if not self.rx:
            ram[aioc_eq.REG_RXCTRL] = 0
        else:
            ram[aioc_eq.REG_RXPAGE] = aioc_eq.RXPAGE_MARKER
        return ram

    def wait(self, seconds):
        """time.sleep in the tool: a live firmware has counted another second's passes"""
        if self.live:
            self.ram[aioc_eq.D_LOOPS] += 1

    def writes(self):
        return [(a, v) for c, a, v in self.log if c & 0x01]


def fake_hid(device):
    mod = types.ModuleType("hid")
    mod.enumerate = lambda vid, pid: [{"path": b"x", "serial_number": "123", "interface_number": 3}]
    mod.device = lambda: device
    return mod


class ToolCase(unittest.TestCase):
    def run_tool(self, dev, *args, stdin_tty=False, answer="y"):
        out = io.StringIO()
        with mock.patch.dict(sys.modules, {"hid": fake_hid(dev)}), \
             mock.patch("sys.stdin.isatty", return_value=stdin_tty), \
             mock.patch("builtins.input", return_value=answer), \
             mock.patch("time.sleep", side_effect=dev.wait), \
             contextlib.redirect_stdout(out):
            aioc_eq.main(list(args))
        return out.getvalue()


class ToolTests(ToolCase):

    def test_apply_writes_only_eq_registers_control_last(self):
        dev = FakeAioc()
        dev.ram[0xB0:0xC0] = [0] * 16       # stored off
        self.run_tool(dev, "apply", "k5-red")
        w = dev.writes()
        self.assertTrue(all(a in aioc_eq.EQ_REGS for a, _ in w))
        self.assertEqual(w[-1], (0xBF, K5["ctrl"]))
        self.assertEqual(dev.ram[0xB0:0xBF], K5["coefs"])
        self.assertEqual(dev.ram[0xBF], K5["ctrl"])
        self.assertIsNone(dev.flash, "stored without --store")
        self.assertEqual(dev.ram[0x24], 0x404)

    def test_apply_twice_is_a_no_op(self):
        dev = FakeAioc()
        self.run_tool(dev, "apply", "k5-red")
        n = len(dev.writes())
        self.run_tool(dev, "apply", "k5-red")
        self.assertEqual(len(dev.writes()), n)

    def test_same_control_word_other_coefficients_switches_off_first(self):
        dev = FakeAioc()
        dev.ram[0xB0] = 0x20000000
        dev.ram[0xBF] = K5["ctrl"]
        self.run_tool(dev, "apply", "k5-red")
        w = dev.writes()
        self.assertEqual(w[0], (0xBF, 0))
        self.assertEqual(w[-1], (0xBF, K5["ctrl"]))

    def test_off(self):
        dev = FakeAioc()
        self.run_tool(dev, "apply", "k5-red")
        dev.log.clear()
        self.run_tool(dev, "off")
        self.assertEqual(dev.writes()[0], (0xBF, 0))
        self.assertEqual(dev.ram[0xB0:0xC0], [0] * 16)

    def test_custom(self):
        dev = FakeAioc()
        words = [f"0x{w:08X}" for w in K5["coefs"]] + ["0x00001101"]
        self.run_tool(dev, "apply", "custom", *words)
        self.assertEqual(dev.ram[0xBF], 0x1101)
        with self.assertRaises(SystemExit):
            self.run_tool(dev, "apply", "custom", "1", "2")

    def test_store_persists_whole_page(self):
        dev = FakeAioc()
        dev.ram[0x24] = 0x4             # changed earlier by another tool: stored too
        self.run_tool(dev, "apply", "k5-red", "--store", "--yes")
        self.assertEqual(dev.flash[0xB0:0xC0], K5["coefs"] + [K5["ctrl"]])
        self.assertEqual(dev.flash[0x24], 0x4)
        self.assertEqual(aioc_eq.other_changes({a: dev.ram[a] for a in aioc_eq.WRITABLE}), {0x24: 0x4})

    def test_store_asks_and_can_be_declined(self):
        dev = FakeAioc()
        with self.assertRaises(SystemExit):
            self.run_tool(dev, "apply", "k5-red", "--store", stdin_tty=True, answer="n")
        self.assertIsNone(dev.flash)
        self.assertEqual(dev.ram[0xBF], K5["ctrl"], "the RAM change stays")
        self.run_tool(dev, "apply", "k5-red", "--store", stdin_tty=True, answer="y")
        self.assertEqual(dev.flash[0xBF], K5["ctrl"])

    def test_store_refused_without_tty_or_yes(self):
        dev = FakeAioc()
        with self.assertRaises(SystemExit):
            self.run_tool(dev, "apply", "k5-red", "--store")
        self.assertIsNone(dev.flash)

    def test_failed_store_puts_ram_back(self):
        dev = FakeAioc()
        dev.ram[0x24] = 0x4
        dev.fail_store = True
        with self.assertRaises(SystemExit):
            self.run_tool(dev, "apply", "k5-red", "--store", "--yes")
        self.assertEqual(dev.ram[0x24], 0x4)
        self.assertEqual(dev.ram[0xBF], K5["ctrl"])

    def test_status_does_not_write(self):
        dev = FakeAioc()
        self.run_tool(dev, "status")
        self.assertEqual(dev.writes(), [])

    def test_write_guard(self):
        with self.assertRaises(AssertionError):
            aioc_eq.Aioc(FakeAioc()).write(0x24, 0)

    def test_tx_status_mentions_rx(self):
        dev = FakeAioc()
        self.assertIn("Receive equaliser: on", self.run_tool(dev, "status"))
        self.assertNotIn("Receive equaliser", self.run_tool(FakeAioc(rx=False), "status"))
        self.assertEqual(dev.writes(), [])


class RxTests(ToolCase):
    def test_rx_off_and_on(self):
        dev = FakeAioc()
        self.run_tool(dev, "rx", "off")
        self.assertEqual(dev.writes(), [(0xAF, 0)])
        self.assertIsNone(dev.flash, "stored without --store")
        self.run_tool(dev, "rx", "off")
        self.assertEqual(dev.writes(), [(0xAF, 0)], "already off: no write")
        self.run_tool(dev, "rx", "on")
        self.assertEqual(dev.writes(), [(0xAF, 0), (0xAF, 1)])
        self.assertEqual(dev.ram[0xB0:0xC0], K5["coefs"] + [K5["ctrl"]], "the TX registers were touched")

    def test_rx_off_store(self):
        dev = FakeAioc()
        out = self.run_tool(dev, "rx", "off", "--store", "--yes")
        self.assertEqual(dev.flash[0xAF], 0)
        self.assertIn("Every other setting is at its firmware default", out)
        # Power-up: stays off
        dev.send_feature_report([0, 0x40, 0, 0, 0, 0, 0])
        self.assertEqual(dev.ram[0xAF], 0)

    def test_tx_store_says_how_rx_is_stored(self):
        dev = FakeAioc()
        self.run_tool(dev, "rx", "off")
        out = self.run_tool(dev, "apply", "k5-red", "--store", "--yes")
        self.assertIn("The receive equaliser is stored as it is now: off.", out)
        self.assertIn("Every other setting is at its firmware default", out)
        self.assertNotIn("0xAF", out)
        self.assertEqual(dev.flash[0xAF], 0)

    def test_rx_store_says_how_tx_is_stored(self):
        dev = FakeAioc()
        self.run_tool(dev, "off")
        out = self.run_tool(dev, "rx", "off", "--store", "--yes")
        self.assertIn("The transmit equaliser is stored as it is now: off.", out)
        self.assertNotIn("0xB0", out)

    def test_store_on_factory_defaults_lists_nothing_else(self):
        # A factory-default AIOC holds k5-red in 0xB0 to 0xBF: those are defaults, not changes
        dev = FakeAioc()
        out = self.run_tool(dev, "rx", "off", "--store", "--yes")
        self.assertNotIn("0xB", out)
        self.assertIn("The transmit equaliser is stored as it is now: k5-red", out)
        self.assertIn("Every other setting is at its firmware default", out)
        dev = FakeAioc()
        out = self.run_tool(dev, "apply", "k5-red", "--store", "--yes")
        self.assertIn("The receive equaliser is stored as it is now: on (UV-K5 profile).", out)
        self.assertIn("Every other setting is at its firmware default", out)
        self.assertEqual(aioc_eq.other_changes({a: dev.ram[a] for a in aioc_eq.WRITABLE}), {})

    def test_store_on_firmware_without_rx_says_nothing_about_it(self):
        dev = FakeAioc(rx=False)
        out = self.run_tool(dev, "apply", "k5-red", "--store", "--yes")
        self.assertNotIn("receive equaliser", out)

    def test_stale_rx_marker(self):
        # Downgraded firmware loaded a page stored by this one: the marker is there, but nothing
        # updates the registers
        dev = FakeAioc(rx=False)
        dev.ram[aioc_eq.REG_RXPAGE] = aioc_eq.RXPAGE_MARKER
        dev.ram[aioc_eq.D_LOOPS] = 123456
        with self.assertRaises(SystemExit) as cm:
            self.run_tool(dev, "rx", "status")
        self.assertIn("copy", str(cm.exception))
        self.assertNotIn("Receive equaliser", self.run_tool(dev, "status"))
        self.assertGreaterEqual(dev.reads.count(aioc_eq.D_LOOPS), 1 + aioc_eq.LIVE_TRIES)
        self.assertEqual(dev.writes(), [])

    def test_rx_refused_on_firmware_without_it(self):
        dev = FakeAioc(rx=False)
        for cmd in ("status", "on", "off"):
            with self.assertRaises(SystemExit):
                self.run_tool(dev, "rx", cmd)
        self.assertEqual(dev.writes(), [])

    def test_rx_status(self):
        dev = FakeAioc()
        out = self.run_tool(dev, "rx", "status")
        self.assertIn("on (UV-K5 profile)", out)
        self.assertIn("Not recording now", out)
        dev.ram[0xD0] = 2 << 24                    # recording
        dev.ram[0xD2] = 48000
        dev.ram[0xCA] = 0x00030101                 # profile 1, running, 3 clipped
        dev.ram[0xCC] = (180 << 16) | 240
        out = self.run_tool(dev, "rx", "status")
        self.assertIn("Running now: yes, 3 clipped", out)
        self.assertIn("180 cycles per sample on average, 240 at most", out)
        dev.ram[0xCA] = 0
        dev.ram[0xD2] = 16000
        self.assertIn("Recording runs at 16000 Hz", self.run_tool(dev, "rx", "status"))
        dev.ram[0xAF] = 7
        self.assertIn("reserved value", self.run_tool(dev, "rx", "status"))
        dev.ram[0xAF] = 1
        dev.ram[0xD2] = 48000
        dev.ram[0xCA] = 0x400 | (2 << 11)          # switched off by the overload guard, twice
        out = self.run_tool(dev, "rx", "status")
        self.assertIn("overload guard switched it off", out)
        self.assertIn("switched it off 2 time(s)", out)
        self.assertEqual(dev.writes(), [])

    def test_recording_state_with_vptt_bit(self):
        # Earlier firmware ORs the virtual PTT state into bit 0 of the record state field
        dev = FakeAioc()
        dev.ram[0xD2] = 48000
        dev.ram[0xCA] = 0x00000101
        for field, recording in ((2, True), (3, True), (1, False), (0, False)):
            dev.ram[0xD0] = field << 24
            out = self.run_tool(dev, "rx", "status")
            self.assertEqual("Not recording now" not in out, recording, f"record state {field}")
        dev.ram[0xD0] = 3 << 28                 # playing, with the virtual COS bit
        dev.ram[0xC8] = 0x1307
        self.assertNotIn("Not playing audio now", self.run_tool(dev, "status"))

    def test_rx_arguments(self):
        dev = FakeAioc()
        for args in (("rx",), ("rx", "maybe"), ("rx", "on", "off"), ("rx", "status", "--store")):
            with self.assertRaises(SystemExit):
                self.run_tool(dev, *args)
        self.assertEqual(dev.writes(), [])


IWDG, PIN, POR, SFT, WWDG = 1 << 29, 1 << 26, 1 << 27, 1 << 28, 1 << 30
VALID, FAULT, REBOOT = 1 << 18, 1 << 16, 1 << 17


class DiagTests(ToolCase):
    def device(self, **regs):
        dev = FakeAioc()
        dev.ram[0xE0] = aioc_eq.DIAG_MARKER
        dev.ram[0xEC] = dev.ram[0xED] = 0xFFFFFFFF
        dev.ram[0xEE] = 0xFFFF | (5120 << 16)
        for name, value in regs.items():
            dev.ram[getattr(aioc_eq, "D_" + name.upper())] = value
        return dev

    def diag(self, dev):
        out = self.run_tool(dev, "diag")
        self.assertEqual(dev.writes(), [], "diag must not write")
        return out

    def test_firmware_without_diagnostics(self):
        self.assertIn("has no reset diagnostics", self.diag(FakeAioc()))

    def test_power_on(self):
        out = self.diag(self.device(reset=POR | PIN, loops=12345))
        self.assertIn("Last reset: power-on", out)
        self.assertIn("PORRSTF, PINRSTF", out)
        self.assertIn("12345 passes", out)
        self.assertNotIn("Fault", out)

    def test_watchdog_starvation(self):
        out = self.diag(self.device(reset=IWDG | PIN | VALID | 3, uptime=754000,
                                    age0=152 | (0 << 16), age1=0 | (1 << 16), stack=2900 | (5120 << 16)))
        self.assertIn("Last reset: independent watchdog: the main loop stopped", out)
        self.assertIn("Resets since power-on: 3", out)
        self.assertIn("12 min 34 s", out)
        self.assertIn("main loop 152 ms", out)
        self.assertIn("watchdog starvation", out)
        self.assertIn("used at most 2220 of 5120 bytes", out)
        self.assertIn("No fault recorded", out)

    def test_watchdog_everything_stopped(self):
        out = self.diag(self.device(reset=IWDG | VALID | 1, uptime=5000, age0=0 | (1 << 16), age1=0xFFFF0000))
        self.assertIn("everything stopped at once", out)

    def test_hardfault(self):
        out = self.diag(self.device(reset=IWDG | PIN | VALID | FAULT | 1, fault=3 | (1 << 16),
                                    pc=0x08004568, lr=0x08001235, xpsr=0x21000000 | (16 + 18),
                                    cfsr=0x00008200, hfsr=0x40000000, bfar=0x40021000, excret=0xFFFFFFE9,
                                    uptime=1234, age0=0, age1=0))
        self.assertIn("Last reset: independent watchdog, after a HardFault", out)
        self.assertIn("At PC 0x08004568, LR 0x08001235, while running the ADC interrupt", out)
        self.assertIn("addr2line -f -e aioc-fw.elf 0x08004568", out)
        self.assertIn("PRECISERR", out)
        self.assertIn("BFAR (the address it tried to use): 0x40021000", out)
        self.assertIn("FORCED", out)
        self.assertNotIn("starvation", out)

    def test_fault_without_stack(self):
        out = self.diag(self.device(reset=IWDG | VALID | FAULT | 1, fault=3 | (1 << 16), cfsr=1 << 12,
                                    hfsr=0x40000000, stack=0 | (5120 << 16)))
        self.assertIn("no PC (a stack overflow, most likely)", out)
        self.assertIn("STKERR", out)
        self.assertIn("used all 5120 bytes", out)

    def test_unexpected_interrupt(self):
        out = self.diag(self.device(reset=IWDG | VALID | FAULT | 1, fault=16 + 29, pc=0x08000100, lr=0x08000200))
        self.assertIn("after a TIM3", out)

    def test_host_reboot_and_bootloader(self):
        self.assertIn("the host asked the AIOC to reboot", self.diag(self.device(reset=IWDG | VALID | REBOOT | 1)))
        self.assertIn("DFU bootloader", self.diag(self.device(reset=WWDG | PIN | VALID | 1)))
        self.assertIn("software reset", self.diag(self.device(reset=SFT | VALID | 1)))

    def test_record_lost(self):
        self.assertIn("did not survive", self.diag(self.device(reset=IWDG | PIN)))

    def test_audio_cpu(self):
        dev = self.device(reset=POR)
        self.assertIn("Audio processing: no figures", self.diag(dev))
        dev.ram[aioc_eq.REG_RXBLK] = (14400 << 16) | 18000
        out = self.diag(dev)
        self.assertIn("recording: 14400 cycles per 1 ms block on average (20% of the processor), 18000 at most (25%)", out)
        self.assertIn("playback: no figures yet", out)
        self.assertNotIn("together", out)
        dev.ram[aioc_eq.REG_TXBLK] = (7200 << 16) | 0xFFFF
        out = self.diag(dev)
        self.assertIn("playback: 7200 cycles per 1 ms block on average (10% of the processor), 65535 or more", out)
        self.assertIn("Both directions together: 30% on average", out)
        self.assertNotIn("Audio processing", self.diag(FakeAioc()), "no diagnostics, no CPU lines")

    def test_stale_diagnostics(self):
        dev = self.device(reset=IWDG | VALID | 1, loops=5000)
        dev.live = False
        out = self.diag(dev)
        self.assertIn("hold a copy", out)
        self.assertNotIn("Last reset", out)

    def test_diag_takes_no_arguments(self):
        with self.assertRaises(SystemExit):
            self.run_tool(self.device(), "diag", "x")


class RxEqModel(unittest.TestCase):
    """bench/rxeq_fixed.py, the bit-exact model of the firmware's RX equaliser: its WAV command."""

    def test_wav_command(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed")
        import tempfile
        import wave
        sys.path.insert(0, os.path.join(ROOT, "bench"))
        import rxeq_fixed
        x = (np.sin(np.arange(9600) * 2 * np.pi * 300 / 48000) * 12000).astype("<i2")
        with tempfile.TemporaryDirectory() as d:
            inp, out = os.path.join(d, "in.wav"), os.path.join(d, "out.wav")
            rxeq_fixed.write_wav(inp, x)
            subprocess.run([sys.executable, os.path.join(ROOT, "bench/rxeq_fixed.py"), inp, out],
                           check=True, capture_output=True)
            with wave.open(out, "rb") as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 48000))
                y = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        self.assertTrue(np.array_equal(y, rxeq_fixed.run(x)))
        self.assertFalse(np.array_equal(y, x))


class RegisterAgreement(unittest.TestCase):
    """The receive equaliser's register addresses and marker are written down in the
    firmware and here; they must agree."""

    def test_rx_registers_match_firmware(self):
        with open(os.path.join(ROOT, "stm32/aioc-fw/Src/settings.h")) as f:
            text = f.read()

        def addr(name):
            return int(re.search(r"#define SETTINGS_REG_%s\s+(0x[0-9A-Fa-f]+)\s*$" % name, text, re.M).group(1), 16)
        self.assertEqual(addr("RXEQ_CTRL"), aioc_eq.REG_RXCTRL)
        self.assertEqual(addr("INFO_RXEQ"), aioc_eq.REG_RXINFO)
        self.assertEqual(addr("INFO_RXEQPAGE"), aioc_eq.REG_RXPAGE)
        self.assertEqual(addr("INFO_RXEQCYC"), aioc_eq.REG_RXCYC)
        self.assertEqual(aioc_eq.RXPAGE_MARKER, int.from_bytes(b"RXEQ", "little"))
        self.assertEqual(addr("INFO_AUDIO0"), aioc_eq.REG_AUDIO0)
        self.assertEqual(addr("INFO_AUDIO2"), aioc_eq.REG_AUDIO2)
        self.assertEqual(addr("INFO_RXCYC"), aioc_eq.REG_RXBLK)
        self.assertEqual(addr("INFO_TXCYC"), aioc_eq.REG_TXBLK)

    def test_diag_registers_match_firmware(self):
        with open(os.path.join(ROOT, "stm32/aioc-fw/Src/settings.h")) as f:
            text = f.read()

        def addr(name):
            return int(re.search(r"#define SETTINGS_REG_%s\s+(0x[0-9A-Fa-f]+)\s*$" % name, text, re.M).group(1), 16)
        for name, ours in (("INFO_DIAG", aioc_eq.D_MARKER), ("INFO_DIAGRESET", aioc_eq.D_RESET),
                           ("INFO_DIAGFAULT", aioc_eq.D_FAULT), ("INFO_DIAGPC", aioc_eq.D_PC),
                           ("INFO_DIAGLR", aioc_eq.D_LR), ("INFO_DIAGXPSR", aioc_eq.D_XPSR),
                           ("INFO_DIAGCFSR", aioc_eq.D_CFSR), ("INFO_DIAGHFSR", aioc_eq.D_HFSR),
                           ("INFO_DIAGMMFAR", aioc_eq.D_MMFAR), ("INFO_DIAGBFAR", aioc_eq.D_BFAR),
                           ("INFO_DIAGEXCRET", aioc_eq.D_EXCRET), ("INFO_DIAGUPTIME", aioc_eq.D_UPTIME),
                           ("INFO_DIAGAGE0", aioc_eq.D_AGE0), ("INFO_DIAGAGE1", aioc_eq.D_AGE1),
                           ("INFO_DIAGSTACK", aioc_eq.D_STACK), ("INFO_DIAGLOOPS", aioc_eq.D_LOOPS)):
            self.assertEqual(addr(name), ours, name)
        self.assertEqual(aioc_eq.DIAG_MARKER, int.from_bytes(b"DIAG", "little"))


class ProfileAgreement(unittest.TestCase):
    """The k5-red profile is written down in three places; they must agree."""

    def test_firmware_build_option_matches(self):
        with open(os.path.join(ROOT, "stm32/aioc-fw/Src/settings.h")) as f:
            text = f.read()
        ctrl = int(re.search(r"TXEQ_CTRL_DEFAULT\s+(0x[0-9A-Fa-f]+)UL", text).group(1), 16)
        block = re.search(r"TXEQ_COEF_DEFAULTS\s*\{([^}]*)\}", text).group(1)
        coefs = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", block)]
        self.assertEqual(ctrl, K5["ctrl"])
        self.assertEqual(coefs, K5["coefs"])

    def test_eq_py_design_matches(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy not installed")
        out = subprocess.run([sys.executable, os.path.join(ROOT, "bench/eq.py")],
                             capture_output=True, text=True, check=True).stdout
        line = next(ln for ln in out.splitlines() if "apply custom" in ln and not ln.startswith("#"))
        words = [int(x, 16) for x in line.split("apply custom", 1)[1].split()]
        self.assertEqual(words, K5["coefs"] + [K5["ctrl"]])


if __name__ == "__main__":
    unittest.main()
