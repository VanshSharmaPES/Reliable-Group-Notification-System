#!/usr/bin/env python3
"""
benchmark.py — Performance Analysis for Reliable Group Notification System

Spawns N simulated subscribers, fires M notifications, collects:
  - Delivery rate  (packets received / sent)
  - Mean latency   (send timestamp → first receive)
  - Retransmit overhead (per server logs)

Usage:
    # Test reliable server
    python3 benchmark.py --mode reliable --server 127.0.0.1 \
            --group 1 --subscribers 5 --messages 20 --loss 0.10

    # Test baseline server
    python3 benchmark.py --mode baseline --server 127.0.0.1 \
            --group 1 --subscribers 5 --messages 20 --loss 0.10

Requirements:
    sudo tc qdisc add dev lo root netem loss 10% delay 20ms   (to inject loss)
    sudo tc qdisc del dev lo root                              (to remove)
"""

import argparse
import socket
import struct
import threading
import time
import random
import subprocess
import statistics
from dataclasses import dataclass, field
from typing import Optional

# ─── Packet layout (mirrors packet.h) ────────────────────────────────────────
MSG_JOIN   = 0x01
MSG_LEAVE  = 0x02
MSG_NOTIFY = 0x03
MSG_ACK    = 0x04

SERVER_PORT  = 9000
CONTROL_PORT = 9001
MAX_PAYLOAD  = 256
HEADER_FMT   = "!BHHII HB"   # 15 bytes
HEADER_SIZE  = struct.calcsize(HEADER_FMT)


def build_packet(msg_type: int, seq: int, group_id: int,
                 payload: bytes = b"") -> bytes:
    ts_sec  = int(time.time())
    ts_usec = int((time.time() % 1) * 1_000_000)
    pay_len = len(payload)
    # Checksum placeholder = 0 first
    raw = struct.pack(HEADER_FMT,
                      msg_type, seq, group_id,
                      ts_sec, ts_usec,
                      pay_len, 0)
    cs = 0
    for i, b in enumerate(raw):
        if i != 12:
            cs ^= b
    raw = raw[:12] + bytes([cs]) + raw[13:]
    return raw + payload.ljust(MAX_PAYLOAD, b'\x00')


def parse_header(data: bytes) -> dict:
    if len(data) < HEADER_SIZE:
        return {}
    fields = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    return {
        "msg_type":   fields[0],
        "seq":        fields[1],
        "group_id":   fields[2],
        "ts_sec":     fields[3],
        "ts_usec":    fields[4],
        "pay_len":    fields[5],
        "checksum":   fields[6],
    }


# ─── Subscriber ───────────────────────────────────────────────────────────────
@dataclass
class SubResult:
    sub_id:     int
    received:   list = field(default_factory=list)  # (seq, latency_ms)
    duplicates: int  = 0
    bad_cs:     int  = 0


class Subscriber(threading.Thread):
    def __init__(self, sub_id: int, server_ip: str, group_id: int,
                 expected: int, timeout: float = 5.0):
        super().__init__(daemon=True)
        self.sub_id    = sub_id
        self.server_ip = server_ip
        self.group_id  = group_id
        self.expected  = expected
        self.timeout   = timeout
        self.result    = SubResult(sub_id=sub_id)
        self._seen     = set()
        self._done     = threading.Event()

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(1.0)
        self.local_port = self.sock.getsockname()[1]

    def _send_ctrl(self, msg_type: int):
        pkt = build_packet(msg_type, 0, self.group_id)
        self.sock.sendto(pkt, (self.server_ip, CONTROL_PORT))

    def _send_ack(self, seq: int):
        ack = build_packet(MSG_ACK, seq, self.group_id)
        self.sock.sendto(ack, (self.server_ip, SERVER_PORT))

    def run(self):
        self._send_ctrl(MSG_JOIN)
        deadline = time.time() + self.timeout

        while time.time() < deadline:
            try:
                data, _ = self.sock.recvfrom(HEADER_SIZE + MAX_PAYLOAD)
            except socket.timeout:
                if len(self.result.received) >= self.expected:
                    break
                continue

            h = parse_header(data)
            if not h or h["msg_type"] != MSG_NOTIFY:
                continue

            seq = h["seq"]
            recv_ts = time.time()
            send_ts = h["ts_sec"] + h["ts_usec"] / 1_000_000
            latency_ms = (recv_ts - send_ts) * 1000

            # Always ACK (even duplicates, to stop retransmit)
            self._send_ack(seq)

            if seq in self._seen:
                self.result.duplicates += 1
            else:
                self._seen.add(seq)
                self.result.received.append((seq, latency_ms))

            if len(self.result.received) >= self.expected:
                deadline = time.time() + 0.5  # allow stragglers to flush

        self._send_ctrl(MSG_LEAVE)
        self.sock.close()


# ─── Traffic control helpers ──────────────────────────────────────────────────
def apply_netem(loss_pct: float, delay_ms: float = 20.0):
    """Apply tc netem on loopback. Requires root."""
    # Remove any existing qdisc first (ignore error)
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"],
                   capture_output=True)
    cmd = ["tc", "qdisc", "add", "dev", "lo", "root", "netem",
           "loss", f"{loss_pct*100:.0f}%",
           "delay", f"{delay_ms:.0f}ms"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[WARN] tc netem failed (run as root?): {r.stderr.strip()}")
    else:
        print(f"[NET] Simulating {loss_pct*100:.0f}% loss + {delay_ms:.0f}ms delay")


def remove_netem():
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"],
                   capture_output=True)
    print("[NET] Removed tc netem rules")


# ─── Main benchmark runner ───────────────────────────────────────────────────
def run_benchmark(args):
    print(f"\n{'='*56}")
    print(f"  Mode: {args.mode.upper()}  |  Subscribers: {args.subscribers}")
    print(f"  Messages: {args.messages}  |  Simulated loss: {args.loss*100:.0f}%")
    print(f"{'='*56}\n")

    if args.loss > 0:
        apply_netem(args.loss, args.delay_ms)

    # Launch subscribers
    subs = [
        Subscriber(i, args.server, args.group, args.messages)
        for i in range(args.subscribers)
    ]
    for s in subs:
        s.start()
    time.sleep(0.3)  # Give time for all JOINs to arrive

    # Send notifications via stdin pipe to server process
    # (In a real test, you'd feed the server's stdin directly)
    # Here we simulate by recording send timestamps and printing commands.
    print(f"[BENCH] Feed the following to your server's stdin:")
    send_times: dict[int, float] = {}
    for i in range(args.messages):
        msg = f"Alert #{i:04d} — {'x' * random.randint(10, 40)}"
        send_times[i] = time.time()
        print(f"  {args.group}:{msg}")
        time.sleep(0.05)  # 50ms between messages

    # Wait for subscribers to finish
    for s in subs:
        s.join(timeout=args.messages * 0.5 + 8)

    # ─── Aggregate results ────────────────────────────────────────────────
    all_latencies = []
    total_received = 0
    total_duplicates = 0

    for s in subs:
        total_received   += len(s.result.received)
        total_duplicates += s.result.duplicates
        all_latencies    += [lat for _, lat in s.result.received]

    total_expected = args.subscribers * args.messages
    delivery_rate  = total_received / total_expected if total_expected else 0

    print(f"\n{'─'*56}")
    print(f"  RESULTS ({args.mode.upper()})")
    print(f"{'─'*56}")
    print(f"  Expected deliveries : {total_expected}")
    print(f"  Actual deliveries   : {total_received}")
    print(f"  Delivery rate       : {delivery_rate*100:.1f}%")
    print(f"  Duplicate ACKs      : {total_duplicates}")

    if all_latencies:
        print(f"  Mean latency        : {statistics.mean(all_latencies):.2f} ms")
        print(f"  Median latency      : {statistics.median(all_latencies):.2f} ms")
        print(f"  Std dev latency     : {statistics.stdev(all_latencies):.2f} ms"
              if len(all_latencies) > 1 else "")
        print(f"  Min / Max latency   : {min(all_latencies):.2f} / "
              f"{max(all_latencies):.2f} ms")
    print(f"{'─'*56}\n")

    if args.loss > 0:
        remove_netem()

    return {
        "mode":          args.mode,
        "loss":          args.loss,
        "delivery_rate": delivery_rate,
        "mean_latency":  statistics.mean(all_latencies) if all_latencies else None,
        "duplicates":    total_duplicates,
    }


# ─── Multi-scenario sweep ─────────────────────────────────────────────────────
def run_sweep(args):
    """Run across multiple loss rates and print comparison table."""
    loss_rates = [0.0, 0.05, 0.10, 0.20]
    results = []
    for loss in loss_rates:
        args.loss = loss
        r = run_benchmark(args)
        results.append(r)

    print("\n" + "="*60)
    print("  SWEEP SUMMARY")
    print(f"  {'Loss %':<10} {'Delivery %':<15} {'Mean Latency':<18} {'Duplicates'}")
    print("  " + "-"*56)
    for r in results:
        lat = f"{r['mean_latency']:.2f} ms" if r['mean_latency'] else "N/A"
        print(f"  {r['loss']*100:<10.0f} {r['delivery_rate']*100:<15.1f} "
              f"{lat:<18} {r['duplicates']}")
    print("="*60)


# ─── CLI ─────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="RGNS Benchmark")
    parser.add_argument("--mode",        default="reliable",
                        choices=["reliable", "baseline"],
                        help="Which server mode to test")
    parser.add_argument("--server",      default="127.0.0.1",
                        help="Server IP address")
    parser.add_argument("--group",       type=int, default=1,
                        help="Group ID to subscribe to")
    parser.add_argument("--subscribers", type=int, default=5,
                        help="Number of simulated subscribers")
    parser.add_argument("--messages",    type=int, default=20,
                        help="Number of notifications to send")
    parser.add_argument("--loss",        type=float, default=0.0,
                        help="Simulated packet loss fraction (e.g. 0.10 = 10%%)")
    parser.add_argument("--delay-ms",    type=float, default=20.0,
                        dest="delay_ms",
                        help="Simulated network delay in ms")
    parser.add_argument("--sweep",       action="store_true",
                        help="Run across multiple loss rates automatically")
    args = parser.parse_args()

    if args.sweep:
        run_sweep(args)
    else:
        run_benchmark(args)
