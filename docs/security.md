# Security Considerations

## Threat Model

This system executes operating system commands on behalf of a user. The main security risks are:

- command injection
- arbitrary executable launch
- path abuse
- unsafe shell expansion
- privilege misuse
- denial of service through hung or noisy child processes

## Current Security Controls

### No Shell Execution

The system does not use:

- `system()`
- `popen()`
- `/bin/sh -c`

Commands are executed directly through `execvp()` with an argument vector. This removes shell expansion behavior such as:

- pipes
- command chaining
- variable expansion
- redirection syntax
- subshell execution

### Command Allowlist

Only a fixed set of commands is allowed:

- `ls`
- `cat`
- `echo`
- `pwd`
- `whoami`
- `date`
- `uptime`
- `df`
- `du`
- `ps`
- `top`
- `id`

Any command outside this list is rejected.

### Trusted Executable Resolution

Executables are resolved only from trusted system directories:

- `/bin`
- `/usr/bin`

Explicit paths are rejected. This prevents:

- execution from the current working directory
- user-supplied relative path tricks
- arbitrary path-based binary execution

### Argument Filtering

Arguments are length-bounded and rejected if they contain unsafe shell-like metacharacters such as:

- `;`
- `&`
- `|`
- `` ` ``
- `$`
- `<`
- `>`
- `\`

This is a defense-in-depth layer. The main protection remains the no-shell execution model.

### Process Group Control

Each child is placed into its own process group. On timeout, the system kills the entire group rather than only the direct child. This reduces the risk of runaway descendant processes.

### Resource Limits

Child processes are constrained with:

- CPU limit
- address space limit

These limits reduce impact from abusive or malformed commands, although the values are currently hardcoded.

## Remaining Risks

### Allowlisted Command Scope

Some allowed commands expose system information:

- `ps`
- `id`
- `df`
- `du`
- `top`

These are not injection risks, but they are policy risks. In a more restricted deployment, the allowlist should be narrower.

### `cat` Risk

`cat` can read files available to the running user. If the execution environment contains sensitive readable files, this is a disclosure risk.

### Environment Inheritance

The child inherits the parent environment. Although executable resolution is constrained, environment variables can still affect subprocess behavior in other ways.

### No User Separation

The current application assumes the permissions of the user running `remote_command_app`. It does not implement:

- privilege dropping
- chroot jail
- namespaces
- seccomp filtering
- per-session OS account separation

## Recommended Hardening

For stronger deployment:

- run the service under a dedicated low-privilege user
- remove high-risk commands from the allowlist
- make resource limits configurable
- add seccomp or platform sandboxing
- restrict accessible filesystem locations
- consider namespaces or container isolation
- log to a file or centralized sink instead of only stdout

## Security Posture Summary

The current system is substantially safer than a shell-backed executor because it:

- avoids shell invocation
- restricts commands by allowlist
- resolves executables from trusted directories
- validates argument content
- constrains process execution

It is appropriate as a controlled teaching/demo system. It should not be treated as a hardened multi-tenant remote execution service without additional sandboxing and policy controls.
