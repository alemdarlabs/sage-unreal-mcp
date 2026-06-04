---
name: excalidraw-diagrams
description: Converts textual concepts into Excalidraw-friendly diagram instructions (nodes, connectors, layout). Invoke ONLY when the user explicitly requests Excalidraw output (sketch-style, presentation, brainstorm board); for source-controllable diagrams use flowchart-decision-builder (Mermaid).
---

# Excalidraw Diagrams

## Overview

Translates concepts into Excalidraw-compatible diagram structures (nodes + connectors with layout hints). Sketch aesthetic, suited to presentations and external sharing.

**Keywords**: excalidraw, diagram, sketch, presentation, visual, brainstorm

## When to use vs Mermaid

- **Mermaid (default for Sage)**: source-controllable, renders in markdown — use `flowchart-decision-builder`
- **Excalidraw (this skill)**: hand-drawn aesthetic, presentations, brainstorm boards, when shareable visual asset is wanted outside the repo

## Output Format

- Diagram title
- Nodes: list of `{id, label, shape}` (rectangle / ellipse / diamond)
- Connectors: list of `{from, to, label?}`
- Layout suggestion (hierarchical / radial / freeform)

## Instructions

- Identify primary entities → nodes
- Identify relationships → directed connectors
- Group conceptually related nodes for layout
- Suggest pen/sketch styling notes if relevant

## Constraints

- Avoid clutter; if >15 nodes, recommend splitting
- Maintain clarity over decoration
- Token discipline: diagrammatic shorthand, no full prose narration
- Do not invoke for in-repo architecture diagrams — those go in Mermaid
