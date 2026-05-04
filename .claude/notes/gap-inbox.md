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

## Gap #20 — Animation tool suite kapsamlı yol haritası (AnimGraph + State Machine + Notify + BlendSpace + Skeleton + Runtime)

- **Status**: 🔧 IN-PROGRESS — Sage Claude triage + scoping
- **Reported**: 2026-05-04 by Sage Claude (Lyra Claude #16/#17/#18/#19 raporlarının üstüne kapsamlı surface çıkarma)
- **Project**: D:\Steamworks\sage-unreal-mcp (server + plugin)
- **Affected domain**: `animation.*` namespace + `character.*` runtime (yeni)

### Arka plan — Reality check (2026-05-04)

Sage'in 478 tool'undan 45'i `animation.*` namespace'inde, ama **handler implementation matrix**:

| Tool | Schema | Handler | Durum |
|---|---|---|---|
| `animation.create_anim_blueprint` | ✓ | ✓ | gerçek (CommonAIExport AIAnimBlueprintBuilder kullanıyor) |
| `animation.create_montage` / `create_sequence` / `create_blendspace` / `create_composite` / `create_ik_rig` / `create_ik_retargeter` | ✓ | ✓ | asset yaratır, içerik boş |
| `animation.create_state_machine` | ✓ | **STUB** | sadece `note` döndürüyor (Lyra Gap #16) |
| `animation.add_state` | ✓ | **STUB** | sadece `note` |
| `animation.add_transition` | ✓ | **STUB** | sadece `note` |
| `animation.set_state_animation` | ✓ | **STUB** | sadece `note` |
| `animation.set_transition_blend` | ✓ | **STUB** | sadece `note` |
| `animation.add_curve` | ✓ | **YARIM** | `modified=false` + `note: requires IAnimationDataController::AddCurve` |
| `animation.add_notify` | ✓ | **YARIM** | existing UAnimNotify class'ı sequence'a ekler, yeni class yaratmaz (Lyra Gap #18) |
| `animation.create_blendspace` | ✓ | yarım | asset yaratır, sample point ekleme yok (Lyra Gap #17) |
| `animation.read_*` (15 tool) | ✓ | ✓ | hepsi gerçek |
| `animation.set_montage_*` / `add_montage_section` / `set_montage_slot` / `set_root_motion` / etc. | ✓ | ✓ | gerçek |
| `animation.add_virtual_bone` / `remove_virtual_bone` / `set_anim_blueprint_skeleton` | ✓ | ✓ | gerçek |
| `animation.create_pose_search_*` / `set_pose_search_schema` / `build_pose_search_index` | ✓ | ✓ | gerçek |

**Net problem**: AnimBP authoring'in en kritik adımı — **AnimGraph + State Machine programlanabilir hale getirmek** — Sage'de yok. Persona'da elle yapmak Sage workflow'unu kıyor.

CommonAIExport AnimBP builder header'ında tek cümle: `AnimGraph node manipulation is OUT OF SCOPE (requires Persona reimplementation)`. Sage'in ticari moat'ının yarısı bu kapıdan giriyor — Sage **bu sınırı aşmalı**.

### Scope — Lyra Claude raporları + ek surface

Lyra Claude 4 entry açtı:
- **Gap #16** — state machine handler suite stub → real (5 tool)
- **Gap #17** — `animation.add_blendspace_sample` / `set_blendspace_samples` (yeni tool)
- **Gap #18** — `animation.create_anim_notify` / `create_anim_notify_state` (yeni 2 tool)
- **Gap #19** — `character.play_root_motion_source` (yeni runtime tool)

Bu entry **bunları kapsayan + ek ~60 tool öneren** bir yol haritası. Implementation **fazla iş** ama bütün domain bir kerede tasarlanırsa ergonomi ve tutarlılık çok daha iyi.

---

### Yeni tool önerileri (10 cluster)

#### Cluster A — AnimGraph node creation core (P0, must)

**Lyra Gap #16'nın gerçek temeli.** Generic node CRUD yoksa diğer her şey workaround.

```
animation.add_animgraph_node(anim_bp, graph_name, node_class, x, y, properties?, _editor?)
    → { node_id }
animation.remove_animgraph_node(anim_bp, graph_name, node_id, _editor?)
    → { removed }
animation.connect_pose_pin(anim_bp, graph_name, from_node_id, from_pin, to_node_id, to_pin, _editor?)
    → { connected }
animation.disconnect_pose_pin(anim_bp, graph_name, from_node_id, from_pin, to_node_id, to_pin, _editor?)
    → { disconnected }
animation.set_anim_node_property(anim_bp, graph_name, node_id, property_path, value, _editor?)
    → { set: true }                 # set FAnimNode_SequencePlayer.Sequence, Rate, etc.
animation.bind_anim_node_property(anim_bp, graph_name, node_id, property_path, variable_name, _editor?)
    → { bound }                     # property→AnimBP variable binding (Alpha→bIsFlying)
animation.list_animgraph_nodes(anim_bp, graph_name, _editor?)
    → { nodes: [{ id, class, position, properties_summary }] }
animation.set_animgraph_root_pose(anim_bp, root_node_id, _editor?)
    → { connected }                 # AnimGraph Output Pose ← root_node
```

**Engine API**:
- `FBlueprintEditorUtils::AddNode` veya `FEdGraphUtilities::CreateNodesFromTemplate`
- `UEdGraphSchema_K2::TryCreateConnection` (anim graph K2 schema'dan inherit)
- `FAnimGraphNodeFactory::CreateNode` — anim-specific factory
- Property binding: `UAnimGraphNode_Base::PropertyBindings` map

**Test senaryosu**: ABP_FlightCore.AnimGraph'a tek bir SequencePlayer ekle, `Sequence` property'sini A_Idle'a set et, root Output Pose'a connect et, compile et → editor'de açılınca asset düzgün görünmeli.

---

#### Cluster B — AnimGraph node convenience (P0)

Hızlı kullanım için yaygın node'lara doğrudan tool. Hepsi internal'da Cluster A'yı çağırır.

```
animation.add_sequence_player(anim_bp, graph, sequence, x, y, loop=true, rate=1.0)
    → { node_id }
animation.add_blendspace_player(anim_bp, graph, blendspace, x, y, x_axis_var?, y_axis_var?)
    → { node_id }                                 # property bindings auto-set
animation.add_state_machine_node(anim_bp, graph, state_machine_name, x, y)
    → { node_id }                                 # AnimGraph içine SM reference
animation.add_blend_list_by_bool(anim_bp, graph, x, y, condition_var?, blend_time?)
    → { node_id, true_pin, false_pin }
animation.add_blend_list_by_enum(anim_bp, graph, enum_class, x, y)
    → { node_id, pins: [...] }
animation.add_layered_blend_per_bone(anim_bp, graph, x, y, bone_filter)
    → { node_id, base_pose_pin, blend_poses_pins }   # upper/lower body split
animation.add_apply_additive(anim_bp, graph, x, y, additive_pose_var?)
    → { node_id, base_pose_pin, additive_pose_pin }
animation.add_two_bone_ik(anim_bp, graph, x, y, ik_bone, effector)
    → { node_id }
animation.add_skeletal_control_node(anim_bp, graph, control_class, x, y)
    → { node_id }                                 # generic FAnimNode_SkeletalControlBase derivative
animation.add_play_montage_notify_window(anim_bp, graph, slot_name, x, y)
    → { node_id }                                 # slot anim graph hookup
animation.add_link_anim_layer(anim_bp, graph, layer_interface_function, x, y)
    → { node_id }                                 # linked anim layer call
```

**Tasarım kararı**: Lyra-style — node ekleyince auto-connect default pose pinleri yapma; sadece node ID dönüş + pin liste; connection ayrı tool. Idempotency kolaylaşır.

---

#### Cluster C — State machine deep CRUD (P0, Lyra Gap #16 detail)

Lyra'nın stubları + ek state machine surface.

```
animation.create_state_machine(anim_bp, name, _editor?)
    → { state_machine_path, entry_state }
animation.add_state(state_machine_path, name, x, y, _editor?)
    → { state_id }
animation.add_conduit(state_machine_path, name, x, y, _editor?)
    → { conduit_id }
animation.add_state_alias(state_machine_path, name, aliased_states, x, y, _editor?)
    → { alias_id }                                 # UE 5.0+ multi-source transition
animation.set_state_machine_initial_state(state_machine_path, state_id, _editor?)
    → { changed }
animation.add_transition(state_machine_path, from_state_id, to_state_id, _editor?)
    → { transition_id }
animation.set_transition_priority(transition_id, priority, _editor?)
    → { set }                                      # int — lower = higher priority
animation.set_transition_blend(transition_id, blend_time, blend_function, _editor?)
    → { set }                                      # blend_function ∈ Linear|Cubic|Ease|Custom
animation.set_transition_rule(transition_id, expression?, source_blueprint_path?, _editor?)
    → { set }                                      # bool expression OR transition graph
animation.set_state_animation(state_id, sequence_or_blendspace, _editor?)
    → { set }                                      # convenience: state'in single sequence player'ını swap
animation.set_state_entered_event(state_id, custom_event_name, _editor?)
    → { wired }                                    # OnStateEntered/Exited Event Graph hook
animation.list_states(state_machine_path, _editor?)
    → { states: [{ id, name, transitions, animation_summary }] }
animation.list_transitions(state_machine_path, _editor?)
    → { transitions: [...] }
```

**Engine API**:
- `FBlueprintEditorUtils::AddNewState` / `AddNewTransition`
- `UAnimStateNodeBase::BoundGraph` → child sub-graph for entered/exited logic
- `UAnimStateTransitionNode::CrossfadeDuration`, `BlendMode`, `CustomBlendCurve`

**Test senaryosu**: State machine yarat ("FlightStates"), 3 state ekle (Idle, HoverMove, FastMove), Idle→HoverMove transition (bIsMoving > 0.1), HoverMove→FastMove (Speed > 800), state machine'i AnimGraph root'a connect et, compile.

---

#### Cluster D — AnimNotify class authoring (Lyra Gap #18 + ek)

```
animation.create_anim_notify(path, name, parent_class="/Script/Engine.AnimNotify", _editor?)
    → { asset_path }                               # UAnimNotify subclass BP
animation.create_anim_notify_state(path, name, parent_class="/Script/Engine.AnimNotifyState", _editor?)
    → { asset_path }                               # UAnimNotifyState subclass BP
animation.set_anim_notify_color(notify_class_path, rgba, _editor?)
    → { set }                                      # editor track color
animation.set_anim_notify_display_name(notify_class_path, friendly_name, _editor?)
    → { set }
```

**Notify track + instance management** (existing `add_notify` zaten var, ek olarak):

```
animation.add_notify_track(sequence_path, track_name, _editor?)
    → { track_index }                              # multi-track support (existing single track)
animation.list_notifies(sequence_path, _editor?)
    → { notifies: [{ name, time, duration, class, track }] }
animation.remove_notify(sequence_path, notify_index, _editor?)
    → { removed }
animation.set_notify_position(sequence_path, notify_index, time, duration?, _editor?)
    → { set }
animation.add_anim_notify_instance(sequence_path, time, notify_class_path, track_index?, _editor?)
    → { notify_index }                             # mevcut add_notify ile dup; daha explicit isim
```

**Engine API**:
- `FKismetEditorUtilities::CreateBlueprint` (AnimNotify parent)
- `UAnimSequenceBase::AnimNotifyTracks` — track array
- `UAnimSequenceBase::Notifies` — instance array
- `UAnimNotify::NotifyColor` — editor display

---

#### Cluster E — BlendSpace sample + axis ops (Lyra Gap #17 + ek)

```
animation.add_blendspace_sample(blendspace_path, position, animation_path, rate_scale?, _editor?)
    → { sample_index }
animation.set_blendspace_samples(blendspace_path, samples, replace=true, _editor?)
    → { sample_count }                             # bulk
animation.remove_blendspace_sample(blendspace_path, sample_index, _editor?)
    → { removed }
animation.set_blendspace_axis(blendspace_path, axis, name, min, max, grid_divisions, _editor?)
    → { set }                                      # axis ∈ X|Y
animation.set_blendspace_smoothing(blendspace_path, axis, interpolation_speed, _editor?)
    → { set }
animation.set_blendspace_target_weight_interpolation(blendspace_path, time, _editor?)
    → { set }
animation.read_blendspace_samples(blendspace_path, _editor?)
    → { samples: [...], axes: [...] }              # mevcut read_blendspace genişletilmiş
```

**Engine API**:
- `UBlendSpaceBase::AddSamplePoint(animation, position)`
- `UBlendSpaceBase::AddBlendSample` (private, refleksiyonla erişim olabilir)
- `UBlendSpaceBase::BlendParameters[axis]` → FBlendParameter
- 1D BlendSpace: `position` `[float]`; 2D: `[float, float]`

---

#### Cluster F — Sync markers + curve compression (P1)

```
animation.add_sync_marker(sequence_path, marker_name, time, _editor?)
    → { marker_index }
animation.remove_sync_marker(sequence_path, marker_index, _editor?)
    → { removed }
animation.list_sync_markers(sequence_path, _editor?)
    → { markers: [...] }
animation.set_curve_compression(sequence_path, codec, settings?, _editor?)
    → { set }                                      # codec ∈ UniformlySampled|RemoveLinearKeys|...
animation.run_animation_modifier(sequence_path, modifier_class_path, params?, _editor?)
    → { ran }                                      # UAnimationModifier execute
animation.add_animation_modifier(sequence_path, modifier_class_path, params?, _editor?)
    → { added }                                    # persistent attachment
```

---

#### Cluster G — Animation Layer Interface (P1)

UE 5.x linked anim layers — modular character mesh setup'ı için kritik.

```
animation.create_anim_layer_interface(path, name, _editor?)
    → { asset_path }                               # UAnimLayerInterface child
animation.add_layer_function(layer_interface_path, function_name, params?, _editor?)
    → { added }
animation.implement_anim_layer_interface(anim_bp_path, layer_interface_path, _editor?)
    → { implemented }
animation.set_linked_anim_layer(actor_or_cdo, layer_function_name, anim_class_path, _editor?)
    → { set }                                      # runtime SetLinkedAnimLayer
animation.list_implemented_layers(anim_bp_path, _editor?)
    → { layers: [...] }
```

---

#### Cluster H — Sequence/Montage advanced (P1)

```
animation.set_sequence_additive_settings(sequence_path, additive_type, base_pose_type, base_pose_animation?, _editor?)
    → { set }                                      # AAT_LocalSpaceBase | AAT_MeshSpaceBase | none
animation.set_sequence_compression_scheme(sequence_path, scheme_path?, _editor?)
    → { set }
animation.add_montage_branching_point(montage_path, time, branch_name, _editor?)
    → { branching_point_index }
animation.set_montage_blend_curve(montage_path, blend_in_or_out, curve_path?, _editor?)
    → { set }
animation.set_montage_section_loop(montage_path, section_name, loop=true, _editor?)
    → { set }
animation.set_montage_section_next(montage_path, section_name, next_section_name?, _editor?)
    → { set }                                      # chain section1 → section2
animation.copy_animation_curves(from_sequence, to_sequence, curve_names?, _editor?)
    → { copied_count }
```

---

#### Cluster I — Skeleton authoring (P1)

```
animation.add_skeleton_socket(skeleton_path, socket_name, parent_bone, transform, _editor?)
    → { socket_added }
animation.remove_skeleton_socket(skeleton_path, socket_name, _editor?)
    → { removed }
animation.add_slot(skeleton_path, slot_name, group_name?, _editor?)
    → { added }                                    # slot definition (montage'ların ihtiyacı)
animation.add_slot_group(skeleton_path, group_name, _editor?)
    → { added }
animation.set_bone_translation_retargeting(skeleton_path, bone, mode, _editor?)
    → { set }                                      # Animation|Skeleton|AnimationScaled|AnimationRelativeToRefPose|OrientAndScale
animation.add_skeleton_curve_metadata(skeleton_path, curve_name, type, _editor?)
    → { added }                                    # MaterialCurve / MorphTarget / etc.
```

---

#### Cluster J — Runtime PIE animation control (Lyra Gap #19 + ek)

`character.*` namespace yeni — runtime/PIE only, transactional değil. SkeletalMesh + AnimInstance manipulation.

```
character.play_root_motion_source(actor, source_type, direction, strength, duration, accumulate_mode?, finish_velocity_mode?, debug_name?, _editor?)
    → { source_id }                                # Lyra Gap #19 - ConstantForce|RadialForce|MoveTo|Jump
character.play_montage(actor, montage_path, play_rate?, start_section?, _editor?)
    → { montage_id, length }                       # AnimInstance::Montage_Play
character.stop_montage(actor, montage_id?, blend_out_time?, _editor?)
    → { stopped }
character.set_anim_instance_class(actor, anim_class_path, _editor?)
    → { swapped }                                  # SkeletalMeshComponent::SetAnimInstanceClass
character.list_active_montages(actor, _editor?)
    → { montages: [{ id, path, position, weight }] }
character.set_animation_mode(actor, mode, _editor?)
    → { set }                                      # AnimBlueprint|AnimAsset|Custom
character.play_animation(actor, animation_path, looping=true, _editor?)
    → { played }                                   # SkeletalMeshComponent::PlayAnimation (single asset)
character.set_morph_target(actor, target_name, value, _editor?)
    → { set }
```

---

### Implementation öncelik sırası

| Öncelik | Cluster | Tool sayısı | Bağımlılık |
|---|---|---|---|
| **P0** | A — AnimGraph node creation core | 8 | yok |
| **P0** | B — AnimGraph convenience nodes | 11 | A |
| **P0** | C — State machine deep CRUD | 13 | A, B |
| **P0** | D — AnimNotify class authoring | 8 (4 yeni + 4 mevcut iyileştirme) | yok |
| **P0** | E — BlendSpace sample + axis | 7 (5 yeni + 2 read genişletme) | yok |
| **P1** | F — Sync markers + curve compression + modifier run | 6 | yok |
| **P1** | G — Animation Layer Interface | 5 | A, B (LinkedAnimLayer node) |
| **P1** | H — Sequence/Montage advanced | 7 | yok |
| **P1** | I — Skeleton authoring | 6 | yok |
| **P0/P1 mix** | J — Runtime PIE animation | 8 (Lyra Gap #19 P0, ek P1) | yok |

**Toplam**: ~79 yeni/iyileştirilmiş tool. Mevcut 45'in yarısı stub/yarım zaten — **gerçek surface artışı net ~60 tool** (478 → ~538 toplam).

---

### Implementation strategy

1. **Plugin tarafı yapı taşları** (sıralı, blocker-first):
   - `SageAnimGraphBuilder.{h,cpp}` yeni dosya — Cluster A primitives (AddNode, ConnectPin, SetProperty, BindToVariable). Bu olmadan B/C çalışmaz.
   - State machine schema integration: `UAnimationStateMachineGraph::StaticClass()` + `UAnimationStateMachineSchema::StaticClass()` — `FBlueprintEditorUtils::CreateNewGraph` ile.
   - Pin connection: `UAnimationGraphSchema::TryCreateConnection` veya base K2 schema fallback.

2. **Plugin handler dosyaları** (parallel, A bittikten sonra):
   - `SageAnimGraphTools.cpp` — Cluster A + B (AnimGraph manipulation primitives + node helpers)
   - `SageAnimStateMachineTools.cpp` — Cluster C (state machine CRUD)
   - `SageAnimNotifyTools.cpp` — Cluster D (notify class + instance ops)
   - `SageBlendSpaceTools.cpp` — Cluster E
   - `SageSequenceTools.cpp` — Cluster F + H (sequence/montage advanced)
   - `SageAnimLayerTools.cpp` — Cluster G (layer interface)
   - `SageSkeletonTools.cpp` — Cluster I
   - `SageCharacterRuntimeTools.cpp` — Cluster J (`character.*` namespace, PIE-aware)

3. **Server schemas**:
   - `phase4_schemas.cpp` — yeni 79 schema. nlohmann brace-init pitfall ÇOK önemli (Gap pre-13 dersi). `obj()` helper'ı üzerinden geç, `{}` literal yasak. Build sonrası mutlaka `tools/list` smoke + null property tarama.

4. **Test stratejisi**:
   - Her cluster için bir end-to-end senaryo (yukarıda yazıldı)
   - PIE-aware tool'lar (Cluster J) için `RejectIfPie` opposite — sadece PIE'da çalışsın
   - State machine fixture: `T_FlightCore_StateMachineFixture.uasset` — ABP_FlightCore'a kurulu reference state machine, regression test

5. **Effort tahmini**:
   - Cluster A: 3-4 gün (graph node creation + pin connection en zor kısım)
   - Cluster B: 1-2 gün (A bittikten sonra delegasyon)
   - Cluster C: 2-3 gün (state machine sub-graph yönetimi)
   - Cluster D: 1 gün
   - Cluster E: 1 gün
   - Cluster F-J: her biri 1-2 gün
   - **Toplam P0 (A+B+C+D+E + Cluster J Gap #19): ~10-12 gün**
   - **Toplam P1 (F+G+H+I + Cluster J ek): ~5-7 gün**
   - **Hepsi**: ~3 hafta dedicated work

### Engine API ana referans noktaları

```cpp
// AnimGraph node creation
#include "BlueprintEditor/Public/EdGraphSchema_K2.h"
#include "AnimGraph/Public/AnimationGraph.h"
#include "AnimGraph/Public/AnimationGraphSchema.h"
#include "AnimGraph/Public/AnimGraphNode_Base.h"
#include "AnimGraph/Public/AnimGraphNode_SequencePlayer.h"
#include "AnimGraph/Public/AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraph/Public/AnimGraphNode_StateMachine.h"
#include "AnimGraph/Public/AnimGraphNode_BlendListByBool.h"
#include "AnimGraph/Public/AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraph/Public/AnimGraphNode_TwoBoneIK.h"
#include "AnimGraph/Public/AnimGraphNode_LinkAnimLayer.h"

// State machine
#include "AnimGraph/Public/AnimationStateMachineGraph.h"
#include "AnimGraph/Public/AnimationStateMachineSchema.h"
#include "AnimGraph/Public/AnimGraphNode_StateMachine.h"
#include "AnimGraph/Public/AnimStateNode.h"
#include "AnimGraph/Public/AnimStateConduitNode.h"
#include "AnimGraph/Public/AnimStateAliasNode.h"
#include "AnimGraph/Public/AnimStateTransitionNode.h"

// Notify class authoring
#include "Engine/Public/Animation/AnimNotifies/AnimNotify.h"
#include "Engine/Public/Animation/AnimNotifies/AnimNotifyState.h"
#include "Kismet/Public/KismetEditorUtilities.h"

// BlendSpace
#include "Engine/Public/Animation/BlendSpace.h"
#include "Engine/Public/Animation/BlendSpace1D.h"

// Sync markers / Curve compression / Modifier
#include "Engine/Public/Animation/AnimSequence.h"
#include "Engine/Public/Animation/AnimationModifier.h"
#include "Engine/Public/Animation/AnimCurveCompressionCodec.h"

// Layer interface
#include "Engine/Public/Animation/AnimLayerInterface.h"
#include "Engine/Public/Animation/AnimSubsystem_LinkedAnimGraph.h"

// Runtime
#include "Engine/Public/GameFramework/CharacterMovementComponent.h"
#include "Engine/Public/GameFramework/RootMotionSource.h"
#include "Engine/Public/Components/SkeletalMeshComponent.h"
#include "Engine/Public/Animation/AnimInstance.h"
```

Build.cs'e zaten dahil olmayanlar:
- `AnimGraph` (editor module — public)
- `AnimGraphRuntime` (runtime module)

Mevcut `SageBridge.Build.cs`'te `AnimGraph` + `AnimGraphRuntime` var (`9d7380b` öncesi de). Yeni include yok — pure code addition.

### Risk + lessons.md güncellemeleri

1. **AnimGraph schema vs K2 schema farkı** — pin tipleri (`PoseLink` vs `Wildcard`) farklı, `TryCreateConnection` schema'ya göre validate eder. Yanlış schema seçimi silent fail.
2. **State machine sub-graph** — her state'in kendi `BoundGraph`'i var (transition logic). Compile'da unbound graph crash'ler.
3. **Property binding vs literal property** — `UAnimGraphNode_Base::PropertyBindings` map (variable binding) ≠ `Node.MyProperty = X` (literal). Karışırsa runtime'da pin disconnected görünür.
4. **AnimNotify track index** — UE 5.x'te `AnimNotifyTracks` array ayrı, default index 0; track yoksa notify display problem.
5. **GameThread marshal** — bütün AnimGraph mutation FBlueprintEditorUtils çağırır → `RunOnGameThread` zorunlu (lessons.md mevcut kural).
6. **Compile ordering** — node ekleyince + property set sonrası + connect sonrası compile. Erken compile state machine boş kabul eder.

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-04 ~13:30 (Sage Claude — surface çıkarma + yol haritası)
- **Kök sebep**: AnimGraph manipulation hiç implement edilmemiş; CommonAIExport "out of scope" demiş, Sage henüz dokunmamış. Phase 4-r2 BP authoring'in anim equivalent'ı eksik.
- **Fix commit**: pending — bu yol haritası onaylanırsa P0 cluster A'dan başlanacak
- **Deploy adımı**: P0 cluster'lar batch (A+B+C+D+E+J(#19)) → plugin rebuild → 3-şey copy (Lyra+HeroFlight) → editor restart → Lyra Claude verify
- **Verify durumu**: pending — Mahmut prio onayı + cluster ordering kararı

#### Mahmut'a sorular (öncelik kararı için)

1. **Bu yol haritasındaki cluster'lar ve sayım onaylanıyor mu?** Eklenmesi/çıkarılması gereken bir şey var mı?
2. **P0 olarak başlangıç set'i**: A+B+C+D+E + Cluster J'den sadece `play_root_motion_source` (Gap #19 = HeroFlight FlightCore Phase 5 polish) — ~3 hafta tam dedicated. Daha küçük scope ister misin?
3. **CommonAIExport ile ilişki**: AnimBP authoring Sage'e taşınınca CommonAIExport AIAnimBlueprintBuilder sunset olabilir mi (donor pattern, daha önce konuştuk)? Yoksa CommonAIExport simple wrapper kalmaya devam mı eder?
4. **Test fixture**: `T_FlightCore_StateMachineFixture.uasset` Lyra'da mı duracak yoksa Sage repo'da test asset'leri için bir yer mi açacağız?
5. **Lyra Gap #16-19**: bunları **Gap #20 ile birleştirme** mi (DUPLICATE link) yoksa ayrı entry'ler tutup #20 referans mı (master tracking)?

---

## Gap #19 — `character_play_root_motion_source` (CMC root motion driven launches)

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5 — Dodge ability launch quality)
- **Project**: D:\Steamworks\Lyra
- **Affected tool**: yeni — `character_play_root_motion_source` (öneri)

### Belirti

FlightCore Dodge ability şu anda `CMC.AddImpulse(direction * DodgeStrength, true)` ile fırlatma yapıyor — predicted ama **net-correct değil**, hız hızla bozulur. HeroFlight'ta BPANS_DodgeMovementInputDirection + RootMotionSource pattern'i.

GAS'da doğru pattern: `UAbilityTask_ApplyRootMotionConstantForce` veya `UAbilityTask_ApplyRootMotionRadialForce` async task, internal'da `UCharacterMovementComponent::ApplyRootMotionSource` çağırır. Bu Sage tool olarak yok.

### Test edilen alternatif

`AddImpulse` MVP olarak çalışıyor ama momentum'u CMC.Velocity'e direct yazıyor, smoothing/interp yok. RootMotionSource:
- Predicted + replicated (server reconciliation doğru)
- Curve-driven (force vs time)
- Multi-source stackable (üst üste dodge ya da sprint+dodge etkileri)

### Beklenen API

```
character_play_root_motion_source(
    actor: SoftObjectPath,           # PIE world içi character actor
    source_type: enum,                # ConstantForce | RadialForce | MoveToForce | JumpForce
    direction: Vector,
    strength: float,
    duration: float,                  # saniye
    accumulate_mode: enum,            # Override | Additive
    finish_velocity_mode: enum,       # MaintainLastRootMotion | SetVelocity | ClampVelocity
    debug_name: str
) -> { source_id: int }
```

C++ tarafı: `UCharacterMovementComponent::ApplyRootMotionSource` + `FRootMotionSource_ConstantForce` (vb. struct) — engine API'si zaten var, TCP handler ile expose lazım.

### Workaround (Lyra Claude)

FlightCore Dodge ability şu an `AddImpulse` ile geçti. RootMotionSource Phase 6+ polish için. Plus AbilityTask_ApplyRootMotion* C++'ta direkt kullanılabilir — bu Sage tool olmadan da yapılabilir ama runtime trigger için Sage tool runtime test'te kolaylık sağlar.

---

## Gap #18 — `animation_create_anim_notify_state` (UAnimNotifyState BP child)

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5 — flight notify hooks)
- **Project**: D:\Steamworks\Lyra
- **Affected tools**: yeni — `animation_create_anim_notify_state`, `animation_create_anim_notify`

### Belirti

HeroFlight'ta animation-driven gameplay event pattern: BPANS_DodgeMovementInputDirection (montage sırasında WASD oku → direction set), BPANS_SetIsSuperherolanding (landing montage sırasında bIsSuperheroLanding true), BPAN_SpawnSonicBoomVFX (sprint anim ortasında VFX trigger). Bu pattern'i FlightCore'da sıfırdan kurmak gerek (HeroFlight'tan migrate edilen broken BP'ler silindi — `/Script/HeroFlightGame.SuperheroFlightComponent` parent dependency yok).

`animation_add_notify` Sage tool'u var ama **var olan bir notify class'ını** sequence'a ekler. Yeni notify class (UAnimNotify veya UAnimNotifyState child BP) yaratmaz.

### Beklenen API

```
animation_create_anim_notify(path: str, parent_class: str = "/Script/Engine.AnimNotify") -> {asset_path}
animation_create_anim_notify_state(path: str, parent_class: str = "/Script/Engine.AnimNotifyState") -> {asset_path}
```

İçerik (Received notify event, BeginState/Tick/EndState) BP graph'ı boş olur — kullanıcı sonradan BP graph'ında logic yazar (gameplay tag set/clear, gameplay event broadcast). Plus Sage'in mevcut BP graph node ekleme tool'larıyla doldurulabilir (`bp_add_event_node` + `bp_add_function_call`).

### Etki

Bu olmadan FlightCore animation-driven event'leri BP üzerinden tanımlanamaz. Runtime'da `OnNotifyBegin` C++ delegate ile dinlenebilir ama gameplay tag/effect tetikleme zorlaşır — Lyra-canonical değil.

### Workaround

C++ side `UAnimNotify_PlayMontageNotifyWindow` veya custom `UAnimNotify` subclass yaratıp source kod'a ekleyebilirim. Plugin C++ build cycle'a giriyor — daha ağır.

---

## Gap #17 — `animation_set_blendspace_samples` (BlendSpace anim sample placement)

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5 — flight locomotion BlendSpace)
- **Project**: D:\Steamworks\Lyra
- **Affected tool**: existing — `animation_create_blendspace` çalışıyor olabilir ama sample placement yok

### Belirti

FlightCore HoverMove için 2D BlendSpace gerek: X = ForwardSpeed (-1500..+1500), Y = StrafeSpeed (-1500..+1500). 9 sample point: corners + axes:
- (-1500, 0): A_Flight_HoverMove_Backward (yok — Idle ya da Backward strafe)
- (+1500, 0): A_Flight_HoverMove_Forward
- (0, ±1500): A_Flight_HoverMove_Strafe_L/R
- vb.

`animation_create_blendspace` boş asset yaratır (deneme yapmadım, dokümantasyona göre `dimensions` + `skeleton` parametreleri alıyor). Sample anim point'leri eklemek için ayrı tool yok.

### Beklenen API

```
animation_set_blendspace_samples(
    path: str,
    samples: list[{ position: float[2], animation: str }]  # 1D için float, 2D için float[2]
) -> {sample_count}

# veya append-style:
animation_add_blendspace_sample(path: str, position: ..., animation: str) -> {sample_index}
```

C++ tarafı: `UBlendSpaceBase::AddSamplePoint` (engine'de mevcut) — TCP handler ile expose.

### Etki

BlendSpace olmadan FlightCore HoverMove tek anim oynar (forward), strafe/backward yön hissi vermez. State machine içinde HoverMove state'i için BlendSpace temel. Bu olmazsa hover sadece "ileri" gözükür.

### Workaround

Persona'da manuel BlendSpace düzenleme. State machine ile birlikte zaten Persona oturumu gerek (Gap #16) — bu da o oturumda elle yapılır. Ama Sage tool olsa state machine + BlendSpace ikisi de scriptable olur.

---

## Gap #16 — Animation tool suite stub: AnimGraph node creation + state machine wiring

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5 — flight locomotion AnimBP)
- **Project**: D:\Steamworks\Lyra
- **Affected tools**: `animation_create_state_machine`, `animation_add_state`, `animation_add_transition`, `animation_set_state_animation` (and likely the rest of the Sage anim tool family)

### Belirti

`animation_create_state_machine` çağrısı içerik üretmiyor — sadece açıklama metni döndürüyor:
```
"note": "state machine graph creation requires FBlueprintEditorUtils::CreateNewGraph with AnimationStateMachineSchema; open the AnimBP in Persona and use editor.run_python"
```

### Test

UE 5.7 Python `unreal.*` modülünde EdGraph manipulation expose edilmemiş:
- `unreal.AnimationGraph` üzerinde `add_node`, `spawn_node`, `connect_pins` yok (`get_animation_graphs` ve `get_nodes_of_class` var ama read-only).
- `unreal.AnimGraphNode_*` class'ları (SequencePlayer, StateMachine, BlendSpacePlayer, BlendListByBool, StateResult, Root) hepsi import edilebilir ama instantiate edip graph'a koymak için API yok.
- `BlueprintEditorLibrary.add_function_graph` var, anim graph eklemek için yok.

Bu yüzden `editor_run_python` ile workaround mümkün değil.

### Etki

FlightCore animasyon işi blocked: Persona açıp manuel state machine kurmak zorunlu. ABP_FlightCore.AnimGraph şu an boş — Idle/HoverMove/FastMove state'leri programatik ekleyemiyorum. MVP olarak `Mesh.PlayAnimation(AnimSequence)` hard-swap kullandım (Lyra-canonical değil).

Diğer FlightCore-style projelerde de aynı bottleneck olur — herhangi bir custom locomotion AnimBP otomatize edilemiyor.

### Beklenen fix

C++ TCP handler (`AIBlueprintGraphBuilder` modülünün AnimBP karşılığı). UE C++ side'da:
- `FBlueprintEditorUtils::CreateNewGraph(AnimBP, "FlightStates", UAnimationStateMachineGraph::StaticClass(), UAnimationStateMachineSchema::StaticClass())`
- `UAnimGraphNode_StateMachine::StaticClass()` instance + AnimGraph root output pose'a connect
- `FBlueprintEditorUtils::AddNewState` + `AddNewTransition` API'leri C++ tarafında mevcut

Tool surface'ı zaten Sage'de tanımlı (`animation_create_state_machine`, `animation_add_state`, `animation_add_transition`, `animation_set_state_animation`, `animation_set_transition_blend`). Sadece TCP handler'lar C++ tarafında yazılmamış — placeholder dönüş ile bağlı.

Ek olarak yararlı olur:
- `animation_add_sequence_player_to_graph(graph, anim_path, x, y)` — single-state Output Pose hookup için
- `animation_connect_pose_pin(from_node, from_pin, to_node, to_pin)` — graph içi connection
- `animation_set_state_machine_root` — AnimGraph root'a state machine bağlama

### Workaround (Lyra Claude tarafı)

Geçici olarak `Mesh.PlayAnimation` hard-swap kullanılıyor (CharacterMovementComponent.EnterFlightMode). Persona manuel kurulduğunda `Mesh.SetAnimInstanceClass(ABP_FlightCore_C)` swap'a geçilecek — Lyra-canonical pattern.

---

## Gap #15 — `gameplay_set_world_game_mode` schema/server param uyumsuzluğu

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5 — WorldSettings GameMode override)
- **Project**: D:\Steamworks\Lyra
- **Tool**: `mcp__sage__gameplay_set_world_game_mode`

### Belirti

Schema `game_mode` parametresi tanımlıyor:
```json
{"required": ["game_mode"], "properties": {"game_mode": {"type": "string"}}}
```

Server `game_mode_class` istiyor:
```
MCP error -32602: missing 'game_mode_class'
```

### Etki

Tool kullanılamaz — schema'ya uyumlu çağrı server tarafında reject. Workaround: `level_set_world_settings` ile `DefaultGameMode` property'si üzerinden set etmek (çalışıyor, gap-inbox'a eklemek için yeterince temiz workaround).

### Beklenen fix

- ya schema güncellensin → `game_mode_class` param adı
- ya server handler düzelsin → `game_mode` param adı kabul etsin

İki taraftan biri tek seçim. Tutarlılık için `game_mode` daha doğal (Lyra/UE convention'una uyumlu, "class" eki gereksiz).

---

## Gap #14 — `level_create` boş level + save → Niagara `Level` assertion crash

- **Status**: 🆕 OPEN
- **Reported**: 2026-05-04 by Lyra Claude (FlightCore Phase 5.2 — FlightMaps test map setup)
- **Project**: D:\Steamworks\Lyra
- **Editor**: B_FlightCore_Experience PIE smoke test için yeni test map yaratımı

### Hedef
FlightMaps plugin için boş bir test map yarat (`/FlightMaps/Maps/L_FlightCore_Test`), sonra disk'e save et — PIE smoke test için flight pawn'un spawn olabileceği geniş empty level (default Niagara/lighting hiç önemli değil, sadece skeleton level).

### Denenen tool çağrısı

İki kez denedim, ikisinde de aynı sonuç:

**Deneme 1** — boş paths (= save all dirty):
```json
{"tool": "level_create", "args": {"path": "/FlightMaps/Maps/L_FlightCore_Test"}}
{"tool": "save_assets", "args": {}}
```

**Deneme 2** — explicit paths (sadece bu level):
```json
{"tool": "level_create", "args": {"path": "/FlightMaps/Maps/L_FlightCore_Test"}}
{"tool": "save_assets", "args": {"paths": ["/FlightMaps/Maps/L_FlightCore_Test"]}}
```

İki yaklaşım da editör'ü patlatıyor — paths-specific save yardım etmedi.

### Sonuç / hata

```
LogFileHelpers: Saving Map: /FlightMaps/Maps/L_FlightCore_Test
LogChaosDD: Creating Chaos Debug Draw Scene for world L_FlightCore_Test
LogWindows: Error: appError called: Assertion failed: Level
  [File: Engine/Source/Runtime/Engine/Private/TickTaskManager.cpp] [Line: 2243]

[Callstack]
  UnrealEditor-Core.dll!UnknownFunction
  UnrealEditor-Engine.dll!UnknownFunction
  UnrealEditor-Niagara.dll!UnknownFunction        ← suçlu
  UnrealEditor-Niagara.dll!UnknownFunction
  UnrealEditor-Niagara.dll!UnknownFunction
  UnrealEditor-Engine.dll!UnknownFunction (TickTaskManager assertion)
  UnrealEditor-UnrealEd.dll!...                    (save package flow)
  UnrealEditor-SageBridge.dll!SaveAssetsOnGameThread() [SageAssetTools.cpp:316]
```

`level_create` cevap: `{"loaded":true,"path":"..."}` — asset oluştu görünüyor. Ama editor world değişmedi (`get_world` hâlâ önceki map'i veriyor — önemli ipucu, aşağıda).

### Beklenen davranış

1. `level_create` boş bir UWorld oluşturmalı **WorldSubsystem'leri tam initialize edilmiş halde** (Niagara, AISystem, AudioEngine vb.) — UE editor "File → New Level → Empty Level" buradan geçer (`UEditorWorldUtils::SetupAndCreateNewWorld` veya `UEditorEngine::CreateNewMapForEditing`).
2. `save_assets` boş level'i crash etmeden disk'e yazabilmeli — UE editor manual save bunu yapıyor.

### Workaround

**Manuel UE editor**: `File → New Level → Empty Level → Ctrl+Shift+S → Save As → Plugins/GameFeatures/FlightMaps/Content/Maps/L_FlightCore_Test`. Engine'in tam init pipeline'ı çalışıyor, Niagara WorldSubsystem nullptr olmuyor.

Workaround **Sage workflow'unu kıyor**: yeni plugin'lerin kendi test map'lerini Sage-driven yaratmak imkansız → tüm test setup manuel UI clicking gerektiriyor.

### Öneri (A — must)

İki yönlü fix:

**(a)** `level_create` proper init pipeline:

```cpp
// Mevcut (tahmin): muhtemelen sadece UWorld package oluşturuyor
UPackage* Pkg = CreatePackage(*PackagePath);
UWorld* W = NewObject<UWorld>(Pkg, ...);
W->InitializeNewWorld(UWorld::InitializationValues()); // ?

// Olması gereken — UE editor "New Level" pipeline:
UWorldFactory* Factory = NewObject<UWorldFactory>();
Factory->WorldType = EWorldType::Inactive;
Factory->bInformEngineOfWorld = true;
Factory->FeatureLevel = GMaxRHIFeatureLevel;
UPackage* Pkg = CreatePackage(*PackagePath);
UWorld* W = Cast<UWorld>(Factory->FactoryCreateNew(UWorld::StaticClass(), Pkg, *Name, RF_Public|RF_Standalone, nullptr, GWarn));
// veya direkt UEditorEngine::CreateNewMapForEditing
W->InitWorld();
W->UpdateWorldComponents(true, false);
FAssetRegistryModule::AssetCreated(W);
```

`UEditorEngine::CreateNewMapForEditing(/*bPromptUserToSave*/ false)` muhtemelen daha temiz — UE engine'in canonical empty level path'i, tüm subsystem init'lerini içerir.

**(b)** Defensive: `save_assets` Level packages için pre-flight:

```cpp
if (UWorld* AsWorld = Cast<UWorld>(Asset)) {
    if (!AsWorld->PersistentLevel || !AsWorld->GetSubsystemBase(UNiagaraWorldManager::StaticClass())) {
        return Error("level not fully initialized — call level_create with proper world init pipeline");
    }
}
```

Daha az invasive ama crash önler. Asıl fix (a).

**Bonus side issue** — bu fix'le ilgili olabilir: `level_load` da editor world'ü gerçekten değiştirmiyor (`{"loaded":true}` cevabı veriyor ama `get_world` hâlâ eski map'i gösteriyor). `editor_console_command "open L_Expanse"` (allow_unsafe) da no-op. Yani Sage'in level switching pipeline'ı genel olarak editor world'ü mutate etmiyor — sadece asset registry'ye dokunuyor olmalı. (a) fix'i muhtemelen bunu da çözer çünkü `CreateNewMapForEditing` editor world swap'ı içerir.

### Reproduce

```
1. Sage'le bağlı Lyra (veya boş Lyra-style proje, Niagara plugin enabled)
2. mcp__sage__level_create(path="/SomePlugin/Maps/L_NewMap")  → "loaded:true"
3. mcp__sage__save_assets()  veya  save_assets(paths=["/SomePlugin/Maps/L_NewMap"])
4. Editor crashes — TickTaskManager.cpp:2243 assertion in Niagara DLL
```

100% reliable repro Lyra ortamında (iki ayrı denemede 2/2). HeroFlight'ta da büyük ihtimalle (Niagara plugin enabled olduğu sürece).

### Lyra-side state (FYI Sage Claude için)

- FlightCore Phase 5.2 — bu gap engellemiyor, manuel `File → New Level` workaround uygulanacak (Mahmut UI'da yarattıktan sonra Sage akışıyla devam edilebilir)
- Phase 4'ün tamamı bu fix gerektirmedi — sadece Phase 5.2'nin "test map" alt-adımı blocked

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**:
- **Kök sebep**:
- **Fix commit**:
- **Deploy adımı**:
- **Verify durumu**: pending

---

## Closed log (newest-first)

<!-- CLOSED entry'ler buraya. Newest-on-top. -->

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
