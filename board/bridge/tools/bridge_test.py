#!/usr/bin/env python3
# RoadStud bridge comms test — makes the Pi play the role of the iPhone app so we
# can verify the panda NCM bridge end-to-end without iOS.
#
# When the bridge firmware is running, the panda enumerates as USB-CDC-NCM; Linux
# brings up a network interface and the panda's DHCP server leases it 192.168.4.x.
# This script then speaks the raw panda-bridge UDP protocol to 192.168.4.1:5555:
# sends heartbeats, decodes incoming CAN frames (same 6-byte wire format as
# bridge_protocol.h / PandaProtocol.swift), and prints throughput + unique IDs.
#
# Usage:
#   python3 board/bridge/tools/bridge_test.py            # listen + decode CAN
#   python3 board/bridge/tools/bridge_test.py --reflash  # send RSDFUAPP escape hatch
#   python3 board/bridge/tools/bridge_test.py --reflash-bootstub  # send RSDFUBTL
import socket, struct, subprocess, sys, time

HOST, PORT = "192.168.4.1", 5555
DLC2LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]


def decode(buf):
    frames, off = [], 0
    while off + 6 <= len(buf):
        h0 = buf[off]
        dlc, bus = h0 >> 4, (h0 >> 1) & 0x7
        dlen = DLC2LEN[dlc] if dlc < 16 else 8
        if off + 6 + dlen > len(buf):
            break
        xor = 0
        for i in range(5):
            xor ^= buf[off + i]
        if xor != buf[off + 5]:          # resync on bad checksum
            off += 1
            continue
        addr = (buf[off + 1] << 24) | (buf[off + 2] << 16) | (buf[off + 3] << 8) | buf[off + 4]
        data = buf[off + 6:off + 6 + dlen]
        b = bus - 128 if bus >= 128 else (bus - 192 if bus >= 192 else bus)
        frames.append((b, addr, data))
        off += 6 + dlen
    return frames


def preflight():
    # Warn if we don't have a 192.168.4.x address (NCM interface not up / no DHCP lease).
    try:
        out = subprocess.check_output(["ip", "-4", "-o", "addr"], text=True)
    except Exception:
        return
    if "192.168.4." not in out:
        print("WARNING: no 192.168.4.x interface found — the panda NCM link isn't up yet.")
        print("  Check `ip addr` for a new usb*/eth* device; if it has no IP, try:")
        print("    sudo dhclient <iface>   (or check NetworkManager)")
    else:
        for line in out.splitlines():
            if "192.168.4." in line:
                print("iface:", " ".join(line.split()[1:4]))


def main():
    if "--reflash" in sys.argv or "--reflash-bootstub" in sys.argv:
        tag = b"RSDFUBTL" if "--reflash-bootstub" in sys.argv else b"RSDFUAPP"
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(tag, (HOST, PORT))
        print(f"sent {tag.decode()} to {HOST}:{PORT} — panda should reboot into its flasher")
        return

    preflight()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2.0)
    s.sendto(b"\x00", (HOST, PORT))      # heartbeat: registers us as the client
    print(f"listening on {HOST}:{PORT} (Ctrl-C to stop)")

    seen, n, t0, last_hb = {}, 0, time.time(), 0.0
    try:
        while True:
            now = time.time()
            if now - last_hb > 0.2:
                s.sendto(b"\x00", (HOST, PORT))
                last_hb = now
            try:
                data, _ = s.recvfrom(8192)
            except socket.timeout:
                print("... no data (panda powered? on the CAN bus? NCM link up?)")
                continue
            for b, addr, d in decode(data):
                n += 1
                seen[(b, addr)] = seen.get((b, addr), 0) + 1
            if now - t0 >= 2.0:
                ids = ", ".join(f"{a:#x}@b{b}" for (b, a) in sorted(seen)[:12])
                print(f"{n/2:.0f} fps, {len(seen)} unique IDs: {ids}")
                n, t0 = 0, now
    except KeyboardInterrupt:
        print(f"\nstopped. {len(seen)} unique CAN IDs seen total.")


if __name__ == "__main__":
    main()
