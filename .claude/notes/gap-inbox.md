# Sage Gap Inbox

Test eden Claude session'ları (Lyra / HeroFlight / diğer projeler) Sage MCP tool'larında bulduğu eksik / hatalı / kullanılmaz davranışları **buraya yazar**. Sage Claude (geliştirici) ara ara kontrol eder, triage yapar, fix uygular.

## Path

```
D:\Steamworks\sage-unreal-mcp\.claude\notes\gap-inbox.md
```

Tüm test eden Claude'ların bu yola write erişimi olmalı. Yeni bir test projesi açtıysan, o projenin `CLAUDE.md` dosyasına bu yolu hatırlat.

## Workflow

| Aktör | Görev |
|---|---|
| **Test eden Claude** | Gap bulunca `## Inbox`'ın en üstüne yeni bir entry ekler. Status `🆕 OPEN` ile başlar |
| **Sage Claude** | Session başında veya Mahmut'tan istek gelince inbox'ı tarar. OPEN entry'leri triage eder, fix uygular, status günceller |
| **Mahmut** | Çatışan/şüpheli durumlarda triage'i yönlendirir, manuel close yapabilir |

## Status legend

- 🆕 **OPEN** — yeni rapor, henüz triage edilmedi
- 🔧 **IN-PROGRESS** — Sage Claude fix üzerinde çalışıyor
- ✅ **FIXED** — fix uygulandı, deploy bekliyor veya in-engine verify bekliyor
- 🔒 **CLOSED** — fix verify edildi (test eden Claude doğruladı)
- ❌ **WONTFIX** — kasıtlı, fix yapılmayacak (gerekçe entry'de)
- 🔁 **DUPLICATE** — başka entry'nin kopyası (referans entry'e link)

## Entry template

Yeni gap eklerken şu şablonu kullan (newest-first, `## Inbox` altına ekle):

```markdown
## Gap #N — kısa başlık

- **Status**: 🆕 OPEN
- **Reported**: YYYY-MM-DD HH:MM tarafından <reporter session / project label>
- **Project**: D:\Steamworks\<ProjectName>
- **Editor**: <session_id / label / instance_id varsa>

### Hedef
<Bir cümle — ne yapmaya çalıştın>

### Denenen tool çağrısı
\`\`\`json
{
  "tool": "<tool.name>",
  "args": { ... }
}
\`\`\`

### Sonuç / hata
\`\`\`
MCP error -32602: ...
\`\`\`
(veya "tool sessizce başardı ama yan etki yok" gibi davranış raporu)

### Beklenen davranış
<Bir-iki cümle — tool şu olmalıydı>

### Workaround
<Varsa — bu gap'i bypass etme yolu, manuel adımlar, başka tool kombinasyonu>

### Öneri (öncelik A/B/C)
<Varsa — pseudo-code, ilgili fonksiyon, fix yaklaşımı. A=must, B=should, C=nice-to-have>

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**:
- **Kök sebep**:
- **Fix commit**:
- **Deploy adımı**: (server rebuild / plugin rebuild + install / docs update)
- **Verify durumu**: pending / verified by <reporter>
```

Önemli kural — **rapor edenden gelen alanlara dokunma** (Hedef, Denenen, Sonuç/hata, Beklenen, Workaround, Öneri). Sadece son `### Sage Claude triage` blokunu doldur, status field'ını güncelle.

## Sage Claude triage rehberi

1. **Inbox tarama**: en yeni OPEN entry'den başla
2. **Çoğaltıcı kontrol**: aynı tool / aynı hata mesajı zaten kapanmış mı? (`Closed log` bölümüne bak) — varsa DUPLICATE işaretle, mevcut entry'e link
3. **Reproduce**: mümkünse server-side smoke test (`.cdb-stdin.txt` pattern'i veya `tools/call` direct invoke) ile lokalde reproduce et
4. **Kök sebep**:
   - Schema bug (nlohmann brace-init pitfall, Zod fail) → server-side fix
   - Plugin handler bug (UE API yanlış kullanımı) → plugin fix
   - UE API edge case (native vs SCS, reflection eksikliği) → plugin fix + lessons.md güncelle
   - Multi-editor routing (`_editor` parametresi) → ADR-017'ye bak
5. **Fix + commit**:
   - Server fix → `cmake --build --preset debug --target sage-server` + manuel smoke
   - Plugin fix → `scripts/build-plugin.ps1` + 3 şey kopyala (uplugin + Source + Binaries) + editor restart
6. **Status güncelle**: FIXED + Verify durumu pending
7. **Reporter'a bildir**: Mahmut'tan rica et, "Lyra Claude'a Gap #N fixed denir" formatında
8. **Verify gelince**: status CLOSED, entry'i `## Closed log` bölümüne taşı (newest-on-top)

## Lessons capture

Bir gap fix'lendikten sonra **non-obvious bir öğreti** çıktıysa (`obj({})` null pitfall, ACharacter native CDO subobject, BPGC SCS chain, vs.) `.claude/notes/lessons.md` dosyasına da bir kayıt ekle. Aynı hata yine yapılmasın diye.

---

## Inbox (newest-first)

<!-- Yeni gap entry'leri buraya. En üste yeni geleni koy. -->

## Gap #21 — AnimBlueprint state-machine transition rule authoring eksik

- **Status**: ✅ FIXED — deploy/verify pending
- **Reported**: 2026-05-04 22:36 tarafından Codex / Lyra FlightCore integration
- **Project**: D:\Steamworks\Lyra
- **Editor**: Lyra@bb1b818d, UE 5.7.4

### Hedef
`/FlightCore/Animations/ABP_FlightCore` içinde gerçek `Idle -> HoverMove -> FastMove` flight locomotion state machine kurmak; initial state ve transition `Can Enter Transition` graphlarını MCP üzerinden yazabilmek.

### Denenen tool çağrısı
```json
{
  "tool": "animation.set_state_machine_initial_state",
  "args": {
    "_editor": "Lyra@bb1b818d",
    "path": "/FlightCore/Animations/ABP_FlightCore",
    "state_machine_name": "FlightLocomotion",
    "state_id": "C497EEF74C76A3040440D9BEFF2DF87A"
  }
}
```

```json
{
  "tool": "animation.set_transition_rule",
  "args": {
    "_editor": "Lyra@bb1b818d",
    "path": "/FlightCore/Animations/ABP_FlightCore",
    "state_machine_name": "FlightLocomotion",
    "transition_id": "FE5DE4E443D5257EA131ABAFCC6BB5D4",
    "expression": "true"
  }
}
```

### Sonuç / hata
```text
animation.set_state_machine_initial_state -> MCP error -32603: pin lookup failed
animation.set_transition_rule -> schema/tool description: [NOT IMPLEMENTED] Boolean expression authoring inside transition's BoundGraph
```

Ek doğrulama:
- `animation.add_transition` yalnızca state/transition node bağlantısı kuruyor; transition `BoundGraph` içindeki `Can Enter Transition` bool rule yazılamıyor.
- `animation.set_transition_priority` ve `animation.set_transition_blend` var ama rule compile warning'ini çözmüyor.
- `ABP_FlightCore` önce 6 warning üretiyordu:
  - Entry node `FlightLocomotion` is not connected to state
  - `Idle to HoverMove` / `HoverMove to Idle` / `HoverMove to FastMove` / `FastMove to HoverMove` will never be taken
  - There was no entry state connection in `FlightLocomotion`

### Beklenen davranış
Sage, UE 5.7 AnimBlueprint state-machine graphlarını tam author edebilmeli:
- Entry node'u seçilen state'e güvenilir bağlayabilmeli.
- Transition `BoundGraph` içinde `Can Enter Transition` pinine bool rule graphı yazabilmeli.
- Minimum ilk sürümde `expression: "true"` ve `expression: "false"` compile warning üretmeden çalışmalı.
- İkinci sürümde variable getter + basit compare/boolean expression desteklenmeli.

### Workaround
Geçici compile-clean için gerçek state machine root node'u kaldırıldı ve root `AnimGraph`, `A_Flight_Idle_A` sequence player ile doğrudan `Output Pose`'a bağlandı.

Workaround sonucu:
```text
/FlightCore/Animations/ABP_FlightCore
bp.validate: valid=true, error_count=0, warning_count=0
```

Bu yalnızca compile-clean geçici çözümdür; gerçek flight locomotion değildir. Full `Idle/HoverMove/FastMove/A-E` state machine kurulumu bu gap kapanmadan MCP ile sürdürülemez.

### Öneri (öncelik A/B/C)

**A — must**
1. `animation.set_state_machine_initial_state` UE 5.7 fix:
   - `UAnimStateEntryNode` output pin ve `UAnimStateNodeBase` input pin lookup'ı strict pose-pin varsayımına bağlı kalmamalı.
   - `UAnimationStateMachineSchema::TryCreateConnection` üzerinden schema-valid connection kurulmalı.
   - Failure halinde pin dump dönmeli: node class, pin name, direction, category, subcategory.

2. `animation.set_transition_rule` minimum literal implementation:
   - `UAnimStateTransitionNode` GUID ile bulunmalı.
   - `BoundGraph` / `UAnimationTransitionGraph` içindeki `UAnimGraphNode_TransitionResult` bulunmalı.
   - `Can Enter Transition` bool input pinine gerçek bool producer node bağlanmalı; sadece default value set etmek yeterli olmayabilir çünkü compiler “connect something” warning'i veriyor.
   - Desteklenecek expression minimum:
     - `"true"`
     - `"false"`

3. `animation.read_transition_rule` veya graph pin diagnostics:
   - Transition BoundGraph node/pin/link özetini okuyabilmeli.
   - Debug için en az `{transition_id, result_node_id, can_enter_pin, linked_nodes[]}` dönmeli.

**B — should**
4. `animation.set_transition_rule` expression subset:
   - AnimBP variable getter: bool/float/int/enum-like byte.
   - Float compare: `Speed > 10`, `Speed <= 10`.
   - Bool ops: `!bWantsToFlightSprint`, `A && B`, `A || B`.
   - Equality: `FlightType == 0..4`.

5. State-machine cleanup tools:
   - `animation.remove_transition`
   - `animation.remove_state`
   - `animation.remove_state_machine`
   - Bunlar bad graph rebuild sırasında stale/orphan transition uyarılarını temizlemek için gerekli.

**C — nice-to-have**
6. Convenience spec tool:
```json
{
  "tool": "animation.create_state_machine_from_spec",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore",
    "name": "FlightLocomotion",
    "initial_state": "Idle",
    "states": [
      {"name": "Idle", "animation": "/FlightCore/.../Idle/A_Flight_Idle_A"},
      {"name": "HoverMove", "animation": "/FlightCore/.../HoverMove/A_Flight_HoverMove_A"},
      {"name": "FastMove", "animation": "/FlightCore/.../FastMove/A_Flight_FastMove_A"}
    ],
    "transitions": [
      {"from": "Idle", "to": "HoverMove", "rule": "GroundSpeed > 10"},
      {"from": "HoverMove", "to": "Idle", "rule": "GroundSpeed <= 10"},
      {"from": "HoverMove", "to": "FastMove", "rule": "bWantsToFlightSprint"},
      {"from": "FastMove", "to": "HoverMove", "rule": "!bWantsToFlightSprint"}
    ]
  }
}
```

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-05
- **Kök sebep**: `animation.set_state_machine_initial_state` UE 5.7 entry/state-machine pin modelini strict pose-pin lookup ile ele alıyordu; `animation.set_transition_rule` ise transition `BoundGraph` içindeki `bCanEnterTransition` K2 rule graphını yazmıyordu.
- **Fix commit**: pending (working dir: `SageAnimationTools.cpp`, `phase4_schemas.cpp`)
- **Deploy adımı**: ✅ `scripts/build-all.ps1` server wrapper OK (`sage-server`, no work to do / mevcut binary); ✅ `scripts/build-all.ps1 -SkipServer` plugin UAT BuildPlugin OK; Lyra plugin binary/source install + editor enable/restart bekliyor.
- **Verify durumu**: pending — Codex/Lyra `ABP_FlightCore` full state machine authoring verify bekliyor. Yeni destek: literal bool, AnimBP variable getter, numeric compare, bool equality, string/name equality, bool AND/OR/NOT; hem string shorthand hem JSON AST.

## Closed log (newest-first)

<!-- CLOSED entry'ler buraya. Newest-on-top. -->

### Sweep — Phase 4-r6 anim suite + 13-agent code review (2026-05-04, commit f1112b3)

Tek commit'te 7 inbox gap + 1 silent regression closed; tool count 478 → 551 (+72 net new tool).

| Gap | Status | Closure mechanism |
|---|---|---|
| #20 — anim tool roadmap (master tracking) | ✅ FIXED | 78 yeni tool 10 cluster'da: A (8 primitive) + B (11 convenience) + C (5 stub→real) + C ek (6 real + 2 stub) + D (6) + E (4 + 3 stub) + F (3 + 3 stub) + G (5 stub) + H (3 + 4 stub) + I (4 + 2 stub) + J (8 runtime) |
| #19 — character.play_root_motion_source | ✅ FIXED | Cluster J: ConstantForce + JumpForce + RadialForce + MoveToForce + curve support + sensitive_liftoff_check + NEW character.remove_root_motion_source |
| #18 — animation.create_anim_notify(_state) | ✅ FIXED | Cluster D: UBlueprintFactory ParentClass=UAnimNotify/UAnimNotifyState |
| #17 — animation.add/set_blendspace_sample(s) | ✅ FIXED | Cluster E: AddSample int32 INDEX_NONE check + DeleteSample reverse iter + structured skipped entries |
| #16 — state machine handler suite | ✅ FIXED | Cluster C real impl: PostPlacedNewNode pipeline (BoundGraph + SubGraphs registration), RenameGraphWithSuggestion (no silent _0 collision), AssetPlayerBase widening, StateResult/Root output sink |
| #15 — gameplay.set_world_game_mode mismatch | ✅ FIXED | Schema rename **game_mode → game_mode_class** + add **confirmed: true** required |
| #14 — level.create Niagara crash | ✅ FIXED | UWorld::CreateWorld(EWorldType::Editor) + InitializationValues(FX/AI/Nav/Trace) + UpdateWorldComponents + confirmed gate |
| #9 regression (silent) — project.add_module_dependency substring match | ✅ FIXED | Exact-quoted entry parser; **Core** artık **CoreUObject** ile çakışmıyor |

#### Cross-cutting hardening (170+ findings across 13 agents)

- **15 destructive ops** artık **confirmed: true** gerektiriyor (delete_actor, delete_asset, asset.delete_batch, asset.reload_package, asset.fixup_redirectors, bp.delete_variable, bp.delete_function, level.create, gameplay.set_world_game_mode, editor.build_all/geometry/lighting/hlod, project.set_plugin_enabled, project.write_cpp_file).
- **bp.full_dump arbitrary path sandbox escape closed** (CollapseRelativeDirectories + ProjRoot guard).
- **23 silent stub'lar artık \[NOT IMPLEMENTED\] -32601** dönüyor (silent-success anti-pattern temizliği per lessons.md §8).
- **mat.disconnect REAL impl** (eskiden silent no-op idi).
- **MatTransactions per-Material map** (eskiden process-global static; ADR-017 multi-editor corruption riski).
- **widget.run_utility_widget** + **seq.add_keyframe** REAL impl (stub'tan gerçek implementasyona).
- **Eski animation tools 11 schema/handler param rename** (skeleton_path → skeleton, bone_name → bone, blend_in_time → blend_in, vb. — Lyra Gap #15 pattern protection).
- **AtomicWriteString Move→Copy** backup discipline (PatchUProjectAddModule + ProjectCreateCppClassImpl crash-safe).
- **editor.run_python \_security\_warning** field per response (Phase 5+ auth gate awareness).
- **6 silent-success stubs → -32000 honest errors** (SetBoneKeyframes, SetMontageSequence, BakeRootMotionFromBone, SetPoseSearchSchema, AddPoseSearchSequence, BuildPoseSearchIndex).
- **Yeni response field konvansiyonu**: \_warning, \_skip\_reason, \_security\_warning, \_perf\_warning, \_compile\_warning, kind enum, removed\_count, mapping\_index, requires\_editor\_restart.

#### Verification
- Server build: cmake debug clean (4-step incremental)
- Plugin build: UAT BuildPlugin Win64 OK (~41s, .dll 2.55MB → 2.81MB)
- Tool count: 478 → 551 (smoke tools/list verify)
- Null/non-object property: 0 (Anthropic SDK Zod-clean)
- Schema/handler critical alignment: 10/10 OK
- Confirmed gates: 15/15 destructive ops gated
- HeroFlight in-engine test: bridge handshake OK, Cluster A list_animgraph_nodes 29 nodes verified, Cluster C ek list_states 4 states verified, schema fix gates verified

#### Lessons learned (lessons.md candidates)
- UE 5.7 **FAnimNode_SequencePlayer::bLoopAnimation** + **PlayRate** protected (was public 5.6); direct-write erişim için reflection veya engine accessor gerek.
- **UAnimGraphNode_StateMachineBase::PostPlacedNewNode** kanonik pipeline state machine sub-graph + Entry node + ParentGraph->SubGraphs registration üretir; manuel **FBlueprintEditorUtils::CreateNewGraph** + node spawn engine bookkeeping atlar.
- **UAnimStateNode::BoundGraph** PostPlacedNewNode'da yaratılır, AllocateDefaultPins'te DEĞİL (silent BoundGraph=null trap).
- **IsPosePin** filtresi hem **FPoseLink** hem **FComponentSpacePoseLink** struct'ını kabul etmeli (SkeletalControl türevleri için).
- **UAnimGraphNode_Root** (root AnimGraph) vs **UAnimGraphNode_StateResult** (state BoundGraph) — output sink resolver iki sınıfı da match etmeli.
- **obj({}) → JSON null** (lessons.md §4 reaffirmed); empty schema için **nlohmann::json::object()** kullan.


### Gap #13 — bp.override_inherited_component_class native component için fail

- **Status**: ✅ FIXED — verify pending
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 4 dogfooding)
- **Project**: D:\Steamworks\Lyra
- **Editor**: B_Hero_HeroFlight + B_Hero_ShooterFlight CharMoveComp override

#### Hedef
ACharacter::CharMoveComp class override (native CharacterMovementComponent → UFlightCoreCharacterMovementComponent), `UBlueprint::ComponentClassOverrides` array üzerinden engine pattern.

#### Denenen tool çağrısı
```json
{
  "tool": "bp.override_inherited_component_class",
  "args": {
    "path": "/Game/.../B_Hero_HeroFlight",
    "component": "CharMoveComp",
    "new_class": "/Script/FlightCore.FlightCoreCharacterMovementComponent"
  }
}
```

#### Sonuç / hata
```
MCP error -32602: no SCS component 'CharMoveComp' on any parent BP of B_Hero_HeroFlight
```

#### Beklenen davranış
ACharacter native components (`CharMoveComp`, `CharacterMesh0`, `CollisionCylinder`) `UBlueprint::ComponentClassOverrides` ile override edilebiliyor (TopDownArena B_Hero_Arena.CharMoveComp → UTopDownArenaMovementComponent canonical pattern). Tool bunu desteklemeliydi.

#### Workaround
Yok. ComponentClassOverrides'ı manuel editlemek mümkün ama tool olmadan asset.set_property ile array'e direct yazma sancılı.

#### Öneri (A — must)
Pseudo-code:
```cpp
USCS_Node* Node = FindInheritedSCSNode(ParentBP, ComponentName);
if (!Node) {
    UObject* CDO = ParentBP->GeneratedClass->GetDefaultObject();
    if (UActorComponent* NativeComp = FindObject<UActorComponent>(CDO, *ComponentName)) {
        ParentComponentClass = NativeComp->GetClass();
    }
}
```

---

#### Sage Claude triage

- **Triage tarihi**: 2026-05-04 ~01:00
- **Kök sebep**: `FindParentSCSComponentClass` sadece BP-side `USimpleConstructionScript` chain'ini tarıyordu; native AActor subclass'ların `CreateDefaultSubobject` ile kurduğu component'lar USCS_Node değil, parent class CDO'sunun named subobject'i (`CDO->GetDefaultSubobjects()` üzerinden erişilir).
- **Fix commit**: pending (working dir, `plugin/Source/SageBridge/Private/Tools/SageBlueprintTools.cpp:4505` — Layer 2 native CDO subobjects fallback)
- **Deploy adımı**:
  - ✅ `scripts/build-plugin.ps1` (UAT BuildPlugin Win64, ~45s, 2026-05-04 01:25)
  - ✅ Lyra `Plugins/SageBridge/{Source, Binaries/Win64, .uplugin}` mirror + Intermediate purge (2026-05-04 ~01:27)
  - ✅ HeroFlight `Plugins/SageBridge/{Source, Binaries/Win64, .uplugin}` mirror + Intermediate purge (2026-05-04 ~01:30)
- **Verify durumu**: pending — Lyra editor restart + `bp.override_inherited_component_class` çağrısı bekliyor

#### Lessons (eklemeli)
- AActor native components (CharacterMovementComponent, MeshComponent, CapsuleComponent, etc.) parent class CDO'sunda named subobject olarak yaşar. Inherited component lookup yapan herhangi bir tool BP-SCS'ye ek olarak parent CDO'nun `GetDefaultSubobjects()`'ini de tarayabilmeli.

---

### Gap #pre-13 — tools/list 478 tool yerine boş geliyor (Anthropic SDK Zod reject)

- **Status**: 🔒 CLOSED — verified
- **Reported**: 2026-05-04 by Lyra Claude
- **Project**: D:\Steamworks\Lyra

#### Hedef
Sage tool'ları Lyra Claude registry'sine yüklensin.

#### Sonuç / hata
ToolSearch sıfır match, ListMcpResourcesTool sıfır resource. tools/list "boş array veya timeout" görünüyordu.

#### Beklenen davranış
478 tool registry'de listeli olmalı.

---

#### Sage Claude triage

- **Triage tarihi**: 2026-05-04 ~00:55
- **Kök sebep**: 9d7380b commit'inde `phase4_schemas.cpp:1486` `{"element_value",{}}` — nlohmann brace-init `{}` argümanını JSON null'a init ediyor (lessons.md §4 tam bunu yazıyor, yine yapılmış). Sonuç: `asset.add_array_element` schema'sının `properties.element_value`'su `null` olarak yayınlanıyor. JSON Schema spec'e göre property value null olamaz → Anthropic SDK Zod validation tüm tools/list'i reject ediyor → registry boş.
- **Fix commit**: pending (working dir, tek satır `{}` → `nlohmann::json::object()`)
- **Deploy adımı**: ✅ server rebuild (sage-server.exe 01:04 build) + manual smoke (478 tool, sıfır null property)
- **Verify durumu**: ✅ Lyra Claude /mcp reconnect sonrası tools/list 478 dönüyor, FlightCore Phase 4 dogfooding devam edebildi (Gap #10/#11/#12 verify'a kadar yürüdü)

#### Lessons capture
- Lessons.md §4 yazılı kuralın yine ihlal edildiği bir vaka. Single-tool review'da gözden kaçmış. **Kanonik fix**: `obj()` helper'ında her property value'nun `is_null()` olup olmadığını assert et — null çıkıyorsa derlemeye katılan code'da brace-init pitfall var demektir, build-time fail edilebilir.
