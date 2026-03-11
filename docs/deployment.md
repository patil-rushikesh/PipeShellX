# Deployment Guide

## Supported Environment

The project targets a POSIX-style environment with:

- CMake 3.20 or newer
- a C++20 compiler
- POSIX process APIs such as `fork`, `pipe`, `dup2`, `poll`, and `waitpid`

Linux is the intended compatibility target. The project also builds in the current macOS-based development environment used during validation.

## Build Requirements

- C++20-capable compiler
- CMake
- Make
- pthread support through `Threads`

Optional:

- GoogleTest for unit tests

## Build Steps

From the project root:

```bash
mkdir -p build
cd build
cmake ..
make -j
```

The build produces:

- library target: `remote_command_lib`
- executable: `build/bin/remote_command_app`

## Compiler Settings

The project is configured to build with:

- `-std=c++20`
- `-Wall`
- `-Wextra`
- `-Werror`

These flags are enforced via the top-level CMake configuration.

## Running the Application

From the project root:

```bash
./build/bin/remote_command_app
```

Sample interactive session:

```text
remote-shell> pwd
remote-shell> whoami
remote-shell> echo hello
remote-shell> exit
```

## Logging

Logs are currently emitted to stdout unless `Logger::setLogFile(...)` is used from application code.

Each log line includes:

- timestamp
- log level
- PID
- session ID
- command

For production-style deployment, redirect stdout/stderr or add log-file initialization at startup.

## Operational Recommendations

### Run as a Dedicated User

Do not run the application as root. Use a dedicated low-privilege account.

### Restrict the Host Environment

Recommended:

- minimal filesystem permissions
- isolated runtime environment
- limited accessible binaries
- constrained environment variables

### Process Supervision

For service-style operation, use a supervisor such as:

- `systemd`
- container runtime
- process monitor

### Resource Controls

The application already applies hardcoded child CPU and memory limits. For real deployment:

- external cgroup/container limits are recommended
- make execution limits configurable per deployment

## Packaging Notes

Install rules are defined for:

- executable to `bin`
- library to `lib`
- headers to `include`

You can install with:

```bash
cmake --install build
```

## Known Deployment Constraints

- current terminal client is interactive-first, not a daemon or network service
- `SessionManager` exists but is not yet the primary deployment-facing orchestration layer
- logging configuration is minimal
- sandboxing is not strong enough yet for hostile multi-tenant environments
