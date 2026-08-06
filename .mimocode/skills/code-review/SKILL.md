---
name: code-review
description: Structured code review and fix workflow for C++ projects — audit codebase, identify issues, create prioritized fix plan, and execute fixes
---

# Code Review & Fix Skill

Perform a structured code review of a C++ project, produce a prioritized issue report, and execute fixes within the project directory.

## When to use

- User asks to "look at the project, find errors" / "посмотри проект, найди ошибки"
- User asks to verify refactoring progress against a plan document
- User asks to audit code quality or find duplications

## Workflow

### Phase 1 — Explore project structure

1. Use `Glob` with `**/*` to get the full file tree.
2. Use `Bash` with `ls -la` and `wc -l` to identify file sizes and line counts.
3. Identify oversized files (>500 lines) — these are primary candidates for issues.
4. Read the main entry point / plugin file to understand the project.

### Phase 2 — Read key files

Read the core files systematically:
- Main entry point (e.g., `*.php`, `main.cpp`)
- Files with the most lines (sorted by `wc -l`)
- Configuration and build files (`CMakeLists.txt`, `build.sh`, `*.json`)
- Any plan or documentation files referenced by the user

For each file, look for:
- Duplicated code blocks
- Missing error handling
- Security issues (CSRF, input validation, SQL injection)
- Dead/commented-out code
- Inconsistent version numbers
- Missing file existence checks

### Phase 3 — Identify and categorize issues

Create a structured report with these categories:

| Priority | Category | Description |
|----------|----------|-------------|
| 🔴 CRITICAL | Security | Vulnerabilities, missing auth checks |
| 🔴 CRITICAL | Logic errors | Wrong behavior, crashes, data loss |
| 🟡 HIGH | Code duplication | Repeated code blocks that should be unified |
| 🟡 HIGH | Missing error handling | Unguarded operations, missing null checks |
| 🟢 MEDIUM | Dead code | Commented-out code, unused files |
| 🟢 MEDIUM | Style/consistency | Naming, formatting, documentation gaps |

### Phase 4 — Create fix plan

For each issue, specify:
1. File path and line range
2. What is wrong
3. Proposed fix (concrete code change)
4. Risk assessment (safe / needs testing / may break other code)

### Phase 5 — Execute fixes

- Only fix issues marked CRITICAL and HIGH unless user says "do everything"
- Follow user's directive style (e.g., "Yolo" = proceed without asking)
- Stay within the project directory
- After each fix, verify with `wc -l` or build command if available
- Bump version numbers consistently (main file + build script)

### Phase 6 — Report results

Provide a summary:
- Total issues found and fixed
- Remaining issues (if any)
- Files modified
- Build verification status (if applicable)

## Refactoring plan verification variant

When the user asks to check a refactoring plan against actual code:

1. Read the plan document(s) in `docs/`
2. For each planned step, verify actual state:
   - Does the expected file exist? What is its line count?
   - Are expected new files/classes created?
   - Are old files removed?
3. Report: ✅ done / ❌ not done / ⚠️ partially done / inconsistent
4. Continue with the next unfinished step

Use `actor` tool with `explore` subagent to parallelize verification across multiple areas.

## Constraints

- Stay within the project directory
- Do not modify files outside the project root
- Preserve existing code style and language (Russian comments stay Russian)
- When bumping version, update ALL files that reference the version number
