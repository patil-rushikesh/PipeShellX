# Distributed Execution

## Overview

The project now supports distributed command execution across multiple remote machines over SSH.

When a `clients.txt` file is present in the working directory, the command execution path switches from local execution to remote fan-out execution:

- one SSH worker process is created per configured client
- all workers run in parallel
- stdout and stderr are captured independently for each client
- results are returned to the parent and displayed grouped by client

If `clients.txt` is not present, the system continues to use the original local execution path.

## Client Configuration

The client list is loaded from:

```text
clients.txt
```

Example:

```text
user@192.168.1.10
user@192.168.1.11
user@192.168.1.12
```

The configuration loader:

- reads one client per line
- ignores blank lines
- ignores `#` comments
- validates the `user@host` format
- rejects duplicate entries

Implementation:

- `include/client_config.hpp`
- `src/client_config.cpp`

## SSH Execution Model

For a command entered at the terminal:

```text
cmd> whoami
```

the distributed execution layer transforms it into one SSH command per client:

```text
ssh user@192.168.1.10 'whoami'
ssh user@192.168.1.11 'whoami'
ssh user@192.168.1.12 'whoami'
```

Each SSH invocation is executed directly through `execvp()` in a child worker process. The implementation does not invoke a shell locally.

The SSH worker currently uses:

- `/usr/bin/ssh`
- `-o BatchMode=yes`
- `-o StrictHostKeyChecking=no`

This makes the execution non-interactive and suitable for scripted fan-out behavior.

## Multi-Client Architecture

Distributed execution extends the existing architecture rather than replacing it.

### Existing Flow

```text
TerminalClient
  -> CommandExecutor
  -> ProcessManager
```

### Distributed Flow

```text
TerminalClient
  -> CommandExecutor
      -> ClientConfig loads clients.txt
      -> ProcessManager::executeRemote(...)
          -> fork one SSH worker per client
          -> collect output with pipes
          -> waitpid on each worker
```

### Responsibilities

#### CommandExecutor

- parses and validates the entered command
- loads `clients.txt` if present
- builds the remote SSH command payload
- decides whether execution is local or distributed

#### ProcessManager

- forks one worker per client
- creates per-worker pipes
- redirects worker stdout/stderr
- executes SSH through `execvp()`
- captures and classifies client errors
- reaps worker processes

## IPC Integration

Distributed execution reuses the same OS concepts already present in the local command runner.

For each client worker:

- one stdout pipe is created
- one stderr pipe is created
- the worker redirects its output with `dup2()`
- the parent reads outputs using nonblocking I/O and `poll()`

This means the parent can collect output from multiple remote clients concurrently without serial blocking.

### Per-Client Pipe Ownership

Parent owns:

- worker stdout read end
- worker stderr read end

Child owns:

- worker stdout write end
- worker stderr write end

After redirection:

- child closes inherited descriptors
- parent closes write ends

## Parallel Execution Flow

The parent process does the following:

1. load all configured clients
2. for each client:
   - create stdout/stderr pipes
   - `fork()` a worker
3. child worker:
   - create its own process group
   - redirect stdout/stderr
   - run SSH using `execvp()`
4. parent:
   - poll all worker pipes
   - read remote stdout/stderr as it becomes available
   - call `waitpid(..., WNOHANG)` for each worker
   - build per-client results

This ensures all remote commands start in parallel rather than one after another.

## Output Grouping

Results are displayed grouped by client:

```text
CLIENT user@192.168.1.10
ubuntu-node

CLIENT user@192.168.1.11
dev-server

CLIENT user@192.168.1.12
laptop
```

On failure:

```text
CLIENT user@192.168.1.10
ERROR: connection failed
```

## Error Handling

The remote execution path classifies common SSH failures into normalized client-scoped messages:

- `ERROR: connection failed`
- `ERROR: unreachable host`
- `ERROR: authentication failed`
- `ERROR: command timed out`
- `ERROR: command failed with exit code <n>`

Raw SSH stderr is still captured internally for diagnostics and logging.

## Logging

Distributed execution logs:

- command entered
- remote worker creation
- remote stdout/stderr reads
- SSH worker reaping
- normalized client errors

Each log line includes:

- timestamp
- PID
- session ID
- client ID
- command

## Current Constraints

- Distributed execution activates only when `clients.txt` exists in the current working directory.
- The command allowlist still applies before any SSH fan-out happens.
- Output is grouped after collection, not streamed in real time to the terminal by client.
- The implementation assumes SSH connectivity and key-based or otherwise non-interactive authentication.

## Summary

The distributed execution layer preserves the original architecture while extending it from single-host command execution to multi-client SSH-based fan-out. It keeps process creation, pipe-based IPC, and child reaping inside `ProcessManager`, which makes the extension consistent with the rest of the system.
