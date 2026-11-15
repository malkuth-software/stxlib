#!/usr/bin/env python3
"""web_check.py: drive the web service in a test VM over HTTPS (C2).

  web_check.py --port PORT --password-file FILE --phase first|second
               [--timeout SECONDS]

Connects to 127.0.0.1:PORT (the host end of the VM's forwarded port),
retrying until the service answers or the timeout passes, and checks the
interface the way a browser uses it. Every check prints one line; the first
failure stops the run.

  first   the pages and their security fields; the session (a wrong password
          refused, the right one accepted); the supervisor's state and
          services; stopping and starting a service; a POST without the CSRF
          token refused; the web service's own stop refused; the user
          configuration written by turns (a stale base refused with 409, a
          factory name refused by the user rules); an upload streamed and
          hashed; the targets page; then a reboot.
  second  after that reboot: the same certificate, the configuration as
          written, a logout that ends the session.

Prints "WEBCHECK fingerprint=<SHA-256 of the certificate, AB:CD:...>" so the
caller can compare it with the console's line, and "WEBCHECK <phase> PASS"
at the end. Exit status 0 when every check passed, 1 otherwise, 2 on a usage
error. Python's standard library only; the certificate is self-signed, so it
is pinned by fingerprint rather than verified.
"""

import argparse
import hashlib
import http.client
import json
import os
import secrets
import socket
import ssl
import sys
import time
from typing import Dict, List, Optional, Tuple

CONNECT_RETRY_SECONDS: float = 2.0
REQUEST_TIMEOUT_SECONDS: float = 30.0
UPLOAD_BYTES: int = 1024 * 1024
FIRST_SETTINGS: str = '[system]\nhostname = stx-web-check\n'


class CheckFailed(Exception):
    pass


def tls_context() -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    return context


def fingerprint_of(der: bytes) -> str:
    digest = hashlib.sha256(der).hexdigest().upper()
    return ':'.join(digest[index:index + 2] for index in range(0, len(digest), 2))


class Client:
    def __init__(self, port: int) -> None:
        self.port = port
        self.context = tls_context()
        self.cookie: Optional[str] = None
        self.csrf: Optional[str] = None
        self.fingerprint: Optional[str] = None

    def request(self, method: str, path: str, body: Optional[bytes] = None,
                content_type: Optional[str] = 'application/json', csrf: bool = True,
                cookie: Optional[str] = None) -> Tuple[int, Dict[str, str], bytes]:
        connection = http.client.HTTPSConnection('127.0.0.1', self.port, context=self.context,
                                                 timeout=REQUEST_TIMEOUT_SECONDS)
        headers: Dict[str, str] = {}
        if body is not None and content_type is not None:
            headers['Content-Type'] = content_type
        session_cookie = cookie if cookie is not None else self.cookie
        if session_cookie is not None:
            headers['Cookie'] = f'stxsession={session_cookie}'
        if csrf and self.csrf is not None and method == 'POST':
            headers['X-STX-CSRF'] = self.csrf
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            data = response.read()
            if connection.sock is not None:
                der = connection.sock.getpeercert(binary_form=True)
                if der:
                    self.fingerprint = fingerprint_of(der)
            fields = {name.lower(): value for name, value in response.getheaders()}
            return response.status, fields, data
        finally:
            connection.close()

    def json_request(self, method: str, path: str, value: Optional[object] = None,
                     csrf: bool = True, cookie: Optional[str] = None) -> Tuple[int, Dict[str, str], object]:
        body = None
        if method == 'POST':
            body = json.dumps(value if value is not None else {}).encode('utf-8')
        status, fields, data = self.request(method, path, body=body, csrf=csrf, cookie=cookie)
        try:
            parsed = json.loads(data.decode('utf-8')) if data else None
        except ValueError:
            parsed = None
        return status, fields, parsed


def check(condition: bool, what: str) -> None:
    if not condition:
        raise CheckFailed(what)
    print(f'WEBCHECK ok {what}')


def wait_for_service(client: Client, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_failure = ''
    while time.monotonic() < deadline:
        try:
            status, _, _ = client.request('GET', '/api/session', content_type=None)
            if status == 200:
                print(f'WEBCHECK fingerprint={client.fingerprint}')
                return
            last_failure = f'HTTP {status}'
        except (OSError, ssl.SSLError, http.client.HTTPException) as failure:
            last_failure = str(failure)
        time.sleep(CONNECT_RETRY_SECONDS)
    raise CheckFailed(f'service did not answer within {timeout:.0f} s: {last_failure}')


def log_in(client: Client, password: str) -> None:
    status, fields, value = client.json_request('POST', '/api/login', {'password': password})
    check(status == 200 and isinstance(value, dict) and isinstance(value.get('csrf'), str), 'login accepted')
    set_cookie = fields.get('set-cookie', '')
    check(set_cookie.startswith('stxsession=') and 'HttpOnly' in set_cookie and 'Secure' in set_cookie
          and 'SameSite=Strict' in set_cookie, 'session cookie HttpOnly, Secure, SameSite=Strict')
    client.cookie = set_cookie.split(';', 1)[0].split('=', 1)[1]
    client.csrf = value['csrf']


def phase_first(client: Client, password: str) -> None:
    status, fields, data = client.request('GET', '/', content_type=None)
    check(status == 200 and b'app.js' in data, 'index page served')
    check("default-src 'self'" in fields.get('content-security-policy', ''), 'content security policy present')
    check(fields.get('x-frame-options') == 'DENY', 'framing refused')
    status, _, value = client.json_request('GET', '/api/session')
    check(status == 200 and isinstance(value, dict) and value.get('logged_in') is False
          and value.get('setup_required') is False, 'session: provisioned, not logged in')
    status, _, _ = client.json_request('GET', '/api/supervisor')
    check(status == 401, 'supervisor state needs a session')
    status, _, _ = client.json_request('POST', '/api/login', {'password': password + 'x'})
    check(status == 401, 'wrong password refused')
    log_in(client, password)
    status, _, value = client.json_request('GET', '/api/supervisor')
    check(status == 200 and isinstance(value, dict) and value.get('init_done') is True, 'supervisor state read')
    status, _, value = client.json_request('GET', '/api/services')
    names = [record.get('name') for record in value.get('services', [])] if isinstance(value, dict) else []
    check(status == 200 and 'web' in names and 'sleeper' in names, 'services listed')
    status, _, _ = client.json_request('POST', '/api/services/sleeper/stop')
    check(status == 200, 'sleeper stopped')
    status, _, _ = client.json_request('POST', '/api/services/sleeper/start')
    check(status == 200, 'sleeper started')
    status, _, _ = client.json_request('POST', '/api/services/sleeper/stop', csrf=False)
    check(status == 403, 'POST without CSRF token refused')
    status, _, _ = client.json_request('POST', '/api/services/web/stop')
    check(status != 200, 'stop of the web service itself refused')
    status, _, value = client.json_request('GET', '/api/settings')
    check(status == 200 and isinstance(value, dict) and value.get('sequence') == 0, 'user settings empty at first')
    status, _, value = client.json_request('POST', '/api/settings', {'text': FIRST_SETTINGS, 'base_sequence': 0})
    check(status == 200 and isinstance(value, dict) and value.get('sequence') == 1 and value.get('pending') is True,
          'user settings written as turn 1')
    status, _, _ = client.json_request('POST', '/api/settings', {'text': FIRST_SETTINGS, 'base_sequence': 0})
    check(status == 409, 'stale base sequence refused')
    status, _, value = client.json_request('POST', '/api/settings/check',
                                           {'text': '[service-web]\ncommand = /bin/sh\n'})
    check(status == 200 and isinstance(value, dict) and value.get('ok') is False, 'factory service name refused')
    payload = secrets.token_bytes(UPLOAD_BYTES)
    status, _, data = client.request('POST', '/api/upload/package', body=payload,
                                     content_type='application/octet-stream')
    try:
        uploaded = json.loads(data.decode('utf-8'))
    except ValueError:
        uploaded = {}
    check(status == 200 and uploaded.get('bytes') == UPLOAD_BYTES
          and uploaded.get('sha256') == hashlib.sha256(payload).hexdigest(), 'upload stored with its SHA-256')
    status, _, value = client.json_request('GET', '/api/targets')
    check(status == 200 and isinstance(value, dict) and isinstance(value.get('targets'), list), 'targets page read')
    status, _, _ = client.json_request('POST', '/api/reboot')
    check(status == 200, 'reboot accepted')


def phase_second(client: Client, password: str) -> None:
    log_in(client, password)
    status, _, value = client.json_request('GET', '/api/settings')
    check(status == 200 and isinstance(value, dict) and value.get('sequence') == 1
          and value.get('text') == FIRST_SETTINGS and value.get('pending') is False,
          'user settings kept across the reboot')
    old_cookie = client.cookie
    status, _, _ = client.json_request('POST', '/api/logout')
    check(status == 200, 'logout')
    status, _, _ = client.json_request('GET', '/api/supervisor', cookie=old_cookie)
    check(status == 401, 'session ended by logout')


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description='check the web service in a test VM')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--password-file', required=True)
    parser.add_argument('--phase', choices=['first', 'second'], required=True)
    parser.add_argument('--timeout', type=float, default=240.0)
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.password_file, 'r', encoding='utf-8') as stream:
            password = stream.readline().rstrip('\r\n')
    except OSError as failure:
        print(f'web_check: {failure}', file=sys.stderr)
        return 2
    client = Client(arguments.port)
    try:
        wait_for_service(client, arguments.timeout)
        if arguments.phase == 'first':
            phase_first(client, password)
        else:
            phase_second(client, password)
    except CheckFailed as failure:
        print(f'WEBCHECK FAIL {failure}')
        return 1
    except (OSError, ssl.SSLError, http.client.HTTPException) as failure:
        print(f'WEBCHECK FAIL connection: {failure}')
        return 1
    print(f'WEBCHECK {arguments.phase} PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
