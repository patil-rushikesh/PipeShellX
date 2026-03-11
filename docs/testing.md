# Testing Strategy

## Current Test Surface

The repository includes:

- `tests/test_ipc.cpp`
- `tests/test_prcoess_manager.cpp`

These cover basic pipe behavior and basic process execution behavior. Test build is conditional on GoogleTest availability.

## Existing Validation Areas

### IPC Tests

Current coverage includes:

- write/read correctness
- nonblocking behavior
- basic error handling

### Process Tests

Current coverage includes:

- valid command execution
- invalid command execution
- timeout behavior

## Gaps in Automated Testing

Automated coverage is still missing for:

- `CommandExecutor` validation behavior
- trusted executable resolution
- `SessionManager`
- logging behavior
- high-volume stress execution
- concurrent session execution
- zombie-process regression tests
- interactive terminal behavior

## Manual Validation Already Performed

The system has been validated with:

- clean rebuild from scratch
- functional execution of:
  - `ls`
  - `pwd`
  - `whoami`
  - `echo hello`
  - `date`
- 125-command single-session soak test
- 4 concurrent sessions with 120 commands each
- post-run zombie inspection

These checks confirmed:

- stable IPC behavior
- correct process reaping
- no observed defunct child processes after validation
- no reported runtime errors during the soak and concurrency runs

## Recommended Test Categories

### Unit Tests

Add tests for:

- command parser behavior
- allowlist enforcement
- unsafe argument rejection
- executable path resolution
- logger formatting

### Integration Tests

Add tests that execute the full stack:

- terminal client to command executor
- command executor to process manager
- stdout/stderr capture correctness
- non-zero exit handling

### Concurrency Tests

Add repeatable automated tests for:

- many parallel command sessions
- repeated session create/end cycles
- interleaved process completions
- thread-safe logging under load

### Lifecycle and Reliability Tests

Add targeted tests for:

- timeout kills process group
- child reaping under fast exit conditions
- large stdout/stderr output without deadlock
- broken pipe handling

### Regression Tests

There was a previously fixed bug where the parent could continue looping after reaping a child and accidentally call `waitpid(-1, ...)`. Add a regression test for this exact path.

## Running Tests

If GoogleTest is available:

```bash
mkdir -p build
cd build
cmake ..
make -j
ctest --output-on-failure
```

If GoogleTest is not installed, the test target is skipped by CMake.

## Production-Readiness Testing Recommendations

Before treating the system as operationally mature, add:

- CI execution of unit and integration tests
- stress tests with larger command counts
- failure-injection tests around `fork`, `pipe`, and `poll`
- platform validation on target Linux environments
- log verification tests

## Success Criteria

A stronger test suite should prove:

- commands are validated correctly
- IPC never deadlocks under expected workloads
- child processes are always reaped
- concurrent sessions remain isolated
- no FD leaks appear across repeated executions
- security controls reject unsupported input consistently
