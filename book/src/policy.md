# Stability and governance

The full policy lives in `POLICY.md` at the repo root. The book
summarises and points you at the contract.

## Tier 1 — stable

Anything documented here without an `@unstable` tag, plus the CLI
and stdlib surface. Breaking changes only across MAJOR versions, and
only after a deprecation cycle.

## Tier 2 — `@unstable` opt-in

Marked at the function or module level. Requires `--allow-unstable`
to compile. Changes freely between MINOR versions.

## Tier 3 — `experimental.*`

Lives outside `stdlib`. Loaded only with `XS_EXPERIMENTAL=1`. May be
removed without warning.

## Pre-1.0

We're at `0.x`. Until 1.0, MINOR can break, but each break needs at
least one MINOR with `@deprecated` first. After 1.0, even the
deprecation cycle is locked in.

## Release cadence

- PATCH: as needed for fixes.
- MINOR: every 6 weeks.
- MAJOR: roughly yearly, post-1.0.
- LTS: every other MAJOR after 1.0; 18-month support window.

## RFCs

Non-trivial changes go through `RFCS/`. See `RFCS/README.md` for the
template and process. The short version: significant changes need
public discussion before code lands.

## Security

`security@xslang.org` for private reports. We acknowledge in 72
hours, ship in 30 days, publish a CVE within 30 days after the fix.

Releases are signed with minisign. Public key at
`https://xslang.org/pubkey` and in this repo as `MINISIGN.pub`.
