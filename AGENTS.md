# Notes for coding agents

Read `CONTRIBUTING.md` before changing anything. Every rule in it applies to agents.

@CONTRIBUTING.md

- Before each commit:
  1. Stage exactly what you mean to commit (no `git commit -a`).
  2. Print the scope with `python3 scripts/check_text_hygiene.py scope`.
  3. Write the message as UTF-8 without a byte-order mark, to a file outside the repository.
  4. Check it with `python3 scripts/check_text_hygiene.py message <file>`.
  5. Commit with `git commit -F <file>` once the check passes.
- If the scope list printed in step 2 has more than three entries, the message check skips the scope: summarize the touched areas yourself, for example `(...)`.
- On Windows, run `python` instead of `python3`.
- Do not copy the format of older commits. Much of the history before October 2026 breaks these rules.
- When you brief a sub-agent, give it the exact commit format and these rules, not a paraphrase.
- Keep working papers (briefs, reviews, scratch measurements, logs) outside the repository.
- Add no `Co-Authored-By` or other trailers.
