# XS RFCs

Non-trivial changes to the language, stdlib, or governance ship as
RFCs before any code lands. See `POLICY.md` for the full lifecycle.

## Filing an RFC

1. Copy `0000-template.md` to the next free number: `0001-slug.md`,
   `0002-slug.md`, etc.
2. Open a pull request titled `rfc: <slug>`.
3. Comment discussion happens on the PR.
4. A maintainer will move the RFC to `accepted`, `postponed`, or
   `rejected`, with rationale recorded in the RFC front-matter.

## Shortcut: what needs an RFC?

| Change                                              | RFC? |
|-----------------------------------------------------|------|
| New language keyword or syntax                      | yes  |
| New Tier 1 stdlib module                            | yes  |
| Breaking change to any Tier 1 surface               | yes  |
| New Tier 2 (`@unstable`) module                     | no   |
| New Tier 3 (`experimental.*`) module                | no   |
| Bug fix preserving the documented contract          | no   |
| Bug fix *changing* documented contract              | yes  |
| Performance work with no contract change            | no   |
| New benchmarks or tests                             | no   |
| New CI job                                          | no   |
| Governance / security / RFC-process changes         | yes  |

When in doubt, file the RFC. The cost of a wasted RFC is cheaper than
the cost of retracting a bad landing.
