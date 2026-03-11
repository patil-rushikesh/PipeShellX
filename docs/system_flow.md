# System Execution Flow

## End-to-End Flow

This document describes what happens from user command entry to child process completion.

## Interactive Flow

### 1. Application Startup

`main()`:

- logs startup
- constructs `TerminalClient`
- enters the interactive loop

### 2. Prompt and Input

`TerminalClient::run()`:

- prints `remote-shell>`
- reads a line from stdin
- handles `exit`, `quit`, or `history`
- forwards other commands for execution

### 3. Session Context Creation

For interactive usage, the terminal client derives a synthetic session ID:

```text
interactive-<pid>
```

This is used for logging and tracing even though the terminal path does not currently use `SessionManager`.

### 4. Command Execution Request

`TerminalClient::handleCommand()`:

- stores the command in history
- constructs `CommandExecutor`
- passes the raw command string, session ID, callback, and timeout

### 5. Parsing and Validation

`CommandExecutor::execute()`:

- parses the command string into arguments
- validates the command name and arguments
- checks the allowlist
- rejects explicit paths
- resolves the executable from trusted system directories
- logs the validated command

### 6. Process Launch

`CommandExecutor::runCommand()`:

- creates `ProcessManager`
- logs start of execution
- calls `ProcessManager::execute(...)`

### 7. IPC and Child Execution

`ProcessManager::execute()`:

- creates pipes
- forks
- configures the child
- calls `execvp()`
- monitors stdout/stderr using `poll()`
- reaps the child with `waitpid()`
- returns output, stderr, exit code, and timeout status

### 8. Output Handling

The returned output is split into lines and passed to the terminal callback:

- stdout lines are printed normally
- stderr lines are printed in red

### 9. Completion

`TerminalClient`:

- reports non-zero exit codes as user-visible errors
- returns to the prompt

## SessionManager Flow

When `SessionManager` is used:

1. `startSession()` creates a session object
2. a unique session ID is generated
3. a worker thread is launched
4. the worker calls `CommandExecutor::execute(...)`
5. output, error, and exit code are stored in the session
6. `active` is set to false on completion

## Logging Flow

Execution logs now exist at four levels:

- startup and terminal-level failures
- command validation and dispatch
- process creation and reaping
- IPC activity and error paths

Every log line includes:

- timestamp
- log level
- PID
- session ID
- command

## Control Flow Summary

```text
User input
  -> TerminalClient
  -> CommandExecutor
  -> ProcessManager
  -> fork()
     -> Child: dup2 + execvp
     -> Parent: poll + read pipes + waitpid
  -> CommandResult
  -> Terminal output
```

## Error Flow

Typical failure points:

- parse error
- validation failure
- missing executable
- pipe creation failure
- fork failure
- `dup2()` failure
- `execvp()` failure
- timeout
- `waitpid()` or `poll()` failure

Current behavior:

- low-level failures become exceptions in the parent
- child setup failures write to stderr and exit
- terminal-level failures are shown to the user
- all major failure paths are logged
