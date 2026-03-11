# IPC Design

## IPC Model

The system uses unnamed POSIX pipes for communication between the parent process and each child process created for command execution.

Three pipes are created per command:

- stdin pipe: parent writes, child reads
- stdout pipe: child writes, parent reads
- stderr pipe: child writes, parent reads

These pipes are created in `ProcessManager::setupPipes()` in `src/process_manager.cpp`.

## Pipe Topology

For a single command execution:

```text
Parent stdinPipe[1]  ---->  stdinPipe[0] Child STDIN
Child stdoutPipe[1]  ---->  stdoutPipe[0] Parent
Child stderrPipe[1]  ---->  stderrPipe[0] Parent
```

After `fork()`:

- the child maps its pipe ends to standard file descriptors using `dup2()`
- the parent closes the ends it does not own
- the child closes the original inherited pipe descriptors after redirection

## Parent to Child Communication

The parent sends optional input through the write end of the stdin pipe.

Behavior:

- parent writes to `stdinPipe[1]`
- child receives data on `STDIN_FILENO` after `dup2(stdinPipe[0], STDIN_FILENO)`
- once input is fully written, parent closes `stdinPipe[1]` to signal EOF

The current terminal client does not provide user input forwarding to the child, so most commands run with an empty stdin stream.

## Child to Parent Communication

The child writes stdout and stderr normally through `STDOUT_FILENO` and `STDERR_FILENO`. Because these file descriptors are redirected to pipe write ends, the parent receives command output through:

- `stdoutPipe[0]`
- `stderrPipe[0]`

The parent reads both streams using nonblocking I/O and `poll()` to avoid deadlocks.

## Why `poll()` Is Used

The parent must read stdout and stderr while the child is still running.

If the parent waited for process exit before reading:

- the child could fill a pipe buffer
- the child would then block on write
- the parent would block waiting for process exit
- the command would deadlock

The current implementation avoids this by:

- making the parent-side pipe FDs nonblocking
- polling readable/writable events
- draining stdout and stderr incrementally

## FD Ownership Rules

### Parent Owns

- `stdinPipe[1]`
- `stdoutPipe[0]`
- `stderrPipe[0]`

### Child Owns Before `dup2()`

- `stdinPipe[0]`
- `stdoutPipe[1]`
- `stderrPipe[1]`

### Parent Closes

- `stdinPipe[0]`
- `stdoutPipe[1]`
- `stderrPipe[1]`

### Child Closes

After `dup2()`, the child closes all original pipe descriptors because standard input/output/error now refer to the redirected FDs.

## Stability Measures

The IPC path includes several protections:

- nonblocking reads and writes
- `poll()`-driven event loop
- retry on `EINTR`
- cleanup on exceptions
- timeout-based process group termination
- RAII-style descriptor cleanup in destructors and helper functions

## Logging Coverage

IPC-related logging now includes:

- pipe creation
- child creation
- stdin closure
- stdout bytes read
- stderr bytes read
- timeout events
- cleanup failures

Each log entry includes timestamp, PID, session ID, and command context.

## Standalone Pipe Utility

The repository also includes a reusable `Pipe` class in:

- `include/ipc_engine.h`
- `src/ipc_engine.cpp`

It demonstrates:

- RAII pipe closure
- nonblocking mode control
- optional logging support

This helper is currently not used by the active command execution path.

## Current Limitations

- The terminal-facing callback path is not true live streaming yet; output is still surfaced after process execution returns.
- IPC is robust for the tested command model, but commands with interactive stdin requirements are not a primary use case in the current interface.
