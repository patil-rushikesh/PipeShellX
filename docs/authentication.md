# Authentication Guide

## Overview

PipeShellX uses the system OpenSSH client for remote execution.

Depending on the client and the current session state, authentication is handled in one of two ways:

- plain `ssh` for OpenSSH-managed authentication
- `sshpass -p <password> ssh ...` for password-backed clients added interactively

PipeShellX does not implement its own SSH protocol stack or credential exchange.

## Supported Authentication Methods

### Key-Based Authentication

Key-based authentication works through the local OpenSSH client.

If the remote host accepts the user’s SSH keys, PipeShellX simply executes:

```text
ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 user@host "command"
```

This covers:

- default private keys in `~/.ssh/`
- explicit identity files stored in client configuration
- host-specific key selection configured by OpenSSH

### Password Authentication

Password authentication is supported through `sshpass`, but the password must be supplied through the interactive shell.

Example:

```text
PipeShell > add-client user@192.168.1.10
Password required? (y/n) y
Enter password:
```

After the password is captured, the remote worker executes:

```text
sshpass -p <password> ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 user@host "command"
```

The password is stored only in memory for the lifetime of the running PipeShellX process.

### ssh-agent Authentication

If the user has a running `ssh-agent` and the correct key is loaded, PipeShellX works without any additional application-side configuration.

OpenSSH discovers and uses the agent automatically when the worker runs `ssh`.

### ~/.ssh/config Support

PipeShellX also inherits OpenSSH behavior from `~/.ssh/config`.

This means the following can be managed outside the application:

- per-host usernames
- identity files
- proxy or jump-host rules
- agent preferences
- host aliases
- other OpenSSH connection settings

If `~/.ssh/config` already allows a host to be reached with `ssh my-host`, PipeShellX benefits from the same OpenSSH resolution and authentication behavior when it invokes `ssh`.

## Client Configuration Behavior

Persistent client configuration in `clients.txt` supports:

- `user@host`
- `ssh://user@host`
- `ssh://user@host:port?identity=/path/to/key`

Passwords are intentionally not allowed in `clients.txt`.

If a configuration entry contains `password=...`, the loader rejects it. This prevents password persistence on disk.

## Verification and Status Checks

When a client is added, PipeShellX verifies connectivity using the same authentication method currently attached to that client:

- key / agent / SSH config: plain `ssh`
- interactive password: `sshpass + ssh`

The verification command is:

```text
echo connected
```

If stdout contains `connected`, the client is marked `ONLINE`. Otherwise it is marked `OFFLINE`.

The `status` command uses the same client-scoped authentication path.

## Authentication Failure Reporting

If SSH authentication fails, PipeShellX normalizes the output to:

```text
CLIENT user@host
ERROR: authentication failed
```

This applies to both connection verification and remote command execution.

## Security Notes

Current security properties:

- passwords entered interactively are stored in memory only
- passwords are not written to `clients.txt`
- passwords are not printed to the terminal
- passwords are not included in application log messages

Current limitation:

- `sshpass -p <password>` places the password in the child process argument vector while that process exists

That is acceptable for the current implementation scope, but it is weaker than file-descriptor or environment-based secret passing. If stronger process-level secrecy is required, the `sshpass` invocation strategy should be upgraded.
