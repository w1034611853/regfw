#!/usr/bin/env python3
"""Exercise the real cdylib: prefixed rows, native words, tails and batch locking."""
import ctypes as c
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading


def expected_hex(data, unit):
    groups = []
    for start in range(0, 16, unit):
        part = data[start:start + unit]
        if not part:
            groups.append("")
        elif len(part) == unit:
            groups.append(f"{int.from_bytes(part, sys.byteorder):0{unit * 2}x}")
        else:
            pieces = [f"{byte:02x}" for byte in part] + ["??"] * (unit - len(part))
            groups.append("".join(reversed(pieces) if sys.byteorder == "little" else pieces))
    return [value for value in groups if value]


def child(library):
    lib = c.CDLL(library)
    lib.hlog_register_logger.argtypes = [c.c_char_p, c.c_char_p, c.c_uint32]
    lib.hlog_register_logger.restype = c.c_void_p
    lib.hlog_logger_set_level.argtypes = [c.c_void_p, c.c_uint32]
    lib.hlog_set_console_enabled.argtypes = [c.c_bool]
    base = [c.c_void_p, c.c_uint32, c.c_char_p, c.c_size_t,
            c.c_char_p, c.c_size_t, c.c_uint32, c.c_void_p, c.c_size_t]
    for name in ["hlog_hexdump", "hlog_console_hexdump"]:
        getattr(lib, name).argtypes = base
    for name in ["hlog_hexdump_ex", "hlog_console_hexdump_ex"]:
        getattr(lib, name).argtypes = base + [c.c_uint32]
    assert lib.hlog_set_console_enabled(True) == 0
    metadata = []

    def emit(name, handle, data, unit=1, function=b"dump_fn", unaligned=False):
        storage = c.create_string_buffer((b"\xff" if unaligned else b"") + data)
        pointer = c.addressof(storage) + int(unaligned)
        args = [handle, 1, function, len(function), b"test.py", 7, 1, pointer, len(data)]
        if name.endswith("_ex"):
            args.append(unit)
        return getattr(lib, name)(*args)

    for unit in [1, 2, 4, 8]:
        for size in range(36):
            name = f"u{unit * 8}-{size}"
            logger = lib.hlog_register_logger(b"hexdump", name.encode(), 0)
            assert logger
            data = bytes((i + 32) % 256 for i in range(size))
            assert emit("hlog_hexdump_ex", logger, data, unit, unaligned=True) == 0
            metadata.append([name, unit, data.hex(), "mirror"])
    legacy = lib.hlog_register_logger(b"hexdump", b"legacy", 0)
    data = bytes(range(35))
    assert emit("hlog_hexdump", legacy, data) == 0
    metadata.append(["legacy", 1, data.hex(), "mirror"])

    invalid = lib.hlog_register_logger(b"hexdump", b"invalid", 0)
    for unit in [0, 3, 16, 0xFFFFFFFF]:
        assert emit("hlog_hexdump_ex", invalid, b"test", unit) == -1
    assert lib.hlog_hexdump_ex(invalid, 1, b"dump_fn", 7, b"t", 1, 1, None, 1, 4) == -1

    direct = lib.hlog_register_logger(b"hexdump", b"direct", 6)
    data = b"Console dump with a partial word!"
    assert emit("hlog_console_hexdump_ex", direct, data, 8) == 0
    metadata.append(["direct", 8, data.hex(), "console"])
    assert emit("hlog_hexdump_ex", direct, b"FILTERED", 4) == 0

    # Each call is a three-row batch, even when host threads race.
    concurrent = lib.hlog_register_logger(b"hexdump", b"threaded", 0)
    failures = []
    def worker(index):
        try:
            for number in range(5):
                function = f"thread_{index}_{number}".encode()
                assert emit("hlog_hexdump_ex", concurrent, bytes(range(48)), 4, function) == 0
        except BaseException as error:
            failures.append(error)
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not failures

    assert lib.hlog_set_console_enabled(False) == 0
    file_only = lib.hlog_register_logger(b"hexdump", b"file-only", 0)
    data = bytes(range(35))
    assert emit("hlog_hexdump_ex", file_only, data, 2) == 0
    metadata.append(["file-only", 2, data.hex(), "file"])
    assert lib.hlog_flush() == 0
    lib.hlog_shutdown()
    print(json.dumps(metadata))


def verify(lines, name, unit, data):
    selected = [line for line in lines if f"[hexdump/{name}]" in line]
    chunks = [data[i:i + 16] for i in range(0, len(data), 16)] if data else [b""]
    assert len(selected) == len(chunks), (name, selected)
    prefixes = []
    for row, (line, chunk) in enumerate(zip(selected, chunks)):
        assert "[dump_fn] " in line, line
        prefix, payload = line.split("[dump_fn] ", 1)
        prefixes.append(prefix)
        assert "[tid=" in prefix and re.search(r"\[[0-9:.]+\]", prefix), prefix
        if not data:
            assert payload == "<empty>"
            continue
        assert payload.startswith(f"{row * 16:08x}  "), payload
        assert payload.index("|") == 60, payload
        assert payload[10:60].split() == expected_hex(chunk, unit), (name, payload)
        ascii_text = "".join(chr(byte) if 32 <= byte <= 126 else "." for byte in chunk)
        assert payload[61:] == ascii_text.ljust(16) + "|", payload
    # Date/time values may differ, but the complete prefix width must be stable.
    assert len({len(prefix) for prefix in prefixes}) == 1
    return selected


def main():
    library = str(Path(sys.argv[1]).resolve())
    if "--child" in sys.argv:
        child(library)
        return
    with tempfile.TemporaryDirectory(prefix="hlog-hexdump-") as directory:
        run = subprocess.run([sys.executable, str(Path(__file__).resolve()), library, "--child"],
                             cwd=directory, env=dict(os.environ, HLOG_CONSOLE_COLOR="never"),
                             capture_output=True, text=True, check=True)
        metadata = json.loads(run.stdout)
        file_lines = Path(directory, "logs/qemu_device.log").read_text().splitlines()
        console_lines = run.stderr.splitlines()
        for name, unit, encoded, destination in metadata:
            data = bytes.fromhex(encoded)
            file_rows = verify(file_lines, name, unit, data) if destination != "console" else []
            console_rows = verify(console_lines, name, unit, data) if destination != "file" else []
            if destination == "mirror":
                assert ["[HLOG] " + line for line in file_rows] == console_rows
            if destination == "console":
                assert not any(f"[hexdump/{name}]" in line for line in file_lines)
            if destination == "file":
                assert not any(f"[hexdump/{name}]" in line for line in console_lines)
        assert not any("[hexdump/invalid]" in line or "FILTERED" in line for line in file_lines + console_lines)
        for lines in [file_lines, console_lines]:
            threaded = [line for line in lines if "[hexdump/threaded]" in line]
            assert len(threaded) == 60
            for at in range(0, len(threaded), 3):
                group = threaded[at:at + 3]
                assert len({re.search(r"\[(thread_\d+_\d+)\]", line).group(1) for line in group}) == 1
                assert [line.split("] ")[-1][:8] for line in group] == ["00000000", "00000010", "00000020"]
        if len(sys.argv) > 2:
            preview = [line for line in file_lines if any(f"[hexdump/{name}]" in line for name in ["u8-35", "u16-35", "u32-35", "u64-35"])]
            Path(sys.argv[2]).write_text("\n".join(preview) + "\n")
    print("real hexdump runtime: 144 format/length cases, legacy API, complete prefixes, native words, tails, unaligned buffers, mirroring, console-only/OFF and concurrent batches passed")


if __name__ == "__main__":
    main()
