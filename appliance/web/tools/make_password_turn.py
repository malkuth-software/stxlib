#!/usr/bin/env python3
"""make_password_turn.py: a provisioned password for the web service (C2).

Writes one turn file (runtime/store/turn_file.h) holding one password record
(runtime/web/auth.h), for the web service's state directory, so that an
appliance can be prepared on the host with its first password instead of
the setup code on its console (C2-web.md, owner question 3, recommended
answer). Copy the file to <state>/password.0 (by default
/user/stx/web/password.0 on the user partition) and remove password.1.

  make_password_turn.py --password-file FILE --out PATH [--sequence N]

The password is read from FILE (its first line, without the line end), not
from the command line, where other users could see it. The record uses
scrypt with the service's default costs (N=2^15, r=8, p=3), which Python's
hashlib offers through OpenSSL; the service reads any record within the
bounds of auth.h. Exit status 0 when the file was written, 1 on a failure,
2 on a usage error.
"""

import argparse
import hashlib
import os
import secrets
import sys
import zlib
from typing import List

SALT_BYTES: int = 16
HASH_BYTES: int = 32
LOG2_N: int = 15
R: int = 8
P: int = 3
PASSWORD_BYTES_MIN: int = 8
PASSWORD_BYTES_MAX: int = 256
SEQUENCE_MAX: int = 0xFFFFFFFF
# 128 * r * N bytes of scrypt's memory, and a margin.
SCRYPT_MAXMEM: int = 128 * R * (1 << LOG2_N) + 1024 * 1024


def password_record(password: bytes, salt: bytes) -> bytes:
    derived = hashlib.scrypt(password, salt=salt, n=1 << LOG2_N, r=R, p=P, maxmem=SCRYPT_MAXMEM, dklen=HASH_BYTES)
    return f'scrypt${LOG2_N}${R}${P}${salt.hex()}${derived.hex()}\n'.encode('ascii')


def turn_file(sequence: int, body: bytes) -> bytes:
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return f'STXTURN 1 {sequence} {len(body)} {crc:08x}\n'.encode('ascii') + body


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description='a provisioned password turn file for stxweb')
    parser.add_argument('--password-file', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--sequence', type=int, default=1)
    arguments = parser.parse_args(argv)
    if arguments.sequence < 1 or arguments.sequence > SEQUENCE_MAX:
        print('make_password_turn: --sequence must be 1..4294967295', file=sys.stderr)
        return 2
    try:
        with open(arguments.password_file, 'rb') as stream:
            password = stream.readline().rstrip(b'\r\n')
    except OSError as failure:
        print(f'make_password_turn: {failure}', file=sys.stderr)
        return 1
    if len(password) < PASSWORD_BYTES_MIN or len(password) > PASSWORD_BYTES_MAX or b'\0' in password:
        print('make_password_turn: the password must be 8..256 bytes without NUL', file=sys.stderr)
        return 1
    body = password_record(password, secrets.token_bytes(SALT_BYTES))
    data = turn_file(arguments.sequence, body)
    try:
        descriptor = os.open(arguments.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as failure:
        print(f'make_password_turn: {failure}', file=sys.stderr)
        return 1
    print(f'make_password_turn: wrote {arguments.out} ({len(data)} bytes, scrypt N=2^{LOG2_N} r={R} p={P})')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
