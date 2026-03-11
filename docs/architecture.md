# Architecture Overview

## Purpose

This project is a remote command execution system built to demonstrate core operating system concepts:

- process creation with `fork()`
- command execution with `execvp()`
- parent/child communication with unnamed pipes
- I/O redirection with `dup2()`
- multi-session behavior through terminal-based usage

The current system is implemented as a small layered C++ application with a terminal client on top of a command execution and process-management core.

## High-Level Layers

### Entry Point

- `src/main.cpp`

Starts the application, initializes logging, and launches the terminal interface.

### User Interface Layer

- `include/terminal_client.hpp`
- `src/terminal_client.cpp`

Handles interactive command input, command history, colored output, and user-visible error reporting.

### Command Execution Layer

- `include/command_executor.hpp`
- `src/command_executor.cpp`

Responsible for:

- parsing user input into arguments
- validating commands against a strict allowlist
- resolving executables from trusted directories
- creating execution context for logging
- delegating process execution to `ProcessManager`

### Process Management Layer

- `include/process_manager.hpp`
- `src/process_manager.cpp`

Responsible for:

- creating stdin/stdout/stderr pipes
- calling `fork()`
- wiring child process stdio with `dup2()`
- executing the target program with `execvp()`
- draining child output using nonblocking I/O and `poll()`
- enforcing timeouts and killing process groups on failure
- reaping children with `waitpid()`

### Session Layer

- `include/session_manager.hpp`
- `src/session_manager.cpp`

Provides asynchronous session tracking on top of `CommandExecutor`. Each session owns:

- a session ID
- worker thread
- captured output
- captured error
- exit code
- active state

This layer is prepared for multi-session orchestration but is not yet the primary path used by the interactive client.

### IPC Utility Layer

- `include/ipc_engine.h`
- `src/ipc_engine.cpp`

Contains a standalone `Pipe` abstraction for unnamed pipe management. It demonstrates RAII pipe ownership but is currently separate from the active `ProcessManager` execution path.

### Logging Layer

- `include/logger.hpp`
- `src/logger.cpp`

Provides centralized logging with:

- timestamp
- log level
- PID
- session ID
- executed command

## Module Responsibilities

### TerminalClient

- interactive shell behavior
- history display
- command dispatch
- rendering stdout/stderr

### CommandExecutor

- input parsing
- command validation
- executable resolution
- execution audit logging

### ProcessManager

- IPC creation and cleanup
- process lifecycle control
- timeout handling
- child reaping
- low-level execution reliability

### SessionManager

- background session tracking
- thread ownership
- session result retention

### Logger

- serialized output
- execution observability

## Dependency Graph

```text
main
  -> Logger
  -> TerminalClient
       -> CommandExecutor
            -> ProcessManager
                 -> fork
                 -> pipe
                 -> dup2
                 -> execvp
                 -> poll
                 -> waitpid

SessionManager
  -> CommandExecutor
       -> ProcessManager

Pipe
  -> pipe
  -> read/write
  -> fcntl
```

## Current Design Notes

- The runtime command path does not use a shell. This is intentional and reduces injection risk.
- `ProcessManager` uses direct POSIX primitives instead of the `Pipe` helper. That keeps the active execution path explicit, but it also means there are two IPC abstractions in the repository.
- Logging is now execution-aware and includes process/session context across the major layers.

## Known Architectural Limitations

- The terminal client still behaves synchronously from the user’s perspective even though it uses a worker thread.
- `SessionManager` is not yet integrated as the primary orchestration path for interactive execution.
- Output callbacks are still invoked after command completion rather than as truly live streamed events.
