## Summary

<!-- Explain the user-visible outcome and why this change is needed. -->

## Changes

<!-- List the important implementation and documentation changes. -->

## Related issue

<!-- Use "Closes #123" when this pull request fully resolves an issue. -->

## Platforms and compatibility

<!--
State the platforms and transports exercised: Linux, macOS, SSH, native mTLS,
or local execution. Call out public API, CLI, configuration, wire-protocol,
packaging, or backward-compatibility changes. Write "Not applicable" with a
short reason when this section does not apply.
-->

## Security and reliability impact

<!--
Describe changes to command execution, trust boundaries, credentials, TLS,
process ownership, cancellation, output bounds, or audit data. Write "None"
only after considering these areas. Do not disclose an unpatched vulnerability
in a public pull request; follow SECURITY.md instead.
-->

## Validation

<!-- List exact commands run and summarize results and any expected skips. -->

```text
# Example:
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 120
```

## Checklist

- [ ] I added or updated focused tests for behavior changes, or explained why
      tests are not applicable.
- [ ] The relevant build and CTest suite pass locally.
- [ ] `./scripts/check_layering.sh` and `./scripts/check_docs.py` pass.
- [ ] I formatted touched C++ files and ran relevant static or sanitizer checks,
      or explained why they are not applicable.
- [ ] I updated user-facing documentation and `CHANGELOG.md` for changed
      behavior or contracts, or confirmed they are not applicable.
- [ ] I tested every affected supported platform or clearly identified the
      coverage that remains for CI.
- [ ] The diff contains no credentials, private hostnames, certificate material,
      generated inventories, audit output, build artifacts, or local absolute
      paths.
- [ ] I reviewed the contribution guide, security policy, and Code of Conduct.
