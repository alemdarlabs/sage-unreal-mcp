---
name: long-form-summary-compressor
description: Condenses long text into concise summaries while preserving essential ideas. Invoke when shrinking verbose tool output, doc passages, build logs, or research findings to fit token budget.
---

# Long-Form Summary Compressor

## Overview

Reduces complex content into digestible summaries for easy reading and context efficiency.

**Keywords**: summarization, long-form, clarity, conciseness, token-budget, compression

## Features

- Key point extraction
- Bullet or paragraph output (configurable)
- Dense material simplification
- Preservation of citations / refs when present

## Output Format

- Concise paragraph (target: 1/5 the original word count) OR
- Bulleted key points (3-7 items, in priority order)
- Preserved file:line references where applicable
- Note if any critical info was elided ("Truncated: 12 unrelated warnings omitted")

## Instructions

- Identify main points first; everything else is secondary
- Remove redundancy and filler aggressively
- Produce readable, actionable summary
- For build logs: surface errors first, warnings second, info last; collapse repeated patterns
- For docs: preserve decisions and constraints; drop motivation/throat-clearing

## Constraints

- No missing critical info (errors, decisions, constraints)
- No filler words ("As we discussed earlier", "It's worth noting")
- Token discipline: aggressive trim; if uncertain, lean shorter
- Mark elision explicitly so the user knows what was dropped
