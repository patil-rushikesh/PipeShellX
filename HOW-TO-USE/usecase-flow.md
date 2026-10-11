# PipeShellX — Complete Use-Case Flow

## What is the System?

PipeShellX is a **remote command controller**. You sit at one laptop (the **Controller**) and manage many other computers (the **Workers**). You can:
- **Connect** devices to your fleet
- **Run commands** on one, some, or all devices at once
- **Get results** streamed back to your screen in real-time
- **Track history** of what ran, when, and whether it succeeded
- **Disconnect** devices when you're done

---

## The Players

```
┌─────────────────────────────────┐
│     YOUR LAPTOP (Controller)    │
│                                 │
│  • pipeshellx CLI binary        │
│  • fleet.ini (contact list)     │
│  • snapshots.jsonl (diary)      │
│  • audit.jsonl (detailed log)   │
│  • pki/ (security badges)       │
└───────────┬─────────────────────┘
            │
      ┌─────┴──────────────────────────────┐
      │            NETWORK                 │
      │     (Wi-Fi / LAN / Internet)       │
      └──┬──────────┬──────────┬───────────┘
         │          │          │
    ┌────┴───┐ ┌────┴───┐ ┌───┴────┐
    │ Dev 1  │ │ Dev 2  │ │ Dev 3  │
    │ (SSH)  │ │ (SSH)  │ │(Native)│
    │ Server │ │ RPi    │ │ Agent  │
    └────────┘ └────────┘ └────────┘
```

---

## Phase 1: Building Your Fleet (Connect Devices)

### Use Case: You have 3 machines you want to manage

| Device | IP Address | How to Connect | What It Is |
|---|---|---|---|
| Web Server | 192.168.1.50 | SSH (already has SSH) | Ubuntu cloud server |
| Raspberry Pi | 192.168.1.100 | SSH | IoT device at home |
| Build Machine | 10.0.0.5 | Native Agent | High-performance Linux box |

### Step 1: Connect the SSH devices (no setup needed on them)

```bash
# Connect the web server
pipeshellx hosts add 192.168.1.50 \
    --user ubuntu --group web --transport ssh \
    -i fleet.ini

# Connect the Raspberry Pi
pipeshellx hosts add 192.168.1.100 \
    --user pi --group iot --transport ssh --port 22 \
    -i fleet.ini
```

> **What happens:** PipeShellX writes these addresses into `fleet.ini`, like saving
> contacts in your phone. No changes are made on the remote devices — they just
> need SSH running (which Linux has by default).

### Step 2: Connect the native agent device (needs PipeShellX installed on it)

```bash
# First, create security badges (one-time setup)
pipeshellx ca init --cn "My Fleet CA" --dir ./pki
pipeshellx ca issue --san "psx://controller" --ca ./pki --out ./pki/controller
pipeshellx ca issue --san "psx://node/build-box" --ca ./pki --out ./pki/buildbox

# Copy pipeshellx binary + badge files to the build machine
scp ./pipeshellx pki/ca.crt pki/buildbox.crt pki/buildbox.key  user@10.0.0.5:~/

# On the build machine, start the listener:
# ssh user@10.0.0.5
# ./pipeshellx node --cert buildbox.crt --key buildbox.key --ca ca.crt \
#     --listen 0.0.0.0:9443 --allow psx://controller

# Connect it to your fleet
pipeshellx hosts add 10.0.0.5 \
    --group build --transport native --native-port 9443 \
    --san psx://node/build-box \
    -i fleet.ini
```

### Step 3: Verify your fleet

```bash
pipeshellx hosts list -i fleet.ini
```
```
HOST             GROUPS    TAGS    TRANSPORT
192.168.1.50     web       -       ssh
192.168.1.100    iot       -       ssh
10.0.0.5         build     -       native
```

You now have 3 devices connected and ready to receive commands.

---

## Phase 2: Running Operations on Devices

### Use Case A: Run a command on ALL devices at once

```bash
pipeshellx run -i fleet.ini --stream -- uname -a
```
```
[192.168.1.50]  Linux web-server 5.15.0 #1 SMP x86_64
[192.168.1.100] Linux raspberrypi 6.1.0 #1 SMP aarch64
[10.0.0.5]      Linux build-box 6.8.0 #1 SMP x86_64
```

> **What happens:** PipeShellX opens connections to all 3 machines simultaneously,
> sends the command `uname -a`, and streams each machine's output back to your
> terminal as it arrives. Each line is prefixed with the host name so you know
> who said what.

---

### Use Case B: Run a command on only ONE group

```bash
# Check disk space only on the web servers
pipeshellx run -i fleet.ini -g web --stream -- df -h /

# Check temperature only on the Raspberry Pi
pipeshellx run -i fleet.ini -g iot --stream -- vcgencmd measure_temp

# Trigger a build only on the build machine
pipeshellx run -i fleet.ini -g build \
    --transport native \
    --cert ./pki/controller.crt --key ./pki/controller.key --ca ./pki/ca.crt \
    -- make -C /home/user/project build
```

---

### Use Case C: Get results as structured JSON (for scripts/automation)

```bash
pipeshellx run -i fleet.ini --json -- hostname
```
```json
[
  {"host": "192.168.1.50", "exit_code": 0, "stdout": "web-server\n"},
  {"host": "192.168.1.100", "exit_code": 0, "stdout": "raspberrypi\n"},
  {"host": "10.0.0.5", "exit_code": 0, "stdout": "build-box\n"}
]
```

> **What happens:** Instead of printing human-readable text, results come back as
> JSON — perfect for piping into `jq`, Python scripts, or monitoring dashboards.

---

### Use Case D: Run with timeout and failure handling

```bash
# Give each device 10 seconds max. If any fails, stop the rest immediately.
pipeshellx run -i fleet.ini \
    --timeout 10 \
    --fail-fast \
    --stream \
    -- /usr/bin/apt update
```

> **What happens:**
> - Each device gets 10 seconds to finish. If it takes longer, it's killed.
> - `--fail-fast` means: if Device 1 fails, don't bother running on Device 2 and 3.
> - Useful for deployments where you want to stop immediately on first error.

---

### Use Case E: Track everything with audit logs and snapshots

```bash
pipeshellx run -i fleet.ini \
    --stream \
    --audit-log ./audit.jsonl \
    --snapshot-file ./snapshots.jsonl \
    -- systemctl status nginx
```

> **What happens:**
> - The command runs on all devices.
> - `audit.jsonl` records: who ran what, when, how many retries, exit codes.
> - `snapshots.jsonl` records: the global cluster state with Lamport timestamps
>   (which device finished first, second, third — even if clocks are out of sync).

### Inspect the history later:

```bash
# See a table of what happened
pipeshellx snapshot dump ./snapshots.jsonl

# Get the latest state as JSON
pipeshellx snapshot dump ./snapshots.jsonl --latest --json
```
```
=== Cluster Snapshot [Run: 123456789] @ 2026-09-05 12:30:00 UTC ===
HOST             STAGE    STATUS    EXIT    LAMPORT_TS
-----------------------------------------------------
192.168.1.50     s0       exited    0       2
192.168.1.100    s0       exited    0       3
10.0.0.5         s0       exited    0       4
```

---

## Phase 3: Data Flow — How Results Come Back

```
  Your Laptop                          Remote Device
  ──────────                           ─────────────
       │                                     │
       │── "Run: df -h" ──────────────────>  │
       │                                     │ Executes df -h
       │                                     │ stdout: "Filesystem  Size..."
       │  <── stdout bytes (streaming) ────  │
       │  <── stdout bytes (more data) ────  │
       │  <── exit code: 0 ────────────────  │
       │                                     │
  Prints to                            Done. Waiting
  your screen                          for next command.
  Writes to
  audit.jsonl
  snapshots.jsonl
```

### What data can you get back from the remote device?

| Data Type | How You Get It | Example |
|---|---|---|
| **stdout** (normal output) | Streamed live to your terminal | `[host] Linux 5.15.0 ...` |
| **stderr** (error output) | Streamed live (shown in red) | `[host] Permission denied` |
| **Exit code** | Shown in summary / JSON / audit log | `0` = success, `1` = failure |
| **Timing** | Recorded in snapshot with Lamport timestamps | `LAMPORT_TS: 3` |
| **Dropped bytes** | If buffer overflows, reported in snapshot | `dropped_bytes: 0` |

### What operations can you perform on remote devices?

You can run **any shell command** that the remote user has permission to execute:

```bash
# Read files
pipeshellx run -i fleet.ini -- cat /etc/hostname

# Install software
pipeshellx run -i fleet.ini -- sudo apt install -y nginx

# Restart services
pipeshellx run -i fleet.ini -- sudo systemctl restart nginx

# Run scripts
pipeshellx run -i fleet.ini -- bash /opt/deploy/deploy.sh

# Check logs
pipeshellx run -i fleet.ini -- tail -20 /var/log/syslog

# Pipeline: find large files, sort, show top 5
pipeshellx run -i fleet.ini -- /bin/sh -c "du -sh /var/* | sort -rh | head -5"
```

---

## Phase 4: Disconnect Devices

### Remove a single device

```bash
pipeshellx hosts remove 192.168.1.100 -i fleet.ini
```
```
removed 192.168.1.100 from fleet.ini
```

### Verify it's gone

```bash
pipeshellx hosts list -i fleet.ini
```
```
HOST             GROUPS    TAGS    TRANSPORT
192.168.1.50     web       -       ssh
10.0.0.5         build     -       native
```

> **What happens:** The device entry is deleted from `fleet.ini`. The remote device
> is not affected at all — nothing is uninstalled or stopped on it. You simply
> stop managing it. You can re-add it anytime.

---

## Complete Lifecycle Summary

```
  ┌──────────────────────────────────────────────────┐
  │              LIFECYCLE OF A DEVICE                │
  │                                                   │
  │  1. CONNECT                                       │
  │     pipeshellx hosts add <IP> ... -i fleet.ini    │
  │              │                                    │
  │              ▼                                    │
  │  2. LIST / VERIFY                                 │
  │     pipeshellx hosts list -i fleet.ini            │
  │              │                                    │
  │              ▼                                    │
  │  3. OPERATE (repeat as many times as needed)      │
  │     pipeshellx run -i fleet.ini -- <command>      │
  │              │                                    │
  │              ▼                                    │
  │  4. INSPECT HISTORY                               │
  │     pipeshellx snapshot dump snapshots.jsonl      │
  │              │                                    │
  │              ▼                                    │
  │  5. DISCONNECT                                    │
  │     pipeshellx hosts remove <IP> -i fleet.ini     │
  └──────────────────────────────────────────────────┘
```

---

## Quick Reference Card

| What You Want | Command |
|---|---|
| **Connect** a device (SSH) | `pipeshellx hosts add <IP> --user <user> --group <group> --transport ssh -i fleet.ini` |
| **Connect** a device (Native) | `pipeshellx hosts add <host> --group <group> --transport native --native-port 9443 --san <san> -i fleet.ini` |
| **List** all devices | `pipeshellx hosts list -i fleet.ini` |
| **Disconnect** a device | `pipeshellx hosts remove <IP> -i fleet.ini` |
| **Run on all** devices | `pipeshellx run -i fleet.ini -- <command>` |
| **Run on one group** | `pipeshellx run -i fleet.ini -g <group> -- <command>` |
| **Run with JSON output** | `pipeshellx run -i fleet.ini --json -- <command>` |
| **Run with timeout** | `pipeshellx run -i fleet.ini --timeout 30 -- <command>` |
| **Run with audit trail** | `pipeshellx run -i fleet.ini --audit-log audit.jsonl -- <command>` |
| **Run with snapshots** | `pipeshellx run -i fleet.ini --snapshot-file snaps.jsonl -- <command>` |
| **View snapshot history** | `pipeshellx snapshot dump snaps.jsonl` |
| **View latest as JSON** | `pipeshellx snapshot dump snaps.jsonl --latest --json` |
| **Run a local pipeline** | `pipeshellx pipe "'echo hello' \| 'tr a-z A-Z'"` |
| **Import many devices** | `pipeshellx hosts import clients.txt -i fleet.ini` |
