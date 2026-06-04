---
name: skill-creator
description: Generates new Codex Skills in Anthropic Agent Skills format with proper frontmatter, scoped description, and structured sections. Invoke when the user wants to add a new skill to .Codex/skills/ in Sage.
---

# Skill Creator (Meta)

## Overview

Produces ready-to-use Codex Skills in Anthropic Agent Skills format, dropped at `.Codex/skills/<name>/SKILL.md`.

**Keywords**: skill, creator, meta, anthropic, agent-skills, Codex

## Anthropic Format Reference

```markdown
---
name: <kebab-case-name>
description: <one-line; trigger conditions; ≤1024 chars>
---

# <Title Case Name>

## Overview
## Keywords
## Output Format
## Instructions
## Constraints
```

Frontmatter accepts only `name` and `description` (no `license`, no other fields unless Anthropic adds them).

## Output Format

- Folder name (kebab-case, matches `name` field)
- Full SKILL.md content per the Anthropic structure above
- Description tuned with "Invoke when..." pattern for clean triggering

## Instructions

- Ask for the skill's purpose and trigger condition explicitly
- Pick a clear `name` (kebab-case, no `-skill` suffix — convention in this repo)
- Write `description` as a model-invocation hint: **action + context**
  - Good: "Reviews C++ code for memory safety. Invoke when reviewing server or plugin source."
  - Bad: "Code reviewer." (too short, won't trigger reliably)
- Structure body per the reference template
- For Sage: include token discipline note and project-specific guidance (UE, C++23, CMake/vcpkg, KuzuDB) where relevant

## Constraints

- Description must be 50-1024 characters
- Frontmatter only `name` + `description` — nothing else
- Output must be drop-in ready: no further editing required
- Token discipline: skill body itself should respect Sage's principles (concise, ID-first)
