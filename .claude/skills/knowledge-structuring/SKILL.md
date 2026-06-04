---
name: knowledge-structuring
description: Organizes unstructured information into clear frameworks, hierarchies, or structured notes. Invoke for ADR drafting, domain modeling, taxonomy work, project-understanding design, or schema discussions in Sage.
---

# Knowledge Structuring

## Overview

Transforms messy input into structured, usable knowledge artifacts: schemas, taxonomies, ADRs, workflows, and decision frameworks.

## Features

- Categorizes ideas into named groups
- Creates logical hierarchy
- Distinguishes entities, attributes, and relationships
- Highlights gaps, assumptions, dependencies, and decision points

## Output Format

- Structured framework with named sections
- Tight bullets per section
- Optional open questions block
- For schema work: explicit Entity / Attribute / Relationship layers

## Instructions

- Identify major topics first.
- Group related ideas under stable labels.
- For Sage project-understanding work, assume KuzuDB is retired by ADR-018 and prefer source-backed/live-inspection models unless a new ADR reopens persistence.
- For ADR drafting, follow `.claude/decisions/` style: context, decisions, consequences.
- Mark uncertain or disputed items explicitly.

## Constraints

- Avoid ambiguity.
- Keep hierarchy readable.
- Conform to existing project conventions.
- Respect Sage token discipline: concise by default, expandable on demand.
