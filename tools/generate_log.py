#!/usr/bin/env python3
"""
generate_log.py — Synthetic log file generator for stress testing

Generates a massive log file with configurable line count.
Randomly intersperses error patterns for pattern matcher validation.

Usage:
    python generate_log.py [--lines N] [--output FILE] [--error-rate FLOAT]

Examples:
    python generate_log.py --lines 10000000 --output massive_test.log
    python generate_log.py --lines 1000000 --error-rate 0.01
"""

import argparse
import random
import sys
from datetime import datetime, timedelta

# Normal log message templates
NORMAL_MESSAGES = [
    "sshd[{pid}]: Accepted publickey for admin from {ip} port {port} ssh2",
    "sshd[{pid}]: pam_unix(sshd:session): session opened for user admin",
    "systemd[1]: Started Session {session} of user admin.",
    "CRON[{pid}]: pam_unix(cron:session): session opened for user root",
    "kernel: [UFW BLOCK] IN=eth0 OUT= MAC={mac} SRC={ip} DST=10.0.0.1",
    "systemd[1]: logrotate.service: Succeeded.",
    "sshd[{pid}]: Received disconnect from {ip} port {port}:11: disconnected by user",
    "kernel: audit: type=1400 audit(timestamp): apparmor=\"ALLOWED\" operation=\"open\"",
    "systemd-resolved[{pid}]: Server returned error NXDOMAIN",
    "dhclient[{pid}]: DHCPREQUEST for {ip} on eth0 to 10.0.0.1 port 67",
    "postfix/smtp[{pid}]: connect to mail.example.com[{ip}]:25: Connection timed out",
    "nginx: {ip} - - [timestamp] \"GET /index.html HTTP/1.1\" 200 612",
]

# Error patterns that the monitor should detect
ERROR_MESSAGES = [
    "sshd[{pid}]: Failed password for root from {ip} port {port} ssh2",
    "sshd[{pid}]: Failed password for invalid user admin from {ip} port {port} ssh2",
    "sshd[{pid}]: Invalid user hacker from {ip} port {port}",
    "kernel: [UFW BLOCK] IN=eth0 OUT= SRC={ip} DST=10.0.0.1 PROTO=TCP DPT=22",
    "sshd[{pid}]: Connection refused from {ip}",
    "sshd[{pid}]: error: maximum authentication attempts exceeded for root from {ip}",
    "kernel: Out of memory: Killed process {pid} (java) total-vm:2048000kB",
    "systemd[1]: CRITICAL: Service apache2 failed to start",
    "sshd[{pid}]: FATAL: no hostkeys available -- exiting.",
    "kernel: segfault at 0000000000000000 ip 00007f3a2b4c5d6e sp 00007ffd1a2b3c4d",
]


def random_ip():
    return f"{random.randint(1, 254)}.{random.randint(0, 255)}.{random.randint(0, 255)}.{random.randint(1, 254)}"


def random_mac():
    return ":".join(f"{random.randint(0, 255):02x}" for _ in range(6))


def format_message(template, timestamp):
    return template.format(
        pid=random.randint(1000, 65535),
        ip=random_ip(),
        port=random.randint(1024, 65535),
        session=random.randint(1, 9999),
        mac=random_mac(),
    )


def generate_log(num_lines, output_file, error_rate):
    base_time = datetime(2025, 1, 1, 0, 0, 0)
    out = open(output_file, "w") if output_file else sys.stdout

    try:
        for i in range(num_lines):
            timestamp = base_time + timedelta(seconds=i * 0.001)
            ts_str = timestamp.strftime("%b %d %H:%M:%S")
            hostname = "server01"

            if random.random() < error_rate:
                template = random.choice(ERROR_MESSAGES)
            else:
                template = random.choice(NORMAL_MESSAGES)

            message = format_message(template, timestamp)
            line = f"{ts_str} {hostname} {message}\n"
            out.write(line)

            if i % 1_000_000 == 0 and i > 0:
                print(f"  Generated {i:,} / {num_lines:,} lines...", file=sys.stderr)

    finally:
        if output_file:
            out.close()

    print(
        f"Done: generated {num_lines:,} lines"
        + (f" → {output_file}" if output_file else " → stdout"),
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic log files")
    parser.add_argument(
        "--lines", "-n", type=int, default=1_000_000, help="Number of lines (default: 1M)"
    )
    parser.add_argument(
        "--output", "-o", type=str, default=None, help="Output file (default: stdout)"
    )
    parser.add_argument(
        "--error-rate",
        "-e",
        type=float,
        default=0.005,
        help="Fraction of lines with error patterns (default: 0.005 = 0.5%%)",
    )
    args = parser.parse_args()
    generate_log(args.lines, args.output, args.error_rate)


if __name__ == "__main__":
    main()
