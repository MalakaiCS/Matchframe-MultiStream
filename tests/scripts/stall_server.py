#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - test helper.
#
# Accepts TCP connections and never reads or writes: simulates a stalled ingest server
# (the RTMP handshake never completes). Test use only.

import argparse
import socket
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, required=True)
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.listen))
    server.listen(16)
    held = []
    print(f"stalling on {args.listen}", flush=True)
    while True:
        client, _ = server.accept()
        held.append(client)
        print(f"accepted connection {len(held)}; never answering", flush=True)
        time.sleep(0.01)


if __name__ == "__main__":
    main()
