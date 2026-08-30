# PipeShellX

![CI](https://github.com/patil-rushikesh/PipeShellX/actions/workflows/ci.yml/badge.svg)
![CodeQL](https://github.com/patil-rushikesh/PipeShellX/actions/workflows/codeql.yml/badge.svg)

PipeShellX is a C++20 command-execution controller for Linux and macOS. It
fans commands out over OpenSSH or an optional mutual-TLS transport, runs local
process graphs, and can operate a native node agent from the same
`pipeshellx` executable.

The current `main` branch identifies as **0.6.0**, but no version tag or
release artifact has been published on GitHub. Treat `main` as the v0.6
release candidate, not as a published release.

> **Security warning:** PipeShellX is a remote-code-execution tool for trusted
> operators. `run` is unrestricted unless an optional controller policy is
> supplied, and an authenticated native controller may request arbitrary argv
> unless the node has its own policy. PipeShellX is not a multi-tenant sandbox.
> Read the [security policy and trust model](SECURITY.md) before deployment.

## Capabilities

- bounded-concurrency SSH or native fan-out with grouped, streaming, ordered,
  JSON, and consensus output;
- INI inventories with groups, tags, explicit host selection, per-host
  transport, and atomic `hosts add|remove|import` operations;
- a TLS 1.3 native transport with CA-issued identities, SAN authorization,
  optional CRLs, liveness, flow-control credit, and connection-loss fencing;
- optional controller-side and node-side command policies;
- local fan-in/fan-out DAGs plus linear mixed local/native pipelines;
- timeout, cancellation, fail-fast, idempotent SSH retries, native canaries,
  drop-aware capture, disk spooling, and opt-in JSONL audit records;
- an installable executable, library, headers, and relocatable CMake package.

## Platform support

| Role | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Controller | Supported | Supported | Not implemented |
| Native node | Supported | Supported | Not implemented |
| SSH target | Supported | Supported | Supported through Win32-OpenSSH from a POSIX controller |

The CI matrix builds Linux with GCC and Clang and macOS with AppleClang in
Debug and Release modes. Windows has no controller or native-node build and no
Windows CI job. For Windows SSH targets, select `--shell cmd` or
`--shell powershell`; target-shell quoting is not a sandbox.

## Build and install

Requirements are CMake 3.20 or newer, a C++20 compiler, and Ninja or Make.
The default native-enabled build also requires OpenSSL 3. Tests are enabled by
default and use an installed GoogleTest or fetch the immutable v1.17.0 source
commit during configuration.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake --install build --prefix /tmp/pipeshellx-prefix
/tmp/pipeshellx-prefix/bin/pipeshellx --version
```

For an SSH-only build that neither discovers nor links OpenSSL:

```bash
cmake -S . -B build-ssh \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIPESHELLX_NATIVE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
cmake --build build-ssh --parallel
```

Release builders can disable tests and benchmarks with
`-DPIPESHELLX_BUILD_TESTS=OFF -DPIPESHELLX_BUILD_BENCH=OFF` and can request
static OpenSSL preference with `-DPIPESHELLX_STATIC_OPENSSL=ON`. That option
does not guarantee one fully static executable on every platform. See
[deployment](docs/deployment.md) for the complete build and packaging
contract.

Installed CMake consumers use:

```cmake
find_package(pipeshellx 0.6 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE pipeshellx::lib)
```

## Release candidates

The release workflow builds and consumes three native-enabled archives:

- `linux-x86_64`;
- `macos-x86_64`; and
- `macos-arm64`.

Each candidate includes SHA-256 checksums, an SPDX JSON SBOM, a keyless Cosign
signature bundle, and GitHub build-provenance and SBOM attestations. A manual
workflow dispatch creates a signed dry-run Actions artifact only. Pushing the
version-matching tag takes the separately gated publication path and creates
the GitHub release. No v0.6 tag or release has been published yet.

See [deployment](docs/deployment.md#release-archives-and-verification) for the
asset contract, verification commands, and atomic versioned-directory upgrade
procedure.

## Minimal inventory and usage

Create `fleet.ini`:

```ini
[defaults]
user = deploy
identity = /home/operator/.ssh/fleet_ed25519

[web]
web-01 transport=ssh
web-02 port=2222 transport=ssh
```

Then inspect the inventory and run one argv on the group:

```bash
./build/bin/pipeshellx hosts list -i fleet.ini
./build/bin/pipeshellx run -i fleet.ini -g web --stream -c 16 -- uname -a
```

PipeShellX uses OpenSSH's `accept-new` host-key policy with a per-inventory
`<inventory>.known_hosts` file. Pre-seed and verify that file when trust on
first use is not acceptable.

A minimal local pipeline is:

```bash
./build/bin/pipeshellx pipe "'/bin/echo hello' | '/usr/bin/tr a-z A-Z'"
```

All-local pipeline files may be non-linear. Any graph containing a remote
stage must currently be one declared chain; remote pipeline edges use native
mTLS rather than SSH. See [pipelines and output](docs/pipelines.md).

Important output behavior:

- `--overflow drop-oldest|drop-newest` bounds retained capture and reports
  dropped bytes;
- the default `block` policy is lossless and unbounded;
- `spool` bounds only its in-memory tail, while temporary-disk growth and
  final full-result materialization remain unbounded;
- native flow-control credit bounds wire data in flight, not total capture.

Run `pipeshellx --help` for the complete command synopsis.

## Contributing

Contributions are welcome. Start with a
[`good first issue`](https://github.com/patil-rushikesh/PipeShellX/issues?q=is%3Aissue%20is%3Aopen%20label%3A%22good%20first%20issue%22)
or a [`help wanted`](https://github.com/patil-rushikesh/PipeShellX/issues?q=is%3Aissue%20is%3Aopen%20label%3A%22help%20wanted%22)
task, or propose a focused change through the
[issue chooser](https://github.com/patil-rushikesh/PipeShellX/issues/new/choose).
Read [CONTRIBUTING.md](CONTRIBUTING.md) for setup, test commands, pull-request
expectations, and the required CI checks. Use
[GitHub Discussions](https://github.com/patil-rushikesh/PipeShellX/discussions)
for setup and usage questions. Report vulnerabilities privately through
[SECURITY.md](SECURITY.md), not through a public issue.

## Documentation

- [Architecture](docs/architecture.md)
- [Deployment](docs/deployment.md)
- [Pipelines, output, and consensus](docs/pipelines.md)
- [psx/1 wire protocol](docs/wire_protocol.md)
- [Testing](docs/testing.md)
- [Benchmarks](docs/benchmarks.md)
- [Roadmap](ROADMAP.md)
- [Contributing](CONTRIBUTING.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Security](SECURITY.md)
- [Changelog](CHANGELOG.md)

PipeShellX is licensed under Apache License 2.0. The license, attribution, and
direct dependency notices are in [LICENSE](LICENSE), [NOTICE](NOTICE), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). These files are included at
the repository root and in installed packages.
