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

    def __init__(self):
        self.ram = [0] * 256
        for a, v in aioc_eq.DEFAULTS.items():
            self.ram[a] = v
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
        if ctrl & 0x80:
            self.flash = list(self.ram) if not self.fail_store else [0xFFFFFFFF] * 256
        assert not ctrl & 0x20, "reboot bit sent"
        assert not ctrl & 0x10, "load-defaults bit sent"
        self.addr = addr
        return 7

    def get_feature_report(self, report_id, size):
        v = self.ram[self.addr]
        return [0, 0, self.addr, v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24 & 0xFF]

    def open_path(self, path):
        pass

    def close(self):
        pass

    @staticmethod
    def defaults():
        ram = [0] * 256
        for a, v in aioc_eq.DEFAULTS.items():
            ram[a] = v
        return ram

    def writes(self):
        return [(a, v) for c, a, v in self.log if c & 0x01]


def fake_hid(device):
    mod = types.ModuleType("hid")
    mod.enumerate = lambda vid, pid: [{"path": b"x", "serial_number": "123", "interface_number": 3}]
    mod.device = lambda: device
    return mod


class ToolTests(unittest.TestCase):
    def run_tool(self, dev, *args, stdin_tty=False, answer="y"):
        with mock.patch.dict(sys.modules, {"hid": fake_hid(dev)}), \
             mock.patch("sys.stdin.isatty", return_value=stdin_tty), \
             mock.patch("builtins.input", return_value=answer), \
             contextlib.redirect_stdout(io.StringIO()):
            aioc_eq.main(list(args))

    def test_apply_writes_only_eq_registers_control_last(self):
        dev = FakeAioc()
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
