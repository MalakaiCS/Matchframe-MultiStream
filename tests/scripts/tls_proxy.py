#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - test helper.
#
# Minimal TLS-terminating TCP proxy: accepts RTMPS on --listen and forwards the decrypted
# bytes to a plain RTMP receiver on --target. Used to test RTMPS with normal certificate
# verification (certificate signed by a test CA) and with a self-signed certificate
# (expected verification failure). Test use only.

import argparse
import socket
import ssl
import threading


def pump(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def handle(client, context, target):
    try:
        tls = context.wrap_socket(client, server_side=True)
    except (ssl.SSLError, OSError) as exc:
        print(f"handshake failed: {exc}", flush=True)
        client.close()
        return
    try:
        upstream = socket.create_connection(target)
    except OSError as exc:
        print(f"upstream failed: {exc}", flush=True)
        tls.close()
        return
    print("proxying connection", flush=True)
    threading.Thread(target=pump, args=(tls, upstream), daemon=True).start()
    pump(upstream, tls)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, required=True)
    parser.add_argument("--target", type=int, required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    args = parser.parse_args()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.listen))
    server.listen(16)
    print(f"listening on {args.listen}", flush=True)
    while True:
        client, _ = server.accept()
        threading.Thread(target=handle, args=(client, context, ("127.0.0.1", args.target)), daemon=True).start()


if __name__ == "__main__":
    main()
