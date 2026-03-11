# Process Management

## Overview

Process lifecycle control is implemented in `ProcessManager`.

Primary responsibilities:

- create child processes with `fork()`
- isolate child stdio using pipes and `dup2()`
- execute commands using `execvp()`
- monitor child execution
- enforce time limits
- reap children using `waitpid()`
- prevent zombie processes

## Lifecycle Steps

### 1. Validate Inputs

`ProcessManager::execute()` requires a non-empty argument vector. Command parsing and validation occur earlier in `CommandExecutor`.

### 2. Create Pipes

`setupPipes()` creates three unnamed pipes for stdin, stdout, and stderr.

### 3. Fork

`fork()` splits execution into:

- parent branch
- child branch

If `fork()` fails:

- pipes are closed
- the error is logged
- an exception is thrown

### 4. Child Initialization

In the child:

- `setpgid(0, 0)` creates a separate process group
- `dup2()` maps pipe ends onto `STDIN_FILENO`, `STDOUT_FILENO`, and `STDERR_FILENO`
- original pipe descriptors are closed
- resource limits are applied with `setrlimit()`
- `execvp()` replaces the child image with the target executable

If any child-side setup step fails:

- the child writes an error to stderr
- exits immediately with `_exit(...)`

## Why `_exit()` Is Used

The child must not unwind C++ state inherited from the parent after `fork()`.

Using `_exit()` instead of `exit()` avoids:

- double flushing stdio buffers
- invoking parent-owned cleanup code
- undefined behavior in partially copied runtime state

## Parent Execution Loop

In the parent:

- child-side pipe ends are closed immediately
- parent-side pipe FDs are set nonblocking
- `poll()` waits for pipe readiness
- stdout/stderr are drained while the child runs
- `waitpid(..., WNOHANG)` checks for child termination without blocking

This design prevents both deadlocks and zombie accumulation during normal execution.

## Timeout Handling

If a timeout is configured and exceeded:

- the parent sends `SIGKILL` to the child process group using `kill(-childPid, SIGKILL)`

Using the process group rather than only the direct child helps clean up subprocesses spawned by the command.

## Reaping Strategy

Zombie prevention relies on:

- `waitpid(..., WNOHANG)` during the event loop
- synchronous cleanup in exceptional paths
- destructor cleanup via `reapChild(true)` if a child is still active

The implementation also installs a `SIGCHLD` handler once per process so child termination interrupts blocking operations promptly.

## Session-Level Process Management

`SessionManager` adds thread-level lifecycle tracking around command execution:

- creates a session record
- launches a worker thread
- stores output, error, and exit code
- joins threads during shutdown or explicit session end

This is session orchestration, not direct OS process control. The actual child lifecycle remains owned by `ProcessManager`.

## Safety Properties

Current process management explicitly avoids:

- shell execution through `system()`
- leaving active child descriptors open after execution
- waiting for child exit before draining output
- ignoring child cleanup during exception paths

## Remaining Gaps

- The system does not yet expose explicit user-facing cancellation controls.
- Resource limits are hardcoded rather than configurable.
- Session management does not currently expose child PID ownership in a way that supports external supervision or advanced orchestration.
