# PipeShellX

## Description

PipeShellX is a C++ systems programming project that demonstrates how operating system primitives can be used to build a controlled command execution workflow.

The project focuses on low-level process execution using:

- `fork()` for process creation
- unnamed pipes for IPC
- `dup2()` for standard stream redirection
- `execvp()` for command execution
- `waitpid()` for process lifecycle management

It provides an interactive terminal client, structured logging, command validation, and a process manager designed to avoid deadlocks and zombie processes.

The project also supports distributed execution over SSH when a `clients.txt` file is present. In that mode, a command entered once in the terminal is executed in parallel across all configured remote clients.

## Key Features

- Interactive terminal-based command execution
- Command allowlist with input validation
- Safe execution without shell invocation
- Parent/child IPC using stdin, stdout, and stderr pipes
- Nonblocking pipe handling with `poll()`
- Timeout-aware process execution
- Process group cleanup on timeout or failure
- Structured logging with timestamp, PID, session ID, and command
- Session management abstraction for concurrent execution tracking
- Distributed SSH execution across multiple configured clients
- Per-client grouped output and normalized remote error reporting

## Operating System Concepts Demonstrated

- Process creation with `fork()`
- Program replacement with `execvp()`
- Inter-process communication with unnamed pipes
- File descriptor duplication with `dup2()`
- Nonblocking I/O with `fcntl()`
- Event-driven pipe monitoring with `poll()`
- Child reaping with `waitpid()`
- Signal handling with `SIGCHLD`
- Resource limiting with `setrlimit()`
- Process-group based termination with `kill()`

## Architecture Overview

The system is organized into a small set of focused modules:

- `TerminalClient`
  Handles user interaction, command history, colored output, and terminal-facing errors.
- `CommandExecutor`
  Parses commands, validates user input, resolves executables from trusted paths, and prepares execution context.
- `ProcessManager`
  Creates pipes, forks child processes, redirects stdio, executes commands, collects output, reaps child processes, and runs parallel SSH workers for distributed execution.
- `SessionManager`
  Tracks background command sessions using worker threads and per-session state.
- `Logger`
  Provides structured logs for command execution, process lifecycle, IPC activity, and failures.
- `ClientConfig`
  Loads and validates `clients.txt` for distributed SSH execution.
- `Pipe`
  A reusable RAII pipe abstraction included as a supporting IPC utility.

For more detail, see:

- [docs/architecture.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/architecture.md)
- [docs/distributed_execution.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/distributed_execution.md)
- [docs/ipc_design.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/ipc_design.md)
- [docs/process_management.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/process_management.md)

## Installation

### Prerequisites

- CMake 3.20 or newer
- C++20-capable compiler
- Make
- POSIX-compatible environment

Optional:

- GoogleTest for unit tests

### Build

```bash
mkdir -p build
cd build
cmake ..
make -j
```

The executable will be generated at:

```bash
build/bin/PipeShellX
```

## Usage

Run the interactive client:

```bash
./build/bin/PipeShellX
```

At the prompt, enter an allowed command:

```text
cmd> pwd
cmd> whoami
cmd> echo hello
cmd> date
cmd> exit
```

### Built-in Terminal Commands

- `history` shows command history
- `exit` or `quit` terminates the application

## Distributed Usage

Create a `clients.txt` file in the directory where you run the application:

```text
user@192.168.1.10
user@192.168.1.11
user@192.168.1.12
```

Then run the shell normally:

```bash
./build/bin/PipeShellX
```

When `clients.txt` is present, an entered command is executed on all configured clients in parallel over SSH.

Example:

```text
cmd> whoami
CLIENT user@192.168.1.10
ubuntu

CLIENT user@192.168.1.11
devuser

CLIENT user@192.168.1.12
laptop
```

Example failure output:

```text
cmd> hostname
CLIENT user@192.168.1.10
ERROR: connection failed
```

## Example Commands

The current allowlist includes:

- `ls`
- `cat`
- `echo`
- `pwd`
- `whoami`
- `date`
- `hostname`
- `uptime`
- `df`
- `du`
- `ps`
- `top`
- `id`

Example session:

```text
cmd> ls
cmd> pwd
cmd> whoami
cmd> echo hello
cmd> date
```

Distributed example:

```text
cmd> uptime
cmd> hostname
cmd> df -h
```

## Project Directory Structure

```text
.
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── deployment.md
│   ├── distributed_execution.md
│   ├── ipc_design.md
│   ├── process_management.md
│   ├── security.md
│   ├── system_flow.md
│   └── testing.md
├── include/
│   ├── client_config.hpp
│   ├── command_executor.hpp
│   ├── ipc_engine.h
│   ├── logger.hpp
│   ├── process_manager.hpp
│   ├── session_manager.hpp
│   └── terminal_client.hpp
├── src/
│   ├── CMakeLists.txt
│   ├── client_config.cpp
│   ├── command_executor.cpp
│   ├── ipc_engine.cpp
│   ├── logger.cpp
│   ├── main.cpp
│   ├── process_manager.cpp
│   ├── session_manager.cpp
│   └── terminal_client.cpp
└── tests/
    ├── CMakeLists.txt
    ├── test_ipc.cpp
    └── test_prcoess_manager.cpp
```

## Future Improvements

- True live streaming output to the terminal callback
- Deeper integration of `SessionManager` into the interactive runtime path
- Configurable command policy and resource limits
- Stronger sandboxing with seccomp, namespaces, or containers
- Expanded automated tests for concurrency, stress, and failure injection
- Dedicated log-file configuration and log rotation
- Better support for production-style service deployment
- Configurable client configuration path instead of fixed `clients.txt`
- True live per-client streaming output for distributed execution

## Additional Documentation

- [docs/system_flow.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/system_flow.md)
- [docs/distributed_execution.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/distributed_execution.md)
- [docs/security.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/security.md)
- [docs/deployment.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/deployment.md)
- [docs/testing.md](/Users/admin/rushikesh/Academics/operating_system/assignments/application/docs/testing.md)

## License

No license file is currently included in this repository.

If this project is intended for public distribution, add a standard license such as MIT, BSD-3-Clause, or Apache-2.0 and update this section accordingly.
