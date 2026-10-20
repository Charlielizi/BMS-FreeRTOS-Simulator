"""Qt process lifecycle and export smoke test."""

from __future__ import annotations

import csv
import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "desktop"))

from PySide6.QtCore import QProcess, QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402
from app import BmsWindow  # noqa: E402


def main() -> int:
    app = QApplication([])
    window = BmsWindow(ROOT / "build" / "bms_sim.exe")
    window.show()
    failures: list[str] = []

    def charge() -> None:
        if window.last_snapshot is None:
            failures.append("no initial telemetry")
        window.preset("charge")

    def stop() -> None:
        if not window.last_snapshot or window.last_snapshot["actual_ma"] <= 0:
            failures.append("charge preset did not reach simulator")
        window.process.kill()

    def restart() -> None:
        if window.process.state() != QProcess.ProcessState.NotRunning:
            failures.append("simulator did not stop")
        window.start_process()

    def finish() -> None:
        if not window.last_snapshot or not window.last_snapshot["valid"]:
            failures.append("no telemetry after reconnect")
        if window.process.state() != QProcess.ProcessState.Running:
            failures.append("simulator not running after reconnect")
        target = ROOT / "build" / "ui-export.csv"
        window.save_csv(target)
        with target.open(encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.reader(stream))
        if len(rows) < 2 or "cell1_mv" not in rows[0]:
            failures.append("CSV export failed")
        window.close()
        app.exit(1 if failures else 0)

    QTimer.singleShot(1100, charge)
    QTimer.singleShot(2500, stop)
    QTimer.singleShot(2800, restart)
    QTimer.singleShot(4500, finish)
    code = app.exec()
    if failures:
        print("GUI smoke failed: " + ", ".join(failures), file=sys.stderr)
    else:
        print("GUI lifecycle and export passed")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
