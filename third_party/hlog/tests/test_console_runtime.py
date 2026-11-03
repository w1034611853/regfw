#!/usr/bin/env python3
"""Real runtime test, not a mock. Run after scripts/install.sh.

python3 tests/test_console_runtime.py /absolute/path/libhlog.so.1
Each child uses a temporary cwd because the runtime initializes at dlopen.
"""
import ctypes as c
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading


def emit_child(library):
    lib = c.CDLL(library)
    lib.hlog_register_logger.argtypes = [c.c_char_p, c.c_char_p, c.c_uint32]
    lib.hlog_register_logger.restype = c.c_void_p
    lib.hlog_logger_set_level.argtypes = [c.c_void_p, c.c_uint32]
    lib.hlog_logger_config_name.argtypes = [c.c_void_p, c.c_char_p]
    lib.hlog_set_console_enabled.argtypes = [c.c_bool]
    lib.hlog_console_enabled.restype = c.c_bool
    signature = [c.c_void_p, c.c_uint32, c.c_char_p, c.c_size_t,
                 c.c_char_p, c.c_size_t, c.c_uint32, c.c_char_p, c.c_size_t]
    for name in ('hlog_write', 'hlog_console_write', 'hlog_hexdump', 'hlog_console_hexdump'):
        getattr(lib, name).argtypes = signature
    logger = lib.hlog_register_logger(b'console-test', b'0', 2)
    assert logger

    def emit(name, message, level=2, handle=logger):
        data = message.encode() if isinstance(message, str) else message
        assert getattr(lib, name)(handle, level, b'test_fn', 7,
                                   b'test.c', 6, 1, data, len(data)) == 0

    assert not lib.hlog_console_enabled()
    emit('hlog_write', 'FILE_ONLY')
    emit('hlog_console_write', 'DIRECT_DEBUG', 1)
    assert lib.hlog_logger_set_level(logger, 6) == 0
    emit('hlog_write', 'FILTERED')
    emit('hlog_console_write', 'DIRECT_WHILE_OFF', 0)
    emit('hlog_console_write', 'LEVEL_OFF_MUST_NOT_PRINT', 6)
    assert lib.hlog_logger_set_level(logger, 0) == 0
    assert lib.hlog_set_console_enabled(True) == 0
    assert lib.hlog_console_enabled()
    for level in range(6):
        emit('hlog_write', f'LEVEL_{level}', level)
    emit('hlog_console_write', 'DIRECT_NO_DUPLICATE')
    emit('hlog_write', 'MULTILINE_A\nMULTILINE_B')
    future = lib.hlog_register_logger(b'console-test', b'future', 2)
    assert future
    emit('hlog_write', 'FUTURE', handle=future)
    assert lib.hlog_logger_config_name(future, b'renamed') == 0
    emit('hlog_write', 'RENAMED', handle=future)
    emit('hlog_console_write', 'DIRECT_RENAMED', handle=future)
    emit('hlog_hexdump', bytes(range(20)))
    assert lib.hlog_set_console_enabled(False) == 0
    emit('hlog_write', 'AFTER_OFF')
    emit('hlog_console_hexdump', b'Console packet')

    failures = []
    def worker(index):
        try:
            for number in range(20):
                emit('hlog_console_write', f'THREAD_{index}_{number}')
        except BaseException as error:
            failures.append(error)
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for thread in threads: thread.start()
    for thread in threads: thread.join()
    assert not failures
    assert lib.hlog_flush() == 0


def main():
    library = str(Path(sys.argv[1]).resolve())
    assert Path(library).is_file(), 'build/install the actual runtime first'
    if len(sys.argv) > 2 and sys.argv[2] == '--child':
        emit_child(library)
        return
    for color in ('never', 'auto', 'always'):
        with tempfile.TemporaryDirectory(prefix='hlog-console-test-') as directory:
            env = dict(os.environ, HLOG_CONSOLE_COLOR=color)
            run = subprocess.run([sys.executable, str(Path(__file__).resolve()), library, '--child'],
                                 cwd=directory, env=env, capture_output=True, check=True)
            assert run.stdout == b''
            raw_console = run.stderr.decode()
            file_text = Path(directory, 'logs/qemu_device.log').read_text()
            ansi = re.compile(r'\x1b\[[0-9;]*m')
            console = ansi.sub('', raw_console)
            assert ('\x1b[' in raw_console) == (color == 'always')
            assert '\x1b[' not in file_text and '[HLOG]' not in file_text
            assert 'FILE_ONLY' in file_text and 'FILE_ONLY' not in console
            assert 'AFTER_OFF' in file_text and 'AFTER_OFF' not in console
            assert 'FILTERED' not in console + file_text
            assert 'LEVEL_OFF_MUST_NOT_PRINT' not in console + file_text
            for marker in ('DIRECT_DEBUG', 'DIRECT_WHILE_OFF', 'DIRECT_NO_DUPLICATE', 'DIRECT_RENAMED'):
                assert console.count(marker) == 1 and marker not in file_text
            for line in file_text.splitlines():
                if any(marker in line for marker in ('LEVEL_', '] FUTURE', '] RENAMED', 'MULTILINE_A')):
                    assert '[HLOG] ' + line in console, (line, console)
            assert '[console-test/renamed]' in console
            assert '00000010  10 11 12 13' in console
            assert '|Console packet' in console and '|Console packet' not in file_text
            for i in range(4):
                for n in range(20):
                    assert len(re.findall(rf'\[HLOG\][^\n]*\[test_fn\] THREAD_{i}_{n}\n', console)) == 1
            if color == 'always':
                starts = re.findall(r'(\x1b\[[0-9;]*m)\[HLOG\][^\n]*LEVEL_\d', raw_console)
                assert len(set(starts)) == 6
                assert raw_console.count('\x1b[0m') == raw_console.count('[HLOG]')
    print('real runtime: routing, levels, colors, format equality, rename, future loggers, hexdump, concurrency passed')


if __name__ == '__main__':
    main()
