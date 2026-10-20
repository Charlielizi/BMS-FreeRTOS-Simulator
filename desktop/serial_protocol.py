"""Read-only, versioned telemetry protocol for the STM32 board.

All multibyte values are little endian. CRC-16/CCITT-FALSE covers the bytes
from version through the last payload byte (the sync word is excluded).
"""

from __future__ import annotations

import struct

SYNC = b"\xa5\x5a"
VERSION = 1
TYPE_SNAPSHOT = 1
PAYLOAD = struct.Struct("<I5HHhhHBHBBBH5H")
# uptime, 5 cells, pack, current, temperature (0.1 C), SOC (0.1%),
# mode, faults, balance, MOS, validity, reserved, five sample ages (ms).
MAX_PAYLOAD = 64
FRAME_SIZE = 7 + PAYLOAD.size + 2


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_snapshot(*, seq: int, ms: int, cell_mv: list[int], pack_mv: int,
                  actual_ma: int, temp_c10: int, soc_pct10: int, mode: int,
                  faults: int = 0, balance_mask: int = 0, mos_bits: int = 0,
                  valid_mask: int = 0x7F, ages_ms: tuple[int, ...] = (0, 0, 0, 0, 0)) -> bytes:
    """Encode a fixture; the embedded firmware has the matching C encoder."""
    if len(cell_mv) != 5 or len(ages_ms) != 5:
        raise ValueError("five cells and five sample ages required")
    payload = PAYLOAD.pack(ms, *cell_mv, pack_mv, actual_ma, temp_c10,
                           soc_pct10, mode, faults, balance_mask, mos_bits,
                           valid_mask, 0, *ages_ms)
    body = struct.pack("<BBBH", VERSION, TYPE_SNAPSHOT, len(payload), seq) + payload
    return SYNC + body + struct.pack("<H", crc16(body))


class SerialFrameParser:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.errors: list[str] = []
        self.last_seq: int | None = None

    def reset(self) -> None:
        self.buffer.clear()
        self.errors.clear()
        self.last_seq = None

    def feed(self, data: bytes) -> list[dict]:
        self.buffer.extend(data)
        snapshots: list[dict] = []
        while len(self.buffer) >= 7:
            start = self.buffer.find(SYNC)
            if start < 0:
                del self.buffer[:-1]
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 7:
                break
            version, kind, length, seq = struct.unpack_from("<BBBH", self.buffer, 2)
            if length > MAX_PAYLOAD or length != PAYLOAD.size or kind != TYPE_SNAPSHOT or version != VERSION:
                self.errors.append("协议版本或帧格式不匹配")
                del self.buffer[0]
                continue
            total = 7 + length + 2
            if len(self.buffer) < total:
                break
            body = bytes(self.buffer[2:7 + length])
            expected = struct.unpack_from("<H", self.buffer, 7 + length)[0]
            if crc16(body) != expected:
                self.errors.append("CRC 校验失败")
                del self.buffer[0]
                continue
            fields = PAYLOAD.unpack_from(self.buffer, 7)
            if self.last_seq is not None and seq != ((self.last_seq + 1) & 0xFFFF):
                self.errors.append(f"序号跳变：{self.last_seq} → {seq}")
            self.last_seq = seq
            snapshots.append(self._decode(seq, fields))
            del self.buffer[:total]
        if len(self.buffer) > 2 * (MAX_PAYLOAD + 9):
            self.errors.append("接收缓存溢出，已重同步")
            del self.buffer[:-2]
        return snapshots

    @staticmethod
    def _decode(seq: int, f: tuple) -> dict:
        ms = f[0]
        cells = list(f[1:6])
        pack_mv, actual_ma, temp_c10, soc_pct10 = f[6:10]
        mode, faults, balance, mos, valid_mask, _reserved = f[10:16]
        ages = list(f[16:21])
        return {
            "v": 1, "type": "snapshot", "source": "board", "seq": seq, "ms": ms,
            "cell_mv": cells, "pack_mv": pack_mv, "requested_ma": None,
            "actual_ma": actual_ma, "temp_c": temp_c10 / 10,
            "soc_pct": soc_pct10 / 10,
            "mode": {0: "STANDBY", 1: "CHARGE", 2: "DISCHARGE", 3: "SLEEP"}.get(mode, "UNKNOWN"),
            "faults": faults, "balance_mask": balance,
            "charge_on": bool(mos & 1), "discharge_on": bool(mos & 2),
            "charge_enabled": None, "discharge_enabled": None, "balance_enabled": None,
            "valid_mask": valid_mask, "ages_ms": ages,
            "valid": (valid_mask & 0x03) == 0x03,
            "error": None if (valid_mask & 0x03) == 0x03 else "采集无效",
        }
