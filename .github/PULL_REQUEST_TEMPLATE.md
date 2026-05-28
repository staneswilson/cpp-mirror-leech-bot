<!--
Thank you for contributing to CMLB. Please fill in each section.
Empty sections may delay review.
-->

## Summary

<!-- One or two sentences describing what this PR does. -->

## Motivation

<!-- Why is this change needed? Link issues with "Fixes #N" / "Refs #N". -->

## Changes

<!-- Bullet-list of concrete changes. Group by layer if it spans many. -->
-
-
-

## Testing

<!-- How did you verify this works? -->
- [ ] Unit tests added or updated
- [ ] Integration tests added or updated (if a backend changed)
- [ ] Manual verification described below

<!-- Describe manual verification: commands run, outputs observed. -->

## ADR

<!-- If this PR introduces a non-trivial architectural decision, link the ADR. -->
- [ ] No ADR needed — change is local
- [ ] ADR added at `docs/adr/NNNN-<slug>.md`
- [ ] Existing ADR updated: <!-- which one --->

## Checklist

- [ ] `clang-format --dry-run --Werror` passes on changed files
- [ ] `clang-tidy` passes on changed files
- [ ] `ctest --preset debug --output-on-failure` passes locally
- [ ] Sanitizer build passes (if change touches concurrency or I/O)
- [ ] `CHANGELOG.md` updated under `[Unreleased]`
- [ ] Documentation updated (`docs/` or inline comments) where behaviour changed
- [ ] Conventional Commit subject line (`type(scope): subject`, ≤ 72 chars)
- [ ] No secrets, tokens, or credentials in code, comments, tests, or commit messages
- [ ] No new dependencies without justification in `vcpkg.json` and an ADR if non-trivial

## Risk Assessment

<!-- For changes touching persistence, telegram, or composition root: -->
- **Blast radius:** <!-- which subsystems / users affected -->
- **Rollback plan:** <!-- how to revert safely -->
- **Migration:** <!-- forward-only schema change? config flag? -->
