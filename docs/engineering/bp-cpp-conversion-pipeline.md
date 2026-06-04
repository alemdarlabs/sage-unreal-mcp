# Blueprint → C++ Conversion Pipeline

> Status: Production playbook for BP→C++ migration projects.
> Origin: Codified from the 2026-05-01 HeroFlight incident, where partial
> ABP_Player conversion + missing save discipline + a Component reparent
> race triggered ~4 hours of recoverable but expensive data loss. The
> tools and gates documented here exist specifically to make that class
> of failure unreachable.

---

## TL;DR

Convert one Blueprint by walking ten gates in order. **Every gate has a
programmatic exit criterion; if it fails, stop and recover — don't proceed.**
Save is atomic per-BP, not per-batch. Refresh-without-clear is forbidden;
the BP's graphs/variables/functions get emptied **before** reparent so pin
orphans become structurally impossible.

```
1. SNAPSHOT     bp.full_dump (include_t3d=true)
2. INSPECT      bp.read · bp.list_variables · bp.read_components · bp.get_dependencies
3. DEPS READY   parent C++ class compiled · referenced UPROPERTYs typed correctly
4. C++ WRITE    header + cpp mirroring every BP variable/function/event
5. BUILD        editor close · UBT · reopen · handshake
6. EMPTY        bp.sanitize_variable_names → bp.clear_graphs → bp.delete_all_variables
7. REPARENT     bp.reparent (safe now: no nodes left to orphan)
8. VERIFY       bp.validate (error_count == 0) · bp.read (parent_class match)
9. SAVE         bp.save_assets paths=[BP] · bp.read teyit
10. PIE SMOKE   gameplay regression check
```

**Only step 6's first sub-step (sanitize) is allowed to run before step 4 —
it must, because C++ identifiers in the header have to match the BP's FNames.**

---

## Why this pipeline exists

The naive sequence `reparent → refresh → save` looks like it should work.
It doesn't, in three different ways, all observed in production:

1. **Pin orphan cascade.** Reparenting a BP whose graphs still hold
   references to renamed/retyped variables produces "in-use pin no longer
   exists" compile errors. The engine's `ReconstructNode` rebuilds the pin
   set against the new schema and flags orphans, but it doesn't migrate
   their links — so even if you sweep orphans afterward, you've cut the
   wire that fed the next node's `Target` pin. `bp.refresh_nodes` will
   migrate by normalised name now, but the only way to make orphans
   structurally impossible is to remove every node before reparenting.

2. **Variable-type silent degrade.** When a Component BP is reparented to
   its C++ parent, every BP that holds an SCS reference to that component
   has its `FBPVariableDescription::VarType` quietly downgraded from the
   BP child class to the C++ parent class. The variable's `Get` node now
   yields a `USuperheroFlightComponent*`, but BP-defined functions on the
   child class still expect `UBP_SuperheroFlightComponent_C*` on their
   `Target` pin. Every call site reports "type incompatible." The fix is
   either (a) finish moving the child's functions into C++ before
   reparenting dependents, or (b) re-set the variable type explicitly via
   `bp.set_variable_type`. The pipeline's step 3 gate ("deps ready")
   exists to catch (a) before it becomes (b).

3. **Save granularity.** `save_assets` without a `paths` filter saves
   every dirty asset, including ones in a transitional broken state.
   The pipeline's step 9 saves *only* the BP whose validation passed in
   step 8. A failed validation at step 8 means the BP's in-memory state
   is broken; closing the editor without saving rolls it back to disk.
   Save discipline = atomic per-BP, never bulk.

---

## The ten steps in detail

### Step 1 — SNAPSHOT

`bp.full_dump path=/Game/.../BP include_t3d=true output_path=Saved/SageDumps/<BP>/dump-<ts>.json`

The dump is the canonical truth: variables, components, function graphs
with full pin connectivity, CDO defaults, dependencies, T3D for paste-back
recovery. **`include_t3d=true` is non-default**; without it, recovery via
`bp.import_nodes_t3d` is unavailable.

**Rotation**: existing dumps stay; new dumps land with timestamp suffix.
The Phase-0 dump represents pre-everything state; later dumps capture
intermediate states for diffing.

**Exit gate**: file exists, size > 0, `schema_version` field present.

### Step 2 — INSPECT

```
bp.read path                        # parent class, counts
bp.list_variables path              # all NewVariables + types
bp.read_components path             # SCS hierarchy
bp.get_dependencies path            # what BPs/levels depend on this
```

Log everything to a state file, not memory. The same query at step 9
verifies the rename/clear/reparent didn't lose anything that wasn't
supposed to leave.

**Exit gate**: state captured, dump's variable/function counts match the
live counts (drift caught early).

### Step 3 — DEPS READY

For every BP type the target references, the parent C++ class must already
be compiled and its UPROPERTYs must already match the FNames the BP uses.

```
bp.get_dependencies(BP)             # downstream
                                    # walk: each dep's parent_class must be a C++ UCLASS
                                    # walk: each UPROPERTY in the C++ class must match a BP variable FName
```

This gate is the one I missed in the HeroFlight incident. Reparenting a
Component before its 33 BP-defined functions are in C++ means downstream
AnimBPs see a typed-down variable but call a function that doesn't exist
on the parent class — every call is a compile error.

**Exit gate**:
- All parent C++ classes resolve through `list_classes`, `reflect_class`, or C++ source inspection
- All BP-side variables that match a parent UPROPERTY name share the
  parent's type (no "BP_X_C → UX downgrade" pending)
- Build succeeded after the parent class was last edited

### Step 4 — C++ WRITE

Header + implementation that mirror the BP one-to-one. No simplification,
no optimisation, no "the C++ version doesn't really need this." Refactor
is a separate PR.

Naming rules — these are non-negotiable for FName equality on reparent:
- BP `float` → C++ `double` (UE 5 LWC; `float` silently truncates)
- BP variable FName must match the C++ UPROPERTY identifier exactly
- BP variable display strings (with spaces, Turkish chars, keywords, etc.)
  cannot be C++ identifiers — sanitize the BP first (step 6 sanitize pass).
  See [Variable naming edge cases](#variable-naming-edge-cases).
- `BlueprintNativeEvent` overrides require `_Implementation` suffix
- Dynamic delegate handlers require `UFUNCTION()`
- `TSubclassOf<T>` requires the full header for `T`, not a forward decl

**Exit gate**: every BP variable, function, and event has a corresponding
C++ symbol. Build pass.

### Step 5 — BUILD

```
editor.close                       # required — Live Coding can't swap class layout
build_<project>.ps1                # UBT
editor.open                        # waits for SageBridge handshake
list_editors                       # count == 1
```

Class-layout changes (UPROPERTY add/remove, parent class, new UCLASS)
require a full editor restart. Live Coding will appear to succeed but
leaves stale Schema in memory.

**Exit gate**: BUILD SUCCESSFUL · editor running · sage handshake live.

### Step 6 — EMPTY (the BP becomes a shell)

This is the step the naive playbook skips. Three sub-steps, in order:

```
bp.sanitize_variable_names path dry_run=false    # rename FNames to C++-legal identifiers
bp.clear_graphs path scope=all                   # remove every K2Node from event/function/macro graphs
bp.delete_all_variables path                     # remove all NewVariables (now redundant with C++)
```

After this step the BP holds:
- ✅ SCS component hierarchy (designer-overridable instance values)
- ✅ Asset references (animations, materials, meshes, CDO knobs)
- ❌ 0 graphs (event/function/macro all empty)
- ❌ 0 NewVariables (everything is in the C++ parent)
- ❌ 0 BP-defined functions

**Pin orphans become impossible** — there are no nodes left to hold
references that could go stale on reparent. This is what makes step 7 safe.

**Exit gate**: `bp.read` returns variable_count=0, function_count=0,
event_graph_count's pages are empty (entry nodes preserved if
`keep_entry_nodes=true`).

**Recovery**: if the C++ migration was incomplete and you need a behaviour
back temporarily, reload the asset from disk (no `save_assets` was called
yet) and resume step 4. **Don't refresh-and-pray** — that's the trap that
turns 30 minutes of step 4 into 4 hours of step 7 debugging.

### Step 7 — REPARENT

```
bp.reparent path new_parent_class=/Script/<Module>.<Class>
```

Now safe because the BP has no nodes. The engine's reparent path will
update the parent class pointer, regenerate the SCS, and migrate
inheritable component overrides.

**Exit gate**: `bp.read` returns the new parent_class string · `bp.compile`
returns errors=0.

### Step 8 — VERIFY

```
bp.validate path                   # error_count == 0
bp.get_cdo_properties path         # diff against dump (step 1) — no value drift
bp.read path                       # parent_class, function/var counts match expectations
```

`bp.validate` runs `CompileBlueprint` with `SkipSave` and a silent results
log — it surfaces problems without dirtying the asset. **A non-zero
error_count is a stop-the-line event.** Don't save.

**Exit gate**: validation pass · CDO matches dump · structural counts as
expected (typically 0 graph nodes / 0 vars / 0 functions for a fully
converted BP).

### Step 9 — SAVE

```
bp.save_assets paths=[/Game/.../BP]
bp.read path                       # parent_class persisted (catch silent rollback)
```

**Save with explicit `paths`**, never naked `save_assets()` — the latter
saves every dirty asset including in-progress half-converts.

After save, immediately re-read. If `parent_class` reads back as the old
class, the asset on disk silently rejected the change (rare, but it
happens with corrupted SCS overrides). Re-do step 7 in that case.

**Exit gate**: file modified on disk · re-read confirms persisted state.

### Step 10 — PIE SMOKE

```
run_pie                            # play in editor
                                   # exercise the converted BP's primary feature
                                   # check log for runtime errors
stop_pie
```

Compile passing isn't behaviour passing. Spawn the BP, drive its happy
path, look for regressions in adjacent features. Frame-budget regressions
count too — a converted BP can be slower than the original if a member
variable's type widened or a function's BP→C++ port is allocating where
the BP wasn't.

**Exit gate**: PIE crash-free · feature reachable · no error/warning
spike in `editor.search_log`.

---

## Tools added for this pipeline

Five tools were added to the BP authoring surface specifically to make
the gates above mechanically achievable. They share the same conventions
as the rest of the BP tools: PIE-rejected writes, ScopedTransaction +
Modify(), explicit save (the tool never auto-saves the asset).

### `bp.rename_variable`

Renames a BP-defined member variable in place. Wraps
`FBlueprintEditorUtils::RenameMemberVariable`, which updates the
`FBPVariableDescription`, rewrites every `K2Node_VariableGet`/`Set`
reference across all graphs, and renames `OnRep_<Name>` for replicated
variables.

UE Python's `BlueprintEditorLibrary.replace_variable_references` is
*not* equivalent — it touches references but not the definition. That
gap is why this tool exists.

**Validation**:
- Old name must exist as a NewVariable (inherited variables reject)
- New name must be a C++ identifier (`^[A-Za-z_][A-Za-z0-9_]*$`)
- New name must not collide with another NewVariable
- New name must not be a C++ reserved keyword
- New name length cap: 128 chars
- New name must pass `FName::IsValidXName(INVALID_OBJECTNAME_CHARACTERS)`

**Idempotent**: `old_name == new_name` → `{already: true}`.

**Returns**: `{old_name, new_name, references_updated, compiled, [already]}`.

### `bp.sanitize_variable_names`

Bulk-renames every BP member variable whose FName is not a valid C++
identifier into a sanitised form. Built-in algorithm (sage-side, no
caller logic):

1. Known Latin diacritic transliteration (for example `U+00E7 -> c`,
   `U+011F -> g`, `U+0131 -> i`, `U+0130 -> I`, `U+00F6 -> o`,
   `U+015F -> s`, `U+00FC -> u`, plus uppercase pairs).
2. Replace any non-`[A-Za-z0-9_]` with `_`.
3. Collapse runs of `_`.
4. Trim leading/trailing `_`.
5. Empty result → `"UnnamedVar"`.
6. Leading digit → prepend `_`.
7. C++ keyword (lower-case match against the reserved-word table) →
   suffix `_Var`.
8. Cap at 128 chars (truncate; surfaces as `truncated` reason).

**Conflict resolution**: if two variables sanitise to the same name, or
the sanitised name collides with an existing NewVariable, the second one
gets a `_2`, `_3`... suffix appended (counter increments until unique).

**Parameters**:
- `path` (required)
- `dry_run` (default `true`) — preview-only when true; only `dry_run=false`
  actually renames.
- `exclude` (string array) — variable names to skip entirely.

**Returns**: `{dry_run, total_variables, rename_needed, conflicts_resolved,
mapping[{old, new, reason}], skipped{excluded, already_clean}, compiled}`.

`reason` is a comma-separated list drawn from
`{spaces, special_chars, non_ascii, transliteration, number_prefix,
cpp_keyword, truncated, empty_after_sanitize}`.

### `bp.set_variable_type`

Re-types an existing BP member variable. Wraps
`FBlueprintEditorUtils::ChangeMemberVariableType`. Type strings match
`bp.add_variable` (`bool` / `int` / `real` / `string` / `name` / `text` /
`object` / `class` / `struct` / `byte`). `type_object` resolves a
UClass/UStruct path for reference types. `is_array=true` flips the
container flag.

**Designed to fix the variable-type silent degrade** that follows a
Component reparent (BP child class → C++ parent class). When the
post-reparent type is wrong, this is the explicit fix path. It's also
the only sage way to flip a variable from single → array (or vice
versa) without delete-and-recreate.

**Validation**:
- Variable must be a NewVariable (inherited rejects)
- Type string + `type_object` must resolve via the shared `MakePinType` helper
- Same type → `{already: true}`

**Returns**: `{variable, old_type, new_type, default_value_preserved,
references_refreshed, compiled, [already]}`. `old_type`/`new_type` are
human-readable (`"object:USuperheroFlightComponent"`).

### `bp.clear_graphs`

Empties every K2Node out of a Blueprint's event/function/macro graphs
while preserving graph shells. Step 6 of the pipeline.

```
scope ∈ {all, event_graph, functions, macros}
keep_entry_nodes  default true   # preserve K2Node_FunctionEntry/Result/Tunnel
keep_event_entries default false  # preserve K2Node_Event placeholders
```

Recurses into composite/sub-graphs. The graph objects themselves are not
removed — only their node contents — so the BP's function signatures
survive even when their bodies don't.

**Why it's safer than reparent-first**: with no K2Nodes left, there are
no pins to go stale, so the engine's "in-use pin no longer exists"
errors become structurally unreachable.

**Returns**: `{graphs_visited, nodes_removed, entry_nodes_kept, scope,
summary{function_graphs, macro_graphs, ubergraph_pages}, compiled}`.

### `bp.delete_all_variables`

Bulk `RemoveMemberVariable` across `BP->NewVariables`. Inherited and
SCS-component-generated variables are not in `NewVariables` and remain
untouched.

**Parameters**:
- `path`
- `except` (string array) — names to keep
- `dry_run` (default `false`)

**Returns**: `{deleted[], kept{excepted, inherited, scs_generated},
compiled}`. `inherited` and `scs_generated` are reported as empty arrays
today (they never get into the candidate set), but the response shape
reserves the slots for future filtering and for caller observability.

---

## Variable naming edge cases

Tested against `BP_ConversionTest` (HeroFlight, 2026-05-01). All 19
variants compiled inside UE BP — none rejected — even though most are
not valid C++ identifiers. Sage sanitisation maps each to a deterministic
C++-safe name; reasons listed below match what `bp.sanitize_variable_names`
emits.

| BP FName (literal) | Sanitised → | Reason(s) |
|---|---|---|
| `Variable 01 Black` | `Variable_01_Black` | spaces |
| `100Damage` | `_100Damage` | number_prefix |
| `H\u0131z` | `Hiz` | transliteration |
| `X-Position` | `X_Position` | special_chars |
| `class` | `class_Var` | cpp_keyword |
| `Speed (m/s)` | `Speed_m_s` | spaces, special_chars |
| `\U0001F680Rocket` | `Rocket` | non_ascii |
| `auto` | `auto_Var` | cpp_keyword |
| `Damage(per sec)` | `Damage_per_sec` | spaces, special_chars |
| `\u00C7okT\u00FCrkceBir\u00D6rnekValue` | `CokTurkceBirOrnekValue` | transliteration |

Variables already valid C++ identifiers (`SimpleBool`, `bIsActive`,
`IntArray`, `VectorStruct`, `FlightTypeEnum`, `ComponentObj`,
`ComponentClass`, `StringArray`, the long all-ASCII test name) skip
unchanged. Container types (`is_array=true`) and structured types
(`type_object` paths) round-trip through `MakePinType` without loss.

**Naming rules in summary**:
- C++ identifier regex: `^[A-Za-z_][A-Za-z0-9_]*$`
- Non-ASCII: transliterated when a known mapping exists, replaced with `_` otherwise
- C++ reserved words: 65+ keywords from the C++17/20/UE-namespaced set —
  `auto`, `class`, `register`, `template`, `operator`, `public`,
  `private`, `protected`, `friend`, `mutable`, `extern`, `constexpr`,
  `decltype`, `noexcept`, `co_await`, `co_return`, `co_yield`,
  `requires`, ... See `GetCppReservedWords()` in
  `SageBlueprintTools.cpp` for the full list.
- Length cap: 128 chars, applied uniformly by both `bp.rename_variable`
  and `bp.sanitize_variable_names`.

---

## Recovery patterns

Validation fails at step 8. Here's the decision ladder:

```
Did you call save_assets yet?
├── No   → editor_undo (one transaction back)
│         OR editor close-without-save → restart → asset returns to disk state
└── Yes  → Saved/Autosaves/Game/.../<BP>_AutoN.uasset present?
          ├── Yes → copy current state to artifacts/backups/<date>-<reason>/
          │         cp autosave to Content/...
          │         editor restart → retry from step 7
          └── No  → reconstruct from dump (step 1)
                    bp.import_nodes_t3d for each function's T3D blob
                    bp.connect_pins per the dump's connection graph
                    expect this to take longer than redoing the conversion
```

UE's Autosave interval is 10 minutes by default. **Editor restart resets
Autosave**, so the recovery ladder degrades fast — back up before
restarting the editor mid-conversion.

---

## Variable type silent degrade — the canonical case

Recorded as a lesson because this is what cost 4 hours in the HeroFlight
incident.

**Setup**: BP_SuperheroFlightComponent (BP, 33 user-defined functions,
parent = `UActorComponent`). BP_Player_UE5 has it as an SCS component.
ABP_Player_UE5 (AnimBP) holds a `SuperheroFlightComponent` member
variable typed `BP_SuperheroFlightComponent_C` (the BP's generated class)
and calls 17 of its functions.

**Action**: reparent BP_SuperheroFlightComponent → `USuperheroFlightComponent`
(a new C++ parent that adds no functions, only properties).

**Effect**: ABP_Player_UE5's `SuperheroFlightComponent` member variable
silently retypes from `BP_SuperheroFlightComponent_C` to
`USuperheroFlightComponent` (the new parent, since the old class chain
collapsed by one). Every `K2Node_CallFunction` that was calling a
BP-defined function now sees:

- Variable Get pin: `USuperheroFlightComponent*` (parent C++)
- CallFunction Target pin: `BP_SuperheroFlightComponent_C*` (BP child,
  function-defining class)

Subclass→parent assignment is fine; the reverse isn't, so the connection
fails validation: *"BP Superhero Flight Component is not compatible with
Superhero Flight Component."*

**Two correct fixes**:

1. **Finish moving the Component's BP functions into C++ first.** Then
   the CallFunction targets are also `USuperheroFlightComponent` and
   the type chain is consistent. This is what the pipeline's step 3 gate
   ("deps ready") was supposed to catch.

2. **Re-set the variable type explicitly.** When (1) isn't ready yet,
   `bp.set_variable_type` on the dependent BP forces the variable back
   to `BP_SuperheroFlightComponent_C`, restoring CallFunction
   compatibility. This stops being needed once (1) lands.

**The wrong fix**: skipping step 6 EMPTY and trying to refresh the
already-orphaned graphs. That's how 17 disconnected `Target` pins turn
into 60+ "Can't connect pins" errors when the orphan migration moves
links across an incompatible type boundary.

---

## Pre-flight checklist (drop-in, paste at top of a conversion task)

```
☐ bp.full_dump path=… include_t3d=true output_path=Saved/SageDumps/<BP>/
☐ bp.read path=…           # log baseline parent_class + counts
☐ bp.list_variables path=… # log baseline NewVariables
☐ bp.read_components path=… # log SCS hierarchy
☐ bp.get_dependencies path=… # log downstream BPs/levels — these need step 3 review

☐ Parent C++ class compiled and present in HEROFLIGHTGAME_API
☐ Parent's UPROPERTYs match BP variable FNames (or step 6 sanitize will rename them)
☐ Build SUCCESSFUL since last C++ change

☐ Editor up + sage handshake (list_editors count==1)
☐ Saved/Autosaves of this BP exist (recovery cushion)
```

---

_Document origin: HeroFlight project, BP→C++ migration, May 2026.
Each tool listed here exists because of a specific failure mode the
pipeline encountered. Future failure modes will earn their own steps,
gates, or tools — and will be added to this document, not worked around._
