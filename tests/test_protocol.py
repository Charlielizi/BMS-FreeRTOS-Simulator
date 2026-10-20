"""End-to-end scenarios against the running FreeRTOS Windows port."""

from __future__ import annotations

import json
import queue
import subprocess
import threading
import time
import unittest
from pathlib import Path

SIM = Path(__file__).resolve().parents[1] / "build" / "bms_sim.exe"


class ProtocolScenarios(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.process = subprocess.Popen(
            [str(SIM)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        cls.packets: queue.Queue[dict] = queue.Queue()

        def read_output() -> None:
            assert cls.process.stdout is not None
            for line in cls.process.stdout:
                try:
                    cls.packets.put(json.loads(line))
                except json.JSONDecodeError:
                    cls.packets.put({"type": "invalid", "line": line})

        cls.reader = threading.Thread(target=read_output, daemon=True)
        cls.reader.start()
        cls.next_id = 1

    @classmethod
    def tearDownClass(cls) -> None:
        cls.process.terminate()
        try:
            cls.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait(timeout=3)

    def command(self, command_type: str, **values: object) -> dict:
        identifier = self.next_id
        type(self).next_id += 1
        packet = {"v": 1, "id": identifier, "type": command_type, **values}
        assert self.process.stdin is not None
        self.process.stdin.write(json.dumps(packet) + "\n")
        self.process.stdin.flush()
        return self.wait_for("ack", lambda p: p.get("id") == identifier, 5)

    def wait_for(self, kind: str, predicate, timeout: float = 5) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f"simulator exited: {self.process.returncode}")
            try:
                packet = self.packets.get(timeout=0.2)
            except queue.Empty:
                continue
            self.assertNotEqual(packet.get("type"), "invalid", packet)
            if packet.get("type") == kind and predicate(packet):
                return packet
        self.fail(f"timed out waiting for {kind}")

    def test_01_ping_and_validation(self) -> None:
        self.assertTrue(self.command("ping")["ok"])
        self.assertFalse(self.command("set_current", ma=6000)["ok"])

    def test_02_charge_and_ov_recovery(self) -> None:
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_current", ma=1000)["ok"])
        self.wait_for("snapshot", lambda p: p["actual_ma"] > 0, 5)
        self.assertTrue(self.command("set_cell", index=1, mv=4250)["ok"])
        fault = self.wait_for("snapshot", lambda p: p["faults"] & 1, 6)
        self.assertFalse(fault["charge_on"])
        self.assertTrue(fault["discharge_on"])
        self.assertTrue(self.command("set_cell", index=1, mv=None)["ok"])
        recovered = self.wait_for("snapshot", lambda p: not (p["faults"] & 1) and p["charge_on"], 5)
        self.assertTrue(recovered["valid"])

    def test_03_sensor_fail_and_recovery(self) -> None:
        self.assertTrue(self.command("inject_fault", kind="nack", on=True)["ok"])
        fault = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 9), 5)
        self.assertFalse(fault["charge_on"])
        self.assertFalse(fault["discharge_on"])
        self.assertFalse(fault["valid"])
        self.assertTrue(self.command("inject_fault", kind="nack", on=False)["ok"])
        restored = self.wait_for("snapshot", lambda p: p["valid"] and not (p["faults"] & (1 << 9)), 5)
        self.assertTrue(restored["charge_on"])

    def test_04_balance_stop(self) -> None:
        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=True)["ok"])
        self.assertTrue(self.command("set_current", ma=0)["ok"])
        for index, mv in enumerate([3800, 3700, 3780, 3710, 3760], 1):
            self.assertTrue(self.command("set_cell", index=index, mv=mv)["ok"])
        balanced = self.wait_for("snapshot", lambda p: p["balance_mask"] == 0x15, 5)
        self.assertEqual(balanced["mode"], "STANDBY")
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        stopped = self.wait_for("snapshot", lambda p: p["balance_mask"] == 0, 5)
        self.assertEqual(stopped["balance_mask"], 0)

    def test_05_overcurrent_and_temperature(self) -> None:
        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_current", ma=3000)["ok"])
        occ = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 1), 5)
        self.assertFalse(occ["charge_on"])

        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_current", ma=-3000)["ok"])
        ocd = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 5), 5)
        self.assertFalse(ocd["discharge_on"])

        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_temp", c=75)["ok"])
        self.assertTrue(self.command("set_current", ma=1000)["ok"])
        hot = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 2), 5)
        self.assertFalse(hot["charge_on"])

        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_temp", c=-25)["ok"])
        self.assertTrue(self.command("set_current", ma=-1000)["ok"])
        cold = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 8), 5)
        self.assertFalse(cold["discharge_on"])

    def test_06_uv_crc_and_scd(self) -> None:
        self.assertTrue(self.command("reset")["ok"])
        self.assertTrue(self.command("set_enable", charge=True, discharge=True, balance=False)["ok"])
        self.assertTrue(self.command("set_cell", index=1, mv=3000)["ok"])
        uv = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 4), 7)
        self.assertFalse(uv["discharge_on"])
        self.assertTrue(self.command("set_cell", index=1, mv=None)["ok"])
        self.wait_for("snapshot", lambda p: not p["faults"] & (1 << 4), 5)

        self.assertTrue(self.command("inject_fault", kind="crc", on=True)["ok"])
        crc = self.wait_for("snapshot", lambda p: p["error"] == "crc" and p["faults"] & (1 << 9), 5)
        self.assertFalse(crc["valid"])
        self.assertTrue(self.command("inject_fault", kind="crc", on=False)["ok"])
        self.wait_for("snapshot", lambda p: p["valid"] and not p["faults"] & (1 << 9), 5)

        self.assertTrue(self.command("inject_fault", kind="scd", on=True)["ok"])
        scd = self.wait_for("snapshot", lambda p: p["faults"] & (1 << 6), 5)
        self.assertFalse(scd["discharge_on"])


if __name__ == "__main__":
    unittest.main()
