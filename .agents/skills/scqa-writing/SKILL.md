---
name: scqa-writing
description: Structures content using the Situation-Complication-Question-Answer framework. Invoke when drafting README sections, ADR rationale paragraphs, or positioning copy that needs a clear narrative arc.
---

# SCQA Writing

## Overview

Structures unclear ideas into a logical narrative: Situation → Complication → Question → Answer. Used in consulting, executive memos, and technical positioning.

**Keywords**: writing, narrative, structure, scqa, positioning, readme, adr-rationale

## Core Framework

### Situation
- Establish current state, baseline context
- Concise, factual

### Complication
- Introduce the tension or problem that breaks the situation
- Creates the "why this matters"

### Question
- The natural question the reader now asks
- Should feel inevitable given the complication

### Answer
- The insight, decision, or solution
- Clear and actionable

## Output Format

- 4 labeled SCQA blocks (1-3 sentences each), OR
- Flowing prose where labels are implicit but order is preserved

## Instructions

- Identify which input fragment goes to which letter
- Keep each block tight; one idea per block
- For Sage: useful in ADR `Context` sections, README "What this is" framing, and public positioning copy

## Constraints

- All four sections must be present and distinct
- No repetition across sections
- Token discipline: prose mode for short forms, labeled mode for longer ones
