"""SolarLink: ESP32 串口链路 (仅负责收发JSON, 无渲染/无算法)

协议:
  {"cmd":"init","ts":<utc>,"lat":..,"lon":..,"tz":..} -> ACK(完整JSON) + 100ms心跳
  心跳: {"t":"HH:MM:SS","d":"MM-DD","s":"HH:MM","r":"HH:MM","st":"HH:MM",
         "ts":<min>,"dp":<0-100>,"ev":<0|1|2>,"p":<0|1|-1>}
  {"cmd":"stop"} -> {"ok":"stopped"}
"""
import json
import serial


class SolarLink:
    def __init__(self, port="/dev/ttyACM0", baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)

    def _send(self, obj):
        self.ser.write((json.dumps(obj, separators=(",", ":")) + "\n").encode())

    def _read_json(self, timeout=3.0):
        import time
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            b = self.ser.read(1)
            if not b:
                continue
            if b == b"\n":
                line = buf.decode(errors="replace").strip()
                buf = b""
                if line.startswith("{"):
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError:
                        continue
            else:
                buf += b
        return None

    def init(self, ts, lat, lon, tz):
        """发送init, 等待ACK(含noon字段的完整应答), 返回ACK dict"""
        self._send({"cmd": "init", "ts": int(ts), "lat": lat, "lon": lon, "tz": tz})
        while True:
            r = self._read_json(timeout=3.0)
            if r is None:
                return None
            if "noon" in r:  # ACK特征字段(心跳没有noon)
                return r

    def get_cfg(self, timeout=2.0):
        """读取ESP32当前坐标配置"""
        self._send({"cmd": "get"})
        while True:
            r = self._read_json(timeout=timeout)
            if r is None:
                return None
            if "lat" in r and "lon" in r:
                return r

    def heartbeat(self, timeout=1.0):
        """阻塞读下一个心跳(含dp字段), 超时返回None"""
        while True:
            r = self._read_json(timeout=timeout)
            if r is None:
                return None
            if "dp" in r:
                return r

    def read_event(self, timeout=1.0):
        """读任意JSON事件: 心跳(dp) / 搜星状态(gnss) / 其他; 超时返回None"""
        return self._read_json(timeout=timeout)

    def stop(self):
        try:
            self._send({"cmd": "stop"})
        except Exception:
            pass

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass
