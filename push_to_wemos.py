#!/usr/bin/env python3
"""Reads /tmp/claude-quota.json and sends quota data to the Wemos D1 Mini via UDP."""

import json
import os
import socket
import sys
import time

QUOTA_FILE = "/tmp/claude-quota.json"
WEMOS_HOST = os.environ.get("WEMOS_HOST", "")
WEMOS_PORT = 4210

if not WEMOS_HOST:
    print("WEMOS_HOST not set", file=sys.stderr)
    sys.exit(1)

try:
    with open(QUOTA_FILE) as f:
        data = json.load(f)
except FileNotFoundError:
    sys.exit(0)  # no data yet

now = int(time.time())

def mins_remaining(resets_at: int) -> int:
    return max(0, (int(resets_at) - now) // 60)

payload = {
    "five_hour_pct":        data["five_hour"]["used_percentage"],
    "five_hour_resets_min": mins_remaining(data["five_hour"]["resets_at"]),
    "weekly_pct":           data["seven_day"]["used_percentage"],
    "weekly_resets_min":    mins_remaining(data["seven_day"]["resets_at"]),
}

try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(json.dumps(payload).encode(), (WEMOS_HOST, WEMOS_PORT))
    sock.close()
except Exception:
    pass  # fire and forget — Wemos unreachable when away from home
