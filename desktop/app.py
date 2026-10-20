"""Windows desktop console for the FreeRTOS BMS simulator."""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

from PySide6.QtCharts import QChart, QChartView, QLineSeries, QValueAxis
from PySide6.QtCore import QIODevice, QPointF, QProcess, QTimer, Qt
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtSerialPort import QSerialPort, QSerialPortInfo
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFileDialog, QFrame, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPlainTextEdit,
    QProgressBar, QPushButton, QSpinBox, QSplitter, QTabWidget,
    QVBoxLayout, QWidget,
)
from serial_protocol import SerialFrameParser

FAULT_NAMES = [
    "单体过压", "充电过流", "充电过温", "充电低温", "单体欠压",
    "放电过流", "短路", "放电过温", "放电低温", "采集失败",
]


class TrendChart(QChartView):
    def __init__(self, title: str, suffix: str, color: str) -> None:
        self.chart_obj = QChart()
        self.chart_obj.setTitle(title)
        self.chart_obj.setBackgroundBrush(QColor("#172233"))
        self.chart_obj.setTitleBrush(QColor("#e7eef8"))
        self.chart_obj.legend().hide()
        self.series = QLineSeries()
        self.series.setColor(QColor(color))
        self.chart_obj.addSeries(self.series)
        self.x_axis = QValueAxis()
        self.x_axis.setTitleText("设备运行时间 / s")
        self.y_axis = QValueAxis()
        self.y_axis.setTitleText(suffix)
        for axis in (self.x_axis, self.y_axis):
            axis.setLabelsBrush(QColor("#c6d5e5"))
            axis.setTitleBrush(QColor("#c6d5e5"))
            axis.setGridLineColor(QColor("#324154"))
        self.chart_obj.addAxis(self.x_axis, Qt.AlignmentFlag.AlignBottom)
        self.chart_obj.addAxis(self.y_axis, Qt.AlignmentFlag.AlignLeft)
        self.series.attachAxis(self.x_axis)
        self.series.attachAxis(self.y_axis)
        super().__init__(self.chart_obj)
        self.setRenderHint(QPainter.RenderHint.Antialiasing)

    def update_points(self, points: list[tuple[float, float]]) -> None:
        if not points:
            return
        self.series.replace([QPointF(x, y) for x, y in points])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        self.x_axis.setRange(max(0, min(xs)), max(xs) + 1)
        low, high = min(ys), max(ys)
        margin = max((high - low) * 0.15, 0.1)
        self.y_axis.setRange(low - margin, high + margin)


class BmsWindow(QMainWindow):
    def __init__(self, sim_path: Path) -> None:
        super().__init__()
        self.sim_path = sim_path
        self.process = QProcess(self)
        self.process.readyReadStandardOutput.connect(self.read_stdout)
        self.process.readyReadStandardError.connect(self.read_stderr)
        self.process.started.connect(self.process_started)
        self.process.finished.connect(self.process_finished)
        self.process.errorOccurred.connect(self.process_error)
        self.serial = QSerialPort(self)
        self.serial.readyRead.connect(self.read_serial)
        self.serial.errorOccurred.connect(self.serial_error)
        self.serial_parser = SerialFrameParser()
        self.serial_last_frame: float | None = None
        self.serial_timeout = QTimer(self)
        self.serial_timeout.setInterval(200)
        self.serial_timeout.timeout.connect(self.check_serial_timeout)
        self.serial_timeout.start()
        self.source = "sim"
        self.buffer = bytearray()
        self.next_id = 1
        self.rows: deque[dict] = deque(maxlen=3000)
        self.last_snapshot: dict | None = None
        self._setting_controls = False
        self.setWindowTitle("五串 BMS · 仿真与实板监测")
        self.resize(1320, 850)
        self.build_ui()
        self.setStyleSheet("""
            QMainWindow, QWidget { background: #101927; color: #e7eef8; font-size: 13px; }
            QGroupBox { border: 1px solid #34445a; border-radius: 7px; margin-top: 9px; padding: 12px; font-weight: 600; }
            QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
            QPushButton { background: #254363; border: 1px solid #3d6288; border-radius: 5px; padding: 6px 10px; }
            QPushButton:hover { background: #32618a; }
            QPushButton:disabled { background: #25303c; color: #718093; }
            QSpinBox { background: #1d2c3e; border: 1px solid #43546a; padding: 5px; }
            QPlainTextEdit { background: #111c2a; border: 1px solid #34445a; }
            QProgressBar { background: #243246; border: 0; border-radius: 4px; text-align: center; }
            QProgressBar::chunk { background: #34a9c6; border-radius: 4px; }
            QTabWidget::pane { border: 1px solid #34445a; }
            QTabBar::tab { background: #1e3149; padding: 7px 16px; }
            QTabBar::tab:selected { background: #32618a; }
        """)
        QTimer.singleShot(0, self.start_process)

    def create_metric_card(self, name: str) -> tuple[QFrame, QLabel]:
        frame = QFrame()
        frame.setStyleSheet("QFrame { background:#1a2a3e; border:1px solid #30445e; border-radius:8px; }")
        layout = QVBoxLayout(frame)
        title = QLabel(name)
        title.setStyleSheet("border:0; color:#9fb5cd;")
        value = QLabel("—")
        value.setStyleSheet("border:0; font-size:23px; font-weight:700; color:#f3f7fb;")
        layout.addWidget(title)
        layout.addWidget(value)
        return frame, value

    def build_ui(self) -> None:
        root = QWidget()
        outer = QVBoxLayout(root)
        top = QHBoxLayout()
        title = QLabel("五串 BMS 管理监测")
        title.setStyleSheet("font-size:23px; font-weight:700;")
        self.connection = QLabel("未连接")
        self.connection.setStyleSheet("color:#ee8b8b;")
        self.source_selector = QComboBox()
        self.source_selector.addItem("FreeRTOS 仿真", "sim")
        self.source_selector.addItem("实板串口（只读）", "board")
        self.source_selector.currentIndexChanged.connect(self.select_source)
        self.port_selector = QComboBox()
        self.port_selector.setMinimumWidth(145)
        self.refresh_ports()
        self.port_selector.hide()
        self.refresh_button = QPushButton("刷新串口")
        self.refresh_button.clicked.connect(self.refresh_ports)
        self.refresh_button.hide()
        restart = QPushButton("重新连接")
        restart.clicked.connect(self.connect_selected)
        self.disconnect_button = QPushButton("断开")
        self.disconnect_button.clicked.connect(self.disconnect_selected)
        self.disconnect_button.hide()
        export = QPushButton("导出 CSV")
        export.clicked.connect(self.export_csv)
        top.addWidget(title)
        top.addStretch()
        top.addWidget(self.source_selector)
        top.addWidget(self.port_selector)
        top.addWidget(self.refresh_button)
        top.addWidget(self.connection)
        top.addWidget(restart)
        top.addWidget(self.disconnect_button)
        top.addWidget(export)
        outer.addLayout(top)

        metrics = QHBoxLayout()
        self.metric_labels = {}
        for key, name in [
            ("pack", "电池总压"), ("current", "实际电流"), ("soc", "SOC"),
            ("temp", "温度"), ("mode", "运行模式"), ("mos", "充 / 放 MOS"),
        ]:
            frame, label = self.create_metric_card(name)
            metrics.addWidget(frame)
            self.metric_labels[key] = label
        outer.addLayout(metrics)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        left = QWidget()
        left_layout = QVBoxLayout(left)
        splitter.addWidget(left)
        right = QWidget()
        right_layout = QVBoxLayout(right)
        splitter.addWidget(right)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 5)
        outer.addWidget(splitter, 1)

        cells_box = QGroupBox("五节电芯")
        cells_layout = QGridLayout(cells_box)
        self.cell_labels: list[QLabel] = []
        self.cell_bars: list[QProgressBar] = []
        self.balance_labels: list[QLabel] = []
        for i in range(5):
            name = QLabel(f"电芯 {i + 1}")
            bar = QProgressBar()
            bar.setRange(2500, 4400)
            bar.setValue(2500)
            bar.setFormat("%v mV")
            balance = QLabel("○")
            cells_layout.addWidget(name, i, 0)
            cells_layout.addWidget(bar, i, 1)
            cells_layout.addWidget(balance, i, 2)
            self.cell_labels.append(name)
            self.cell_bars.append(bar)
            self.balance_labels.append(balance)
        left_layout.addWidget(cells_box)

        status_box = QGroupBox("保护与采样")
        status_layout = QVBoxLayout(status_box)
        self.alarm_label = QLabel("无故障")
        self.alarm_label.setWordWrap(True)
        self.sample_label = QLabel("等待采样")
        status_layout.addWidget(self.alarm_label)
        status_layout.addWidget(self.sample_label)
        left_layout.addWidget(status_box)

        controls = QGroupBox("仿真输入与使能")
        self.sim_controls = []
        grid = QGridLayout(controls)
        self.current_spin = QSpinBox()
        self.current_spin.setRange(-5000, 5000)
        self.current_spin.setSingleStep(100)
        self.current_spin.setSuffix(" mA")
        current_button = QPushButton("设置电流")
        current_button.clicked.connect(lambda: self.send("set_current", ma=self.current_spin.value()))
        self.temp_spin = QSpinBox()
        self.temp_spin.setRange(-40, 100)
        self.temp_spin.setValue(25)
        self.temp_spin.setSuffix(" °C")
        temp_button = QPushButton("设置温度")
        temp_button.clicked.connect(lambda: self.send("set_temp", c=self.temp_spin.value()))
        self.cell_index = QSpinBox()
        self.cell_index.setRange(1, 5)
        self.cell_mv = QSpinBox()
        self.cell_mv.setRange(2500, 4400)
        self.cell_mv.setValue(3800)
        self.cell_mv.setSuffix(" mV")
        cell_set = QPushButton("覆写电压")
        cell_set.clicked.connect(lambda: self.send("set_cell", index=self.cell_index.value(), mv=self.cell_mv.value()))
        cell_clear = QPushButton("清除覆写")
        cell_clear.clicked.connect(lambda: self.send("set_cell", index=self.cell_index.value(), mv=None))
        grid.addWidget(QLabel("充放电请求：正数充电，负数放电"), 0, 0, 1, 3)
        grid.addWidget(self.current_spin, 1, 0, 1, 2)
        grid.addWidget(current_button, 1, 2)
        grid.addWidget(self.temp_spin, 2, 0, 1, 2)
        grid.addWidget(temp_button, 2, 2)
        grid.addWidget(self.cell_index, 3, 0)
        grid.addWidget(self.cell_mv, 3, 1)
        grid.addWidget(cell_set, 3, 2)
        grid.addWidget(cell_clear, 4, 2)
        self.charge_check = QCheckBox("允许充电")
        self.discharge_check = QCheckBox("允许放电")
        self.balance_check = QCheckBox("允许均衡")
        for i, check in enumerate([self.charge_check, self.discharge_check, self.balance_check]):
            check.stateChanged.connect(self.send_enables)
            grid.addWidget(check, 5, i)
        left_layout.addWidget(controls)
        self.sim_controls.append(controls)

        fault_box = QGroupBox("故障注入")
        fault_layout = QHBoxLayout(fault_box)
        self.fault_checks = {}
        for kind, label in [("scd", "短路事件"), ("nack", "I²C NACK"), ("crc", "CRC 错误")]:
            check = QCheckBox(label)
            check.toggled.connect(lambda on, k=kind: self.send("inject_fault", kind=k, on=on))
            fault_layout.addWidget(check)
            self.fault_checks[kind] = check
        left_layout.addWidget(fault_box)
        self.sim_controls.append(fault_box)

        presets = QGroupBox("典型场景")
        preset_grid = QGridLayout(presets)
        names = [
            ("正常充电", "charge"), ("正常放电", "discharge"),
            ("过压", "ov"), ("欠压", "uv"),
            ("充电过流", "occ"), ("放电过流", "ocd"),
            ("充电过温", "hot"), ("充电低温", "cold"),
            ("非相邻均衡", "balance"), ("复位", "reset"),
        ]
        for i, (name, key) in enumerate(names):
            button = QPushButton(name)
            button.clicked.connect(lambda checked=False, k=key: self.preset(k))
            preset_grid.addWidget(button, i // 2, i % 2)
        left_layout.addWidget(presets)
        self.sim_controls.append(presets)
        left_layout.addStretch()

        tabs = QTabWidget()
        self.pack_chart = TrendChart("电池总压", "V", "#49b9d3")
        self.current_chart = TrendChart("实际电流", "A", "#efae61")
        self.soc_chart = TrendChart("SOC", "%", "#7fca9c")
        tabs.addTab(self.pack_chart, "电压")
        tabs.addTab(self.current_chart, "电流")
        tabs.addTab(self.soc_chart, "SOC")
        right_layout.addWidget(tabs, 3)
        event_box = QGroupBox("事件记录")
        event_layout = QVBoxLayout(event_box)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.document().setMaximumBlockCount(500)
        event_layout.addWidget(self.log)
        right_layout.addWidget(event_box, 2)
        self.setCentralWidget(root)

    def append_log(self, text: str) -> None:
        self.log.appendPlainText(text)

    def refresh_ports(self) -> None:
        if not hasattr(self, "port_selector"):
            return
        previous = self.port_selector.currentData()
        self.port_selector.clear()
        for info in QSerialPortInfo.availablePorts():
            self.port_selector.addItem(f"{info.portName()} · {info.description()}", info.portName())
        if previous:
            index = self.port_selector.findData(previous)
            if index >= 0:
                self.port_selector.setCurrentIndex(index)

    def select_source(self) -> None:
        new_source = self.source_selector.currentData()
        if new_source == self.source:
            return
        self.disconnect_selected()
        self.source = new_source
        is_board = new_source == "board"
        self.port_selector.setVisible(is_board)
        self.refresh_button.setVisible(is_board)
        self.disconnect_button.setVisible(is_board)
        for group in self.sim_controls:
            group.setVisible(not is_board)
        self.clear_live_data()
        self.append_log("已切换至实板串口只读监测" if is_board else "已切换至 FreeRTOS 仿真")
        if not is_board:
            self.start_process()

    def connect_selected(self) -> None:
        if self.source == "board":
            self.start_serial()
        else:
            self.start_process()

    def disconnect_selected(self) -> None:
        if self.serial.isOpen():
            self.serial.close()
        self.serial_last_frame = None
        self.serial_parser.reset()
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.process.kill()
            self.process.waitForFinished(1000)
        self.mark_stale("已断开")

    def clear_live_data(self) -> None:
        self.rows.clear()
        self.last_snapshot = None
        for label in self.metric_labels.values():
            label.setText("—")
        for bar in self.cell_bars:
            bar.setValue(2500)
            bar.setFormat("—")
        for label in self.balance_labels:
            label.setText("○")
        self.alarm_label.setText("等待状态")
        self.sample_label.setText("等待采样")
        for chart in (self.pack_chart, self.current_chart, self.soc_chart):
            chart.series.clear()

    def start_serial(self) -> None:
        if self.source != "board":
            return
        self.disconnect_selected()
        self.clear_live_data()
        port_name = self.port_selector.currentData()
        if not port_name:
            self.connection.setText("无可用串口")
            self.append_log("未发现串口，请连接 USB 转串口适配器后刷新")
            return
        self.serial.setPortName(port_name)
        self.serial.setBaudRate(115200)
        self.serial.setDataBits(QSerialPort.DataBits.Data8)
        self.serial.setParity(QSerialPort.Parity.NoParity)
        self.serial.setStopBits(QSerialPort.StopBits.OneStop)
        self.serial.setFlowControl(QSerialPort.FlowControl.NoFlowControl)
        if not self.serial.open(QIODevice.OpenModeFlag.ReadOnly):
            self.connection.setText("串口打开失败")
            self.append_log(f"{port_name} 打开失败：{self.serial.errorString()}")
            return
        self.serial_last_frame = time.monotonic()
        self.connection.setText(f"{port_name} 已打开，等待遥测")
        self.connection.setStyleSheet("color:#e9bf75;")
        self.append_log(f"已打开 {port_name}，115200 8N1，只读")

    def serial_error(self, error: QSerialPort.SerialPortError) -> None:
        if error == QSerialPort.SerialPortError.NoError or self.source != "board":
            return
        if error in (QSerialPort.SerialPortError.ResourceError, QSerialPort.SerialPortError.DeviceNotFoundError):
            self.append_log(f"串口已断开：{self.serial.errorString()}")
            self.serial.close()
            self.serial_last_frame = None
            self.mark_stale("串口已断开")
        elif error not in (QSerialPort.SerialPortError.NotOpenError,):
            self.append_log(f"串口错误：{self.serial.errorString()}")

    def read_serial(self) -> None:
        self.consume_serial_bytes(bytes(self.serial.readAll()))

    def consume_serial_bytes(self, data: bytes) -> None:
        for snapshot in self.serial_parser.feed(data):
            prior = self.last_snapshot
            self.serial_last_frame = time.monotonic()
            snapshot["received_at"] = datetime.now().astimezone().isoformat(timespec="milliseconds")
            self.show_snapshot(snapshot)
            if prior and prior.get("source") == "board":
                for key, label in (("faults", "故障位"), ("balance_mask", "均衡位"),
                                   ("mode", "模式"), ("charge_on", "充电 MOS"),
                                   ("discharge_on", "放电 MOS")):
                    if prior.get(key) != snapshot.get(key):
                        self.append_log(f"实板 {label}：{prior.get(key)} → {snapshot.get(key)}")
        for issue in self.serial_parser.errors:
            self.append_log(f"串口帧错误：{issue}")
        self.serial_parser.errors.clear()

    def check_serial_timeout(self) -> None:
        if self.source == "board" and self.serial.isOpen() and self.serial_last_frame is not None:
            if time.monotonic() - self.serial_last_frame > 1.0 and self.connection.text() != "数据过期":
                self.mark_stale("数据过期")
                self.append_log("超过 1 秒没有收到有效遥测帧")

    def mark_stale(self, reason: str) -> None:
        self.connection.setText(reason)
        self.connection.setStyleSheet("color:#ee8b8b;")
        for label in self.metric_labels.values():
            label.setText("—")
        for bar in self.cell_bars:
            bar.setFormat("—")
        for label in self.balance_labels:
            label.setText("—")
        self.alarm_label.setText("状态未知")
        self.sample_label.setText(reason)

    def start_process(self) -> None:
        if self.source != "sim":
            return
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.process.kill()
            self.process.waitForFinished(1000)
        self.buffer.clear()
        self.clear_live_data()
        self._setting_controls = True
        self.charge_check.setChecked(False)
        self.discharge_check.setChecked(False)
        self.balance_check.setChecked(False)
        self._setting_controls = False
        self.current_spin.setValue(0)
        self.temp_spin.setValue(25)
        for check in self.fault_checks.values():
            check.blockSignals(True)
            check.setChecked(False)
            check.blockSignals(False)
        self.connection.setText("连接中…")
        self.connection.setStyleSheet("color:#e9bf75;")
        if not self.sim_path.is_file():
            self.append_log(f"找不到仿真程序：{self.sim_path}")
            self.connection.setText("未连接")
            return
        self.process.start(str(self.sim_path), [])

    def process_started(self) -> None:
        if self.source != "sim":
            return
        self.connection.setText("已启动，等待数据")
        self.append_log("FreeRTOS 仿真程序已启动")

    def process_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        if self.source != "sim":
            return
        self.mark_stale("已断开")
        self.append_log(f"仿真程序退出：代码 {code}，状态 {status.name}")

    def process_error(self, error: QProcess.ProcessError) -> None:
        if self.source != "sim":
            return
        self.append_log(f"仿真程序错误：{error.name} {self.process.errorString()}")

    def read_stderr(self) -> None:
        message = bytes(self.process.readAllStandardError()).decode("utf-8", errors="replace").strip()
        if message and self.source == "sim":
            self.append_log(message)

    def read_stdout(self) -> None:
        if self.source != "sim":
            self.process.readAllStandardOutput()
            return
        self.buffer.extend(bytes(self.process.readAllStandardOutput()))
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            try:
                packet = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                self.append_log("收到无法解析的数据帧")
                continue
            if packet.get("v") != 1:
                self.append_log("协议版本不匹配")
                continue
            kind = packet.get("type")
            if kind == "snapshot":
                self.show_snapshot(packet)
            elif kind == "ack":
                if not packet.get("ok"):
                    self.append_log(f"命令 {packet.get('id')} 被拒绝：{packet.get('error')}")
            elif kind == "event":
                self.append_log(f"[{packet.get('ms', 0) / 1000:.1f}s] {self.event_text(packet)}")

    @staticmethod
    def event_text(packet: dict) -> str:
        name, value = packet.get("name"), int(packet.get("value", 0))
        if name == "faults":
            names = [label for i, label in enumerate(FAULT_NAMES) if value & (1 << i)]
            return "保护状态：" + ("、".join(names) if names else "已解除")
        if name == "balance":
            cells = [str(i + 1) for i in range(5) if value & (1 << i)]
            return "均衡电芯：" + ("、".join(cells) if cells else "已停止")
        if name == "mode":
            return "模式：" + {0: "待机", 1: "充电", 2: "放电", 3: "休眠"}.get(value, str(value))
        if name == "mos":
            return f"MOS：充电{'开' if value & 1 else '关'}，放电{'开' if value & 2 else '关'}"
        if name == "reset":
            return "仿真状态已复位"
        return f"{name}：{value}"

    def send(self, command_type: str, **values: object) -> None:
        if self.source != "sim":
            self.append_log("实板串口为只读模式，命令未发送")
            return
        if self.process.state() != QProcess.ProcessState.Running:
            self.append_log("仿真程序未连接，命令未发送")
            return
        packet = {"v": 1, "id": self.next_id, "type": command_type, **values}
        self.next_id += 1
        self.process.write((json.dumps(packet, ensure_ascii=True, separators=(",", ":")) + "\n").encode("ascii"))

    def send_enables(self) -> None:
        if self._setting_controls:
            return
        self.send("set_enable", charge=self.charge_check.isChecked(),
                  discharge=self.discharge_check.isChecked(), balance=self.balance_check.isChecked())

    def set_checks(self, charge: bool, discharge: bool, balance: bool) -> None:
        self._setting_controls = True
        self.charge_check.setChecked(charge)
        self.discharge_check.setChecked(discharge)
        self.balance_check.setChecked(balance)
        self._setting_controls = False
        self.send_enables()

    def preset(self, name: str) -> None:
        self.send("reset")
        for check in self.fault_checks.values():
            check.blockSignals(True)
            check.setChecked(False)
            check.blockSignals(False)
        if name == "reset":
            self.current_spin.setValue(0)
            self.temp_spin.setValue(25)
            self.set_checks(False, False, False)
            self.append_log("已请求复位")
            return
        self.set_checks(True, True, name == "balance")
        values = {"charge": 1000, "discharge": -1000, "ov": 1000, "uv": -1000,
                  "occ": 3000, "ocd": -3000, "hot": 1000, "cold": 1000, "balance": 0}
        self.current_spin.setValue(values[name])
        self.send("set_current", ma=values[name])
        if name == "ov":
            self.send("set_cell", index=1, mv=4250)
        elif name == "uv":
            self.send("set_cell", index=1, mv=3000)
        elif name == "hot":
            self.temp_spin.setValue(75)
            self.send("set_temp", c=75)
        elif name == "cold":
            self.temp_spin.setValue(-25)
            self.send("set_temp", c=-25)
        elif name == "balance":
            for index, mv in enumerate([3800, 3700, 3780, 3710, 3760], 1):
                self.send("set_cell", index=index, mv=mv)
        self.append_log(f"已启动场景：{name}")

    def show_snapshot(self, s: dict) -> None:
        if "source" not in s:
            s = {**s, "source": "sim"}
        s.setdefault("received_at", datetime.now().astimezone().isoformat(timespec="milliseconds"))
        self.last_snapshot = s
        is_board = s["source"] == "board"
        valid_mask = s.get("valid_mask", 0x3F if s.get("valid") else 0)
        available = lambda bit: (not is_board and s.get("valid", False)) or (is_board and bool(valid_mask & (1 << bit)))
        self.connection.setText("已连接 · 实板遥测" if is_board else "已连接 · FreeRTOS 运行中")
        self.connection.setStyleSheet("color:#7bd7ad;")
        self.metric_labels["pack"].setText(f"{s['pack_mv'] / 1000:.3f} V" if available(1) else "—")
        self.metric_labels["current"].setText(f"{s['actual_ma'] / 1000:+.3f} A" if available(2) else "—")
        self.metric_labels["soc"].setText(f"{s['soc_pct']:.1f} %" if available(4) else "—")
        self.metric_labels["temp"].setText(f"{s['temp_c']:.1f} °C" if available(3) else "—")
        modes = {"STANDBY": "待机", "CHARGE": "充电", "DISCHARGE": "放电", "SLEEP": "休眠"}
        self.metric_labels["mode"].setText(modes.get(s["mode"], s["mode"]))
        self.metric_labels["mos"].setText(
            f"{'开' if s['charge_on'] else '关'} / {'开' if s['discharge_on'] else '关'}" if available(5) else "— / —")
        for i, mv in enumerate(s["cell_mv"]):
            if available(0):
                self.cell_bars[i].setValue(max(2500, min(4400, mv)))
            self.cell_bars[i].setFormat(f"{mv} mV" if available(0) else "—")
            balance_known = not is_board or bool(valid_mask & (1 << 6))
            on = bool(s["balance_mask"] & (1 << i)) and balance_known
            self.balance_labels[i].setText("● 均衡" if on else "○" if balance_known else "—")
            self.balance_labels[i].setStyleSheet("color:#7bd7ad;" if on else "color:#7d8da0;")
        active = [name for i, name in enumerate(FAULT_NAMES) if s["faults"] & (1 << i)]
        self.alarm_label.setText("、".join(active) if active else "无故障")
        self.alarm_label.setStyleSheet("color:#ff9b8f;" if active else "color:#7bd7ad;")
        if is_board:
            names = ("电芯", "总压", "电流", "温度", "SOC")
            ages = s["ages_ms"]
            self.sample_label.setText("采样年龄：" + " · ".join(
                f"{name} {ages[i]} ms" if available(i) else f"{name} 无效"
                for i, name in enumerate(names)))
        else:
            self.sample_label.setText("采样有效" if s["valid"] else f"采样无效：{s['error']}")
        self.rows.append(s)
        recent = list(self.rows)[-300:]
        self.pack_chart.update_points([(r["ms"] / 1000, r["pack_mv"] / 1000) for r in recent
                                       if r.get("valid_mask", 0x3F if r.get("valid") else 0) & 2])
        self.current_chart.update_points([(r["ms"] / 1000, r["actual_ma"] / 1000) for r in recent
                                          if r.get("valid_mask", 0x3F if r.get("valid") else 0) & 4])
        self.soc_chart.update_points([(r["ms"] / 1000, r["soc_pct"]) for r in recent
                                      if r.get("valid_mask", 0x3F if r.get("valid") else 0) & 16])

    def export_csv(self) -> None:
        if not self.rows:
            QMessageBox.information(self, "暂无数据", "收到遥测数据后才能导出。")
            return
        path, _ = QFileDialog.getSaveFileName(self, "导出遥测数据", "bms_telemetry.csv", "CSV (*.csv)")
        if not path:
            return
        self.save_csv(Path(path))

    def save_csv(self, path: Path) -> None:
        keys = ["source", "received_at", "seq", "ms", "pack_mv", "requested_ma", "actual_ma", "temp_c", "soc_pct",
                "mode", "faults", "balance_mask", "charge_on", "discharge_on", "valid", "error"]
        with open(path, "w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.writer(stream)
            writer.writerow(keys + ["valid_mask"] + [f"age_{name}_ms" for name in
                                               ("cells", "pack", "current", "temp", "soc")] +
                            [f"cell{i}_mv" for i in range(1, 6)])
            for row in self.rows:
                writer.writerow([row.get(k) for k in keys] + [row.get("valid_mask")] +
                                list(row.get("ages_ms", [None] * 5)) + row["cell_mv"])
        self.append_log(f"已导出 {len(self.rows)} 条记录：{path}")

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt override
        if self.serial.isOpen():
            self.serial.close()
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.process.kill()
            self.process.waitForFinished(1000)
        super().closeEvent(event)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true", help="Exit after two seconds for UI smoke testing")
    parser.add_argument("--screenshot", type=Path, help="Save a screenshot during smoke testing")
    args = parser.parse_args()
    app = QApplication(sys.argv)
    app.setFont(QFont("Microsoft YaHei", 10))
    window = BmsWindow(args.sim.resolve())
    window.show()
    if args.smoke:
        def finish_smoke() -> None:
            ok = window.last_snapshot is not None and window.process.state() == QProcess.ProcessState.Running
            if args.screenshot:
                window.grab().save(str(args.screenshot))
            window.close()
            app.exit(0 if ok else 1)

        QTimer.singleShot(2000, finish_smoke)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
