---
name: mokagram-publish
description: Push reviewed mokaGram changes and create or update a pull request in ecronxy/mokagram when the user asks to publish, push, or open a PR.
---

# Publish mokaGram changes

A publication request is complete only after the source branch is pushed and an
open pull request targets `ecronxy/mokagram`, base branch `mokagram`. A push alone
or a pull request in a fork does not finish publication. Local builds and installs
alone do not authorize publication.

- Inspect remotes, the authenticated GitHub account and upstream before committing.
  Fetch `ecronxy/mokagram` and preserve unrelated working-tree and submodule edits.
- Push the source branch to the authenticated user's fork of `ecronxy/mokagram`.
  Do not assume `origin` is upstream or the right PR head: this checkout's origin
  may be the organization fork `kapustaltd/mokagram`. The working personal fork is
  `xdshkaaa/mokagram`; verify its ownership and parent before using it.
- Create the PR with explicit repository, base and head arguments, for example:
  `gh pr create --repo ecronxy/mokagram --base mokagram --head xdshkaaa:<branch>`.
  Write multiline descriptions to a temporary file and pass `--body-file`.
- Update an existing open PR for the same work. If the earlier PR is closed or
  merged, compare with current upstream and create a new PR containing the
  remaining changes. Do not reopen a closed PR unless requested.
- Keep product changes and workflow-only changes in separate commits. Only the
  workflow-only commit uses the `[ai] ` prefix; follow the repository commit rules.
- Verify the remote branch SHA, PR destination, diff, state and mergeability, then
  return the PR link. Do not merge or force-push without explicit authorization.
