---
name: ship
description: Commit and push the session's changes following this project's strict git policy (author Lewis, direct to main, no branches, no footers, no emojis). Use when Lewis says to commit/push.
---

# Commit + push (project policy)

## Pre-flight

1. Work ONLY in the working repo (`c:\Users\User\Documents\home-controller`) —
   it has the GitHub remote. NEVER commit or push from the private folder.
2. Confirm the private folder already has every change being committed
   (edits made only in private must be applied to the working repo first —
   and vice versa). If unsure, run /sync-private first.
3. Check the author identity BEFORE the first commit:
   ```
   git config user.name    # must be: Lewis
   git config user.email   # must be: lewisf94@users.noreply.github.com
   ```
   If wrong (cloud/web sessions default to Claude), fix it:
   ```
   git config user.name "Lewis" && git config user.email "lewisf94@users.noreply.github.com"
   ```

## Commit message rules

- Conventional-commit style used in this repo, e.g.
  `fix(p4_shared): restore 56-album list + fix generator path desync`
- **No emojis.** **No `Co-Authored-By` / "Generated with Claude" footers.**
  **No `https://claude.ai/code/session_...` footers.**
- `git status` and `git diff` first; stage only the intended files (no
  blanket `git add -A` if unrelated changes are present).

## Push rules

- **Always push directly to `main`**: `git push -u origin main`.
- NEVER create a branch, even if asked indirectly — only an explicit
  "create a branch" in that specific message overrides this.
- Never `--no-verify`. Never force-push without explicit instruction.

## If commits landed with the wrong author

```
git rebase <last-good-sha> --exec "git commit --amend --reset-author --no-edit"
```
then push (force-push only with explicit permission).
