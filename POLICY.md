# XS stability and governance policy

This document defines what "stable" means for XS, how breaking
changes are handled, and how proposals turn into language or stdlib
features.

## Stability tiers

Every public surface in XS belongs to exactly one tier.

### Tier 1 — stable
Language syntax and semantics as documented in `LANGUAGE.md`. Every
stdlib module imported without the `@unstable` annotation.

- Breaking changes are only permitted across a major version bump
  (`0.x → 1.0`, `1.x → 2.0`).
- Within a stable major version, semantics do not silently change.
  Bug fixes that alter observable behaviour ship with an opt-in flag
  first, a deprecation cycle second, then a default flip on the next
  major.
- CLI flags and output formats (stack trace layout, diagnostic
  shape, `xs doc` output) are Tier 1.

### Tier 2 — unstable (opt-in)
Marked with `@unstable` on the module, type, or function. Requires
`--allow-unstable` to compile against, or `xs.toml [unstable]`
declaration per feature.

- Shapeshift freely; no deprecation cycle.
- Will either promote to Tier 1 (after at least one minor cycle of
  real use) or be removed entirely.

### Tier 3 — experimental
Not in `stdlib/`. Must live behind `import experimental.<name>` and
only available under `XS_EXPERIMENTAL=1`.

- Used for genuinely research-grade work where we don't want to
  pretend there's even a stability contract.

### What isn't covered
- Exact timings of the GC (pause budgets, collection frequency).
- Exact layout of bytecode or JIT code buffers.
- `xs --emit bytecode` output format (versioned separately; an older
  xsc can refuse a newer `.xsc` file).
- Internal symbols prefixed with `_` or `__`.
- Plugin-injected names — those are the plugin author's contract, not
  the language's.

## Versioning

`MAJOR.MINOR.PATCH`, following semver with the Tier 1 definition above
as the contract.

- `PATCH` bumps for bug fixes that preserve behaviour.
- `MINOR` bumps for additions and `@unstable` promotions. Existing
  Tier 1 code keeps working.
- `MAJOR` bumps can remove deprecated surface and flip defaults.
  Requires a release candidate cycle (≥ 2 weeks, minimum two RCs).

### Pre-1.0

We're pre-1.0. `0.x → 0.(x+1)` may include breaking changes, but each
one must appear in `CHANGELOG.md` under "breaking" and have been
flagged `@deprecated` for at least one `0.x` release before the
removal.

After 1.0, the `0.x` rule is retired.

### LTS cadence

Once we ship 1.0, every other `MAJOR` is LTS with 18 months of
backported security fixes. Non-LTS majors get 6 months.

## Deprecation

1. Author adds `@deprecated("use X instead", since="0.y.z")` to the
   surface.
2. The CHANGELOG entry for `0.y.z` lists it under "deprecated".
3. First MINOR after the deprecation emits a compile-time warning
   (`D0xxx` diagnostic code) at every use site.
4. Earliest removal: the following MAJOR.
5. Removal CHANGELOG entry lists the deprecation version, the warning
   version, and the removal version.

Deprecations never remove in a MINOR release.

## RFC process

All non-trivial changes — new language features, new stdlib modules,
backwards-incompatible changes, governance changes — go through an
RFC before any code lands.

Trivial means:
- Bug fixes that preserve the documented contract.
- Adding `@unstable` items (no Tier 1 obligation).
- Internal refactors (no public surface change).
- New benches or tests.
- Documentation fixes.

Everything else: file an RFC.

### RFC lifecycle

1. Copy `RFCS/0000-template.md` to `RFCS/NNNN-slug.md`, open a PR.
2. Discussion happens on the PR. Any XS contributor may comment.
3. A core maintainer moves the RFC to one of:
   - **Accepted** (merge, proceed to implementation).
   - **Postponed** (valid but needs groundwork).
   - **Rejected** (with rationale recorded in the RFC file).
4. Accepted RFCs retain the PR link and a `status: accepted in #NNN`
   field, along with the implementation issue.
5. After implementation lands, RFC status flips to `implemented` with
   the shipping version recorded.

An RFC cannot be accepted in under 7 days unless it's a security fix.
High-impact RFCs (language grammar, Tier 1 stdlib, GC changes) need at
least 14 days of public comment and explicit sign-off from two core
maintainers.

### RFC template

See `RFCS/0000-template.md`.

## Maintainers

Currently a two-person team. Quorum: one maintainer for Tier 2/Tier 3
work, both for Tier 1 / breaking changes / governance.

Adding a maintainer requires a two-thirds vote of existing maintainers
and at least 6 months of sustained, high-quality contribution.

## Security

Report security issues to <security@xslang.org> (not the public
tracker). We acknowledge within 72 hours, ship a fix within 30 days
for the supported release stream, and publish a CVE advisory within
30 days after the fix ships.

Supported streams: latest stable release, latest LTS (if distinct), and
any release that is less than 6 months old.

Signed releases: every tag triggers a minisign signature on each
artifact, published with the release. The public key lives at
`https://xslang.org/pubkey` and in this repo as `MINISIGN.pub`.
