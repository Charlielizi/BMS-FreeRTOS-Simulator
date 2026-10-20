"""Serial framing and the read-only board view, without physical hardware."""

from __future__ import annotations

import csv
import os
import random
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "desktop"))

from PySide6.QtSerialPort import QSerialPort  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402
from app import BmsWindow  # noqa: E402
from serial_protocol import FRAME_SIZE, SerialFrameParser, make_snapshot  # noqa: E402


def frame(seq: int = 7, **changes) -> bytes:
    values = dict(seq=seq, ms=1200, cell_mv=[3700, 3710, 3720, 3730, 3740],
                  pack_mv=18600, actual_ma=-850, temp_c10=285, soc_pct10=736,
                  mode=2, faults=(1 << 4) | (1 << 9), balance_mask=0b00101,
                  mos_bits=2, valid_mask=0x7F,
                  ages_ms=(30, 40, 600, 1900, 150))
    values.update(changes)
    return make_snapshot(**values)


class FrameTests(unittest.TestCase):
    def test_firmware_encoder_matches_desktop_decoder(self) -> None:
        vector = ROOT / "build" / "board_frame_vector.exe"
        if not vector.is_file():
            self.skipTest("先运行 build.ps1 -Tests 生成板端编码器测试程序")
        encoded = bytes.fromhex(subprocess.check_output([str(vector)], text=True).strip())
        self.assertEqual(encoded, frame())
        self.assertEqual(SerialFrameParser().feed(encoded)[0]["actual_ma"], -850)

    def test_random_fragments_and_fields(self) -> None:
        wire = frame()
        self.assertEqual(len(wire), FRAME_SIZE)
        rng = random.Random(50)
        for _ in range(40):
            parser = SerialFrameParser()
            data = b"noise" + wire + frame(seq=8, faults=0)
            snapshots = []
            while data:
                n = rng.randint(1, 17)
                snapshots.extend(parser.feed(data[:n]))
                data = data[n:]
            self.assertEqual([s["seq"] for s in snapshots], [7, 8])
            self.assertEqual(snapshots[0]["cell_mv"], [3700, 3710, 3720, 3730, 3740])
            self.assertEqual(snapshots[0]["mode"], "DISCHARGE")
            self.assertEqual(snapshots[0]["faults"], (1 << 4) | (1 << 9))
            self.assertEqual(snapshots[0]["ages_ms"], [30, 40, 600, 1900, 150])

    def test_corruption_version_length_and_sequence_recovery(self) -> None:
        corrupt = bytearray(frame())
        corrupt[20] ^= 0x40
        bad_version = bytearray(frame())
        bad_version[2] = 2
        bad_length = bytearray(frame())
        bad_length[4] = 250
        parser = SerialFrameParser()
        result = parser.feed(bytes(corrupt) + bytes(bad_version) + bytes(bad_length)
                             + frame(seq=9) + frame(seq=11))
        self.assertEqual([s["seq"] for s in result], [9, 11])
        self.assertTrue(any("CRC" in e for e in parser.errors))
        self.assertTrue(any("版本" in e for e in parser.errors))
        self.assertTrue(any("序号跳变" in e for e in parser.errors))

    def test_invalid_fields_are_flagged(self) -> None:
        snapshot = SerialFrameParser().feed(frame(valid_mask=0x15))[0]
        self.assertEqual(snapshot["valid_mask"], 0x15)
        self.assertFalse(snapshot["charge_on"])
        self.assertEqual(snapshot["actual_ma"], -850)


class FakePort:
    def __init__(self) -> None:
        self.active = False
        self.fail_open = False

    def isOpen(self) -> bool:  # noqa: N802
        return self.active

    def close(self) -> None:
        self.active = False

    def setPortName(self, name: str) -> None:  # noqa: N802
        self.name = name

    def setBaudRate(self, value: int) -> None:  # noqa: N802
        self.baud = value

    def setDataBits(self, value) -> None:  # noqa: N802
        pass

    def setParity(self, value) -> None:  # noqa: N802
        pass

    def setStopBits(self, value) -> None:  # noqa: N802
        pass

    def setFlowControl(self, value) -> None:  # noqa: N802
        pass

    def open(self, mode) -> bool:
        self.active = not self.fail_open
        return self.active

    def errorString(self) -> str:  # noqa: N802
        return "设备占用或拔出"


class BoardViewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:  # noqa: N802
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self) -> None:
        self.window = BmsWindow(ROOT / "build" / "bms_sim.exe")
        self.window.source_selector.setCurrentIndex(1)
        self.window.serial = FakePort()
        self.window.port_selector.addItem("测试串口", "COM99")
        self.window.port_selector.setCurrentIndex(self.window.port_selector.count() - 1)

    def tearDown(self) -> None:
        self.window.close()

    def test_board_live_csv_stale_unplug_and_reconnect(self) -> None:
        self.window.start_serial()
        self.assertTrue(self.window.serial.isOpen())
        self.assertTrue(self.window.sim_controls[0].isHidden())
        self.window.consume_serial_bytes(frame()[:15])
        self.assertIsNone(self.window.last_snapshot)
        self.window.consume_serial_bytes(frame()[15:])
        self.assertEqual(self.window.last_snapshot["source"], "board")
        self.assertIn("-0.850", self.window.metric_labels["current"].text())
        self.assertEqual(self.window.balance_labels[0].text(), "● 均衡")
        self.window.send("set_current", ma=1000)
        self.assertIn("只读", self.window.log.toPlainText())
        with tempfile.TemporaryDirectory() as directory:
            export = Path(directory) / "board.csv"
            self.window.save_csv(export)
            with export.open(encoding="utf-8-sig", newline="") as stream:
                row = list(csv.DictReader(stream))[0]
            self.assertEqual(row["source"], "board")
            self.assertEqual(row["age_temp_ms"], "1900")
        self.window.serial_last_frame = time.monotonic() - 2
        self.window.check_serial_timeout()
        self.assertEqual(self.window.connection.text(), "数据过期")
        self.assertEqual(self.window.metric_labels["current"].text(), "—")
        self.window.serial_error(QSerialPort.SerialPortError.ResourceError)
        self.assertFalse(self.window.serial.isOpen())
        self.assertEqual(self.window.connection.text(), "串口已断开")
        self.window.start_serial()
        self.window.consume_serial_bytes(frame(seq=1, faults=0))
        self.assertIn("实板遥测", self.window.connection.text())

    def test_port_open_failure_and_unknown_fields(self) -> None:
        self.window.serial.fail_open = True
        self.window.start_serial()
        self.assertEqual(self.window.connection.text(), "串口打开失败")
        self.window.serial.fail_open = False
        self.window.start_serial()
        self.window.consume_serial_bytes(frame(valid_mask=0x11))
        self.assertEqual(self.window.metric_labels["pack"].text(), "—")
        self.assertEqual(self.window.metric_labels["mos"].text(), "— / —")
        self.assertEqual(self.window.balance_labels[0].text(), "—")


if __name__ == "__main__":
    unittest.main()
