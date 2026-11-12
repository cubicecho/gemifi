"""MIDI port access for gemifi.py: python-rtmidi, then mido, then (Linux) raw ALSA /dev/snd/midiC*D*.
A serial port (raw MIDI bytes over a USB-UART, e.g. a classic ESP32 built with GM_MIDI_SERIAL) is
one more backend, chosen with --serial; it needs pyserial.

Every backend exposes the same small interface:
  backend.ports() -> [name, ...]           ports that have both an input and an output
  backend.open(name) -> Port               Port.send(bytes), Port.recv(timeout) -> [complete SysEx], Port.close()
A backend with `fixed = True` (serial) has exactly one port, whatever its name; the tool doesn't match names on it.
"""
import glob
import os
import re
import select
import time

from gemifi_proto import SysexAssembler


class MidiError(RuntimeError):
    pass


class _PortBase:
    name = ""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ---------------- python-rtmidi ----------------

class RtMidiBackend:
    label = "rtmidi"

    def __init__(self):
        import rtmidi  # noqa: F401  (ImportError selects another backend)
        self.rtmidi = rtmidi

    def _names(self):
        mi, mo = self.rtmidi.MidiIn(), self.rtmidi.MidiOut()
        try:
            return mi.get_ports(), mo.get_ports()
        finally:
            mi.delete() if hasattr(mi, "delete") else None
            mo.delete() if hasattr(mo, "delete") else None

    @staticmethod
    def _key(n):
        # ALSA client:port numbers differ between runs; match on the name without them.
        return re.sub(r"\s+\d+:\d+$", "", n)

    def ports(self):
        ins, outs = self._names()
        okeys = {self._key(o) for o in outs}
        return [self._key(i) for i in ins if self._key(i) in okeys]

    def open(self, name):
        ins, outs = self._names()
        try:
            ii = next(i for i, n in enumerate(ins) if self._key(n) == name)
            oi = next(i for i, n in enumerate(outs) if self._key(n) == name)
        except StopIteration:
            raise MidiError(f"port {name!r} not found") from None
        return _RtPort(self.rtmidi, ii, oi, name)


class _RtPort(_PortBase):
    def __init__(self, rtmidi, ii, oi, name):
        self.name = name
        self.mi = rtmidi.MidiIn()
        self.mi.ignore_types(sysex=False, timing=True, active_sense=True)
        self.mi.open_port(ii)
        self.mo = rtmidi.MidiOut()
        self.mo.open_port(oi)
        self.asm = SysexAssembler()

    def send(self, msg):
        self.mo.send_message(list(msg))

    def recv(self, timeout):
        deadline = time.monotonic() + max(timeout, 0)
        out = []
        while True:
            m = self.mi.get_message()
            while m:
                out += self.asm.feed(m[0])
                m = self.mi.get_message()
            if out or time.monotonic() >= deadline:
                return out
            time.sleep(0.002)

    def close(self):
        for p in (self.mi, self.mo):
            try:
                p.close_port()
            except Exception:
                pass


# ---------------- mido ----------------

class MidoBackend:
    label = "mido"

    def __init__(self):
        import mido
        mido.get_input_names()  # raises if mido has no working backend
        self.mido = mido

    def ports(self):
        outs = set(self.mido.get_output_names())
        return [n for n in self.mido.get_input_names() if n in outs]

    def open(self, name):
        return _MidoPort(self.mido, name)


class _MidoPort(_PortBase):
    def __init__(self, mido, name):
        self.mido, self.name = mido, name
        self.i = mido.open_input(name)
        self.o = mido.open_output(name)

    def send(self, msg):
        self.o.send(self.mido.Message("sysex", data=list(msg[1:-1])))

    def recv(self, timeout):
        deadline = time.monotonic() + max(timeout, 0)
        while True:
            out = [bytes([0xF0, *m.data, 0xF7]) for m in self.i.iter_pending() if m.type == "sysex"]
            if out or time.monotonic() >= deadline:
                return out
            time.sleep(0.002)

    def close(self):
        self.i.close()
        self.o.close()


# ---------------- raw ALSA (Linux, no dependencies) ----------------

def _alsa_card_names():
    names = {}
    try:
        with open("/proc/asound/cards") as f:
            for line in f:
                m = re.match(r"\s*(\d+)\s+\[[^\]]*\]:\s*\S+\s+-\s+(.*)", line)
                if m:
                    names[int(m.group(1))] = m.group(2).strip()
    except OSError:
        pass
    return names


class AlsaRawBackend:
    """Opens /dev/snd/midiC<card>D<dev> directly. Names come from /proc/asound/card<N>/midi<D>
    (first line) or /proc/asound/cards. Same as what `amidi -l` shows."""
    label = "alsa-raw"

    def __init__(self):
        if not os.path.isdir("/dev/snd"):
            raise MidiError("no /dev/snd (not Linux/ALSA)")

    def _devices(self):
        cards = _alsa_card_names()
        devs = []
        for path in sorted(glob.glob("/dev/snd/midiC*D*")):
            m = re.search(r"midiC(\d+)D(\d+)$", path)
            if not m:
                continue
            c, d = int(m.group(1)), int(m.group(2))
            name = None
            try:
                with open(f"/proc/asound/card{c}/midi{d}") as f:
                    name = f.readline().strip()
            except OSError:
                pass
            name = name or cards.get(c) or f"card{c}"
            devs.append((f"{name} [hw:{c},{d}]", path))
        return devs

    def ports(self):
        return [n for n, _ in self._devices()]

    def open(self, name):
        for n, path in self._devices():
            if n == name:
                return _AlsaRawPort(path, name)
        raise MidiError(f"port {name!r} not found")


class _AlsaRawPort(_PortBase):
    def __init__(self, path, name):
        self.name = name
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except OSError as e:
            raise MidiError(f"can't open {path}: {e} (another program using it? in the 'audio' group?)") from e
        self.asm = SysexAssembler()

    def send(self, msg):
        data = bytes(msg)
        while data:
            select.select([], [self.fd], [], 1.0)
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[n:]

    def recv(self, timeout):
        deadline = time.monotonic() + max(timeout, 0)
        out = []
        while True:
            left = deadline - time.monotonic()
            r, _, _ = select.select([self.fd], [], [], max(left, 0))
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    chunk = b""
                except OSError as e:
                    raise MidiError(f"read failed: {e} (device unplugged?)") from e
                out += self.asm.feed(chunk)
            if out or time.monotonic() >= deadline:
                return out

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


# ---------------- serial (raw MIDI byte stream over a UART, pyserial) ----------------

class SerialBackend:
    """One serial device, e.g. /dev/ttyUSB0 at 115200 baud. The bytes on the wire are plain MIDI (the same
    SysEx frames as over USB-MIDI), so SysEx is cut out of the stream; anything else (an ESP boot log, other
    MIDI) is dropped. DTR and RTS are released after opening (RTS first), so opening the port doesn't reset
    an ESP dev board or put it in its ROM bootloader."""
    label = "serial"
    fixed = True

    def __init__(self, path, baud=115200, serial_module=None):
        if serial_module is None:
            try:
                import serial as serial_module
            except ImportError:
                raise MidiError("--serial needs pyserial (pip install pyserial)") from None
        self.serial, self.path, self.baud = serial_module, path, baud

    def ports(self):
        return [self.path] if self.path.startswith(("socket://", "loop://", "rfc2217://")) or \
            os.path.exists(self.path) else []

    def open(self, name=None):
        try:
            ser = self.serial.Serial()
            ser.port, ser.baudrate, ser.timeout, ser.write_timeout = self.path, self.baud, 0, 2
            ser.open()
            # The OS asserts both lines on open, which the usual two-transistor auto-reset circuit
            # ignores. Release RTS (EN) first: DTR alone only pulls IO0, which matters at reset only.
            # The other order would pulse EN low and reset the chip.
            ser.rts = False
            ser.dtr = False
        except Exception as e:
            raise MidiError(f"can't open {self.path}: {e}") from e
        return SerialPort(ser, self.path)


class SerialPort(_PortBase):
    """A Port over any pyserial-like object (write, read(n), in_waiting, close). Tests pass a fake."""

    def __init__(self, ser, name="serial"):
        self.ser, self.name = ser, name
        self.asm = SysexAssembler()

    def send(self, msg):
        try:
            self.ser.write(bytes(msg))
            if hasattr(self.ser, "flush"):
                self.ser.flush()
        except Exception as e:
            raise MidiError(f"{self.name}: write failed: {e} (device unplugged?)") from e

    def recv(self, timeout):
        deadline = time.monotonic() + max(timeout, 0)
        out = []
        while True:
            try:
                n = self.ser.in_waiting
                chunk = self.ser.read(n) if n else b""
            except Exception as e:
                raise MidiError(f"{self.name}: read failed: {e} (device unplugged?)") from e
            if chunk:
                out += self.asm.feed(chunk)
                continue        # drain what's there before returning
            if out or time.monotonic() >= deadline:
                return out
            time.sleep(0.001)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


BACKENDS = {"rtmidi": RtMidiBackend, "mido": MidoBackend, "alsa": AlsaRawBackend}


def get_backend(which="auto"):
    if which != "auto":
        return BACKENDS[which]()
    errors = []
    for cls in (RtMidiBackend, MidoBackend, AlsaRawBackend):
        try:
            return cls()
        except Exception as e:  # ImportError, missing backend, not Linux
            errors.append(f"{cls.label}: {e}")
    raise MidiError("no MIDI backend available (" + "; ".join(errors) + ")")
