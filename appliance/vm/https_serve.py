#!/usr/bin/env python3
"""https_serve.py: the C2 VM scenario's update server (appliance/vm/c2_m4.sh).

Serves one directory over HTTPS on 127.0.0.1 only (QEMU's user network
gives the guest the host's loopback as 10.0.2.2), with the scenario's
throwaway certificate, until killed. Test only: the certificate's CA is
trusted by the scenario's image alone (payload extra-ca.pem), and its
private keys are deleted when the scenario ends.

Usage: https_serve.py --dir DIR --cert CHAIN.pem --key KEY.pem --port N
"""

import argparse
import functools
import http.server
import ssl
import sys
from typing import List


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format_string: str, *arguments) -> None:
        sys.stderr.write('https_serve: ' + (format_string % arguments) + '\n')


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--dir', required=True)
    parser.add_argument('--cert', required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--port', type=int, required=True)
    arguments = parser.parse_args(argv)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(arguments.cert, arguments.key)
    handler = functools.partial(QuietHandler, directory=arguments.dir)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', arguments.port), handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
