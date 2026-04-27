---
name: tone-style-enforcer
description: Ensures outputs match a consistent tone or style guideline. Invoke when polishing docs, ADRs, README, or commit messages for cross-document consistency in Sage.
---

# Tone & Style Enforcer

## Overview

Keeps generated content aligned with the project's tone and writing conventions.

**Keywords**: style, tone, consistency, clarity, writing, documentation, brand-voice

## Features

- Tone preservation across documents
- Cross-document terminology consistency
- Formatting enforcement (headers, bullets, code blocks)
- Voice alignment (active vs passive, technical vs casual)

## Output Format

- Revised text aligned with the target style
- Diff or change log highlighting modifications (`- old line` / `+ new line`)
- Optional brief rationale for non-trivial changes

## Instructions

- Apply the defined tone to input text
- Check for terminology inconsistencies (e.g., "knowledge graph" vs "KG" — pick one and apply)
- Adjust language, structure, and formatting
- For Sage docs: tone is **technical, terse, opinionated, decision-anchored**
  - Active voice preferred
  - One-line section intros okay; avoid throat-clearing
  - Reference ADRs by ID when stating rationale
  - Code blocks for any commands or paths
  - See CLAUDE.md `Code Conventions` and `Working Rules` for more

## Constraints

- No deviation from the chosen style without justification
- Maintain semantic meaning of original content
- Token discipline: prefer surgical edits over wholesale rewrites
