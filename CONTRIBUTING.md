# Contributing to PipeShellX

Thank you for improving PipeShellX. Contributions should keep command
execution predictable, portable, testable, and honest about its trust and
platform boundaries.

## Find and scope work

Search the [open issues](https://github.com/patil-rushikesh/PipeShellX/issues)
and [pull requests](https://github.com/patil-rushikesh/PipeShellX/pulls) before
starting. Work selected for new contributors is labeled
[`good first issue`](https://github.com/patil-rushikesh/PipeShellX/issues?q=is%3Aissue%20is%3Aopen%20label%3A%22good%20first%20issue%22);
tasks needing contributors are labeled
[`help wanted`](https://github.com/patil-rushikesh/PipeShellX/issues?q=is%3Aissue%20is%3Aopen%20label%3A%22help%20wanted%22).
Comment on an issue before starting so work is not duplicated.

Use [GitHub Discussions](https://github.com/patil-rushikesh/PipeShellX/discussions)
for setup help, usage questions, and early design conversations. Keep issues
focused on reproducible bugs or concrete, actionable work.

The [roadmap](ROADMAP.md) contains broad goals, not ready-to-implement tasks.
For a feature, public API or protocol change, new dependency, platform claim,
or other substantial change, [open an issue](https://github.com/patil-rushikesh/PipeShellX/issues/new/choose)
and agree on a focused scope first. Obvious bug fixes, tests, and small
documentation corrections may go directly to a pull request. Suspected
vulnerabilities are the exception: report them privately as described in
[SECURITY.md](SECURITY.md).

## Set up a development build

You need CMake 3.20 or newer, a C++20 compiler, and Ninja or Make. The default
build also needs OpenSSL 3. CMake uses an installed GoogleTest when available
or fetches v1.17.0 during configuration; use `PIPESHELLX_SYSTEM_GTEST=ON` for
a strictly system-provided dependency.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/pipeshellx --help
```

Useful CMake options:

| Option | Default | Purpose |
| --- | ---: | --- |
| `PIPESHELLX_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `PIPESHELLX_BUILD_BENCH` | `ON` | Build the baseline benchmark. |
| `PIPESHELLX_WERROR` | `ON` | Treat first-party warnings as errors. |
| `PIPESHELLX_NATIVE_TRANSPORT` | `ON` | Build OpenSSL-backed native transport, CA, and node support. |
| `PIPESHELLX_STATIC_OPENSSL` | `OFF` | Prefer static OpenSSL libraries; this is not a fully-static guarantee. |
| `PIPESHELLX_SYSTEM_GTEST` | `OFF` | Require an installed GoogleTest rather than fetching it. |
| `PIPESHELLX_SANITIZE` | empty | Enable a sanitizer list such as `address,undefined`. |

For an SSH-only build without OpenSSL discovery:

```bash
cmake -S . -B build-ssh \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
ctest --test-dir build-ssh --output-on-failure
```

See [testing](docs/testing.md) for sanitizer, focused-test, soak, packaging,
and CI commands.

## Make changes

Create a topic branch from the latest `main`; do not develop directly on the
protected branch. Keep each branch and pull request focused on one problem.

- Add or update a focused regression test for behavior changes. Tests must not
  depend on the caller's working directory, environment, real fleet, or
  external network.
- Use helpers in `tests/test_support.hpp` for temporary directories and scoped
  environment changes.
- Keep production code warning-clean under `-Wall -Wextra -Werror` or
  `/W4 /WX`.
- Run `clang-format -i` on files you touch. Do not reformat unrelated code.
- Run `clang-tidy -p build <file>` for materially changed C++ files and fix or
  explain new findings.
- Update public documentation whenever a command, option, output schema,
  security boundary, platform claim, or packaging contract changes.
- Update `THIRD_PARTY_NOTICES.md` when adding or changing a distributed
  dependency, and verify that its license is compatible with Apache-2.0.
- Pin third-party GitHub Actions to immutable commit SHAs and run `actionlint`
  when changing workflows.

## Preserve layering and portability

Dependencies point down from CLI and orchestration through transports,
runtime, and the OS abstraction. In particular:

- headers under `include/psx/` expose standard C++ types rather than native
  file descriptors, sockets, or platform headers;
- POSIX platform headers and implementations stay under `src/os/posix/`;
- higher layers use the OS abstraction instead of calling platform APIs
  directly;
- new resources have explicit ownership, cleanup, cancellation, and error
  paths;
- Linux and macOS behavior must remain equivalent where the feature is
  documented as supported.

Run the repository checks after relevant changes:

```bash
./scripts/check_layering.sh
./scripts/check_docs.py
```

## Validate before opening a pull request

At minimum:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 120
./scripts/check_layering.sh
./scripts/check_docs.py
./build/bin/pipeshellx --version
./build/bin/pipeshellx --help
git diff --check
```

For process, pipe, socket, TLS, cancellation, or concurrency changes, also run
ASan and UBSan:

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPIPESHELLX_SANITIZE=address,undefined
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Hosted CI repeats the suite across Linux GCC, Linux Clang, and macOS
AppleClang, validates installation and the downstream CMake package, exercises
the SSH-only build, and runs ASan+UBSan. The protected `main` branch requires
the aggregate `Required CI` check and CodeQL's `Analyze C/C++` check to pass on
the latest commit. Every update to `main` must use a pull request, and all
review threads must be resolved before merge.

## Open a pull request

Push your topic branch to your fork and open a pull request against `main`.
Complete the pull-request template, link the issue with `Closes #123` when
appropriate, and include the exact commands and platforms you tested. Draft
pull requests are welcome when early implementation feedback would prevent
wasted work.

Pull requests should be narrow enough to review and explain the user impact,
supported platforms, security or compatibility implications, and any known
limitation. Before submitting, verify that:

- tests cover the changed behavior and pass locally;
- touched files are formatted and warning-clean;
- layering and sanitizer checks relevant to the change pass;
- documentation and, for user-visible changes, changelog entries match the
  implementation;
- the diff contains no credentials, private hostnames, audit output, generated
  inventories, build artifacts, or local absolute paths.

Conventional Commit subjects are preferred, for example:

```text
fix(transport): preserve stderr channel attribution
```

Keep review discussion attached to the pull request and add follow-up commits
instead of hiding review history. A maintainer merges only after review is
resolved and the required checks pass.

## Security reports

Do not disclose suspected vulnerabilities in a public pull request or issue.
Follow [SECURITY.md](SECURITY.md) for the current private-reporting procedure
and supported versions.

## Respectful conduct

Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

By submitting a contribution, you agree that it may be distributed under the
repository's Apache License 2.0.
