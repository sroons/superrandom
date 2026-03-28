#!/usr/bin/env bash
# review-loop.sh
# Two-agent Claude Code review loop.
# Agent 1 (Reviewer) reads the code and writes a structured review.
# Agent 2 (Author)   reads the review, responds to each point, and applies agreed changes.
#
# Usage:
#   ./review-loop.sh --diff                   # review staged/unstaged git changes
#   ./review-loop.sh src/MyComponent.tsx       # review a specific file
#   ./review-loop.sh src/                      # review a directory

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────
REVIEW_FILE="${REVIEW_FILE:-REVIEW.md}"
CLAUDE="${CLAUDE_CMD:-claude}"   # override if claude isn't on PATH

# ── Helpers ───────────────────────────────────────────────────────────────────
usage() {
  echo "Usage: $0 --diff | <file-or-dir>"
  exit 1
}

run_agent() {
  local role="$1"
  local prompt="$2"
  echo ""
  echo "▶ Running $role agent..."
  # --dangerously-skip-permissions lets claude run non-interactively
  # without prompting for each tool call. Review the prompts below
  # before using in a sensitive codebase.
  "$CLAUDE" --dangerously-skip-permissions -p "$prompt"
}

# ── Args ──────────────────────────────────────────────────────────────────────
[[ $# -lt 1 ]] && usage
TARGET="$1"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

if [[ "$TARGET" == "--diff" ]]; then
  DIFF_CONTENT=$(git diff HEAD 2>/dev/null || git diff 2>/dev/null)
  if [[ -z "$DIFF_CONTENT" ]]; then
    echo "No git diff found. Stage some changes or specify a file/dir."
    exit 1
  fi
  TARGET_LABEL="git diff (HEAD)"
  CODE_BLOCK="\`\`\`diff
$DIFF_CONTENT
\`\`\`"
else
  [[ ! -e "$TARGET" ]] && { echo "Target not found: $TARGET"; exit 1; }
  TARGET_LABEL="$TARGET"
  # Claude Code will read the files itself via its tools, so we just name the path
  CODE_BLOCK="(See target path below — use your Read tool to inspect it.)"
fi

# ── Phase 1: Reviewer ─────────────────────────────────────────────────────────
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " Phase 1 — Reviewer"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

REVIEWER_PROMPT="You are a senior engineer acting as a code reviewer.

$(if [[ "$TARGET" == "--diff" ]]; then
  echo "Review the following git diff:"
  echo ""
  echo "$CODE_BLOCK"
else
  echo "Review the code at this path: $TARGET"
  echo "Use your Read tool to read any files you need."
fi)

Write a structured review to the file \`$REVIEW_FILE\` using this exact format:

---
# Code Review — $TIMESTAMP
## Target: \`$TARGET_LABEL\`

## Issues

For each issue use this structure:
### [CRITICAL|MAJOR|MINOR|NIT] Short title
- **Location**: <file>:<line> or 'general'
- **Issue**: What the problem is
- **Suggestion**: What to do instead

## Strengths
Brief notes on what is done well.

## Summary
One paragraph overall assessment and recommendation (Approve / Approve with changes / Request changes).

---

Write ONLY the review content to the file. Do not output anything else."

run_agent "Reviewer" "$REVIEWER_PROMPT"

if [[ ! -f "$REVIEW_FILE" ]]; then
  echo "❌ Reviewer did not create $REVIEW_FILE. Aborting."
  exit 1
fi

echo ""
echo "✅ Review written to $REVIEW_FILE"

# ── Phase 2: Author ───────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " Phase 2 — Author"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

AUTHOR_PROMPT="You are the author of code that has just been reviewed.

1. Read the review at \`$REVIEW_FILE\`.
2. Read the code that was reviewed: \`$TARGET_LABEL\`$(if [[ "$TARGET" != "--diff" ]]; then echo " (use your Read tool)"; fi).
3. For each issue raised, decide: agree or disagree, and why.
4. Apply any changes you agree with directly to the source files.
5. Append your response to \`$REVIEW_FILE\` under a new section using this exact format:

---
## Author Response — $TIMESTAMP

For each issue title from the review:
### [Issue title]
- **Response**: Agree / Disagree — [your reasoning]
- **Action**: [Changed: description of what you did] OR [No change: reason]

## Overall Notes
Any broader comments or context the reviewer may have lacked.
---

After appending your response, you are done. Do not output anything else."

run_agent "Author" "$AUTHOR_PROMPT"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ Review loop complete."
echo "   Full exchange saved to: $REVIEW_FILE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
