---
name: ship
description: Commit and push the changes made in this session, under the git policy for this project. The policy sets the author as Lewis, pushes to main only, and forbids branches, footers, and emojis. Use this skill when Lewis says to commit or to push.
---

# Commit and push (project policy)

## Pre-flight checks

1. Work only in the working repository
   (`c:\Users\User\Documents\home-controller`); this is the folder with the
   GitHub remote. Never commit or push from the private folder.
2. Confirm that the private folder already carries every change in this
   commit. Apply a private-folder-only edit to the working repository
   first. Apply a working-repository-only edit to the private folder
   first. Run the `/sync-private` skill first, when unsure.
3. Check the author identity before the first commit:
   ```
   git config user.name    # must be: Lewis
   git config user.email   # must be: lewisf94@users.noreply.github.com
   ```
   A cloud or web session defaults to Codex as the author. Fix an incorrect
   value with this command:
   ```
   git config user.name "Lewis" && git config user.email "lewisf94@users.noreply.github.com"
   ```

## Commit message rules

- Use the conventional-commit style already used in this repository, for
  example:
  `fix(p4_shared): restore 56-album list + fix generator path desync`
- Do not use an emoji. Do not add a `Co-Authored-By` footer, or a
  "Generated with Codex" footer, or a `https://Codex.ai/code/session_...`
  footer.
- Run `git status` and `git diff` first. Stage only the intended files. Do
  not run a blanket `git add -A` command when an unrelated change is
  present.

## Push rules

- Always push directly to `main`: `git push -u origin main`.
- Never create a branch, even on an indirect request. Only an explicit
  instruction to create a branch, stated in that exact message, overrides
  this rule.
- Never pass `--no-verify`. Never force-push without an explicit
  instruction to do so.

## If a commit lands with the wrong author

```
git rebase <last-good-sha> --exec "git commit --amend --reset-author --no-edit"
```

Then push the corrected history. Force-push only with explicit permission.
