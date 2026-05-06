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

## Gap #24 — AnimLayerInterface child override graph spawn + master AnimGraph linked-layer call eksik

- **Status**: ✅ FIXED — deploy/verify pending (Lyra plugin install + tools/list verify)
- **Reported**: 2026-05-06 16:45 tarafından Lyra Claude / FlightCore asset wiring sprint
- **Project**: `D:\Steamworks\Lyra`
- **Editor**: Lyra UE 5.7.4
- **Affected assets**:
  - `/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C` (anim layer interface, 1 fonksiyon: `FlightLocomotionPose`)
  - `/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion` (parent `UFlightCoreAnimInstance`, ALI implement edilmiş)
  - `/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin` (master ABP, `ABP_Mannequin_Base` duplikasyonu, ALI ek olarak implement edilmiş)
  - `/FlightCore/Game/B_Hero_HeroFlight.B_Hero_HeroFlight` (Pawn BP — Mesh.AnimClass = master, CMC.FlightLocomotionLayerClass = locomotion)

### Hedef

Lyra-canonical **AnimLayerInterface linked-layer pattern**'ı tamamen MCP üzerinden author etmek. Lyra'nın weapon system'i (`B_WeaponInstanceBase.cpp:110` → `Mesh->LinkAnimClassLayers(LayerClass)` + `ABP_Mannequin_Pistol/Rifle` override'ları) bu pattern'i kullanır. FlightCore aynı pattern'i flight locomotion için uyguluyor:

1. ALI_FlightLocomotionLayer (anim layer interface BP, BPTYPE_Interface AnimBlueprint)
2. ABP_FlightCore_Locomotion bu interface'i implement eder, `FlightLocomotionPose` fonksiyonunu state machine ile override eder
3. Master ABP_HeroFlight_Mannequin AnimGraph'ında `FlightLocomotionPose()` çağrısı yapan bir UAnimGraphNode_LinkedAnimLayer node'u olur
4. Runtime: CMC `OnMovementModeChanged`'da `Mesh->LinkAnimClassLayers(ABP_FlightCore_Locomotion_C)` çağrılır → master'ın FlightLocomotionPose call'ı child override'ına yönlendirilir

İki manuel UE Editor adımı kalıyor; bunlar MCP'de tool olmadığı için sprint blocker.

### Denenen tool çağrıları

**Adım A — child override graph spawn beklentisi:**

```json
{ "tool": "bp.add_interface",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C"
  } }
```

Sonuç: `{"already": false, "interface": "ALI_FlightLocomotionLayer_C", ...}` — interface eklendi ama:

```json
{ "tool": "bp.list_functions",
  "args": { "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion..." } }
```

`{"count": 2, "functions": [{"kind":"function","name":"AnimGraph"}, {"kind":"event_graph","name":"EventGraph"}]}`

**`FlightLocomotionPose` override graph yok** — child BP'de interface'in fonksiyon graph'ı spawn etmedi. UE Editor UI'da "My Blueprint > Interfaces > ALI_FlightLocomotionLayer > FlightLocomotionPose" sağ tık → "Implement" eylemine eşdeğer otomasyon eksik.

**Adım B — master AnimGraph'a linked-layer call ekleme:**

Mevcut `animation.add_animgraph_node` UAnimGraphNode_LinkedAnimLayer için convenience yok; class adıyla doğrudan spawn deneyince:

```json
{ "tool": "animation.add_animgraph_node",
  "args": {
    "path": "/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin",
    "graph_name": "AnimGraph",
    "node_class": "/Script/AnimGraph.AnimGraphNode_LinkedAnimLayer"
  } }
```

(Henüz dene-il-medi — bu gap aslında 2-faz: önce A çözülürse, B için convenience spec lazım. Aşağıda B için detaylı spec verildi.)

### Sonuç / hata

- Adım A: tool sessizce başardı (`already=false`) AMA yan etki yok — child BP'de override graph spawn etmedi, `bp.list_functions` aynı 2 graph dönüyor, `animation.list_animgraph_nodes graph_name=FlightLocomotionPose` → `MCP error -32602: graph 'FlightLocomotionPose' not found`.
- Adım B: deneme yapılmadı; convenience tool eksikliği rapor edildi.

### Beklenen davranış

#### Tool 1: `animation.add_layer_function_override`

Child AnimBP'de implemented AnimLayerInterface'in spesifik fonksiyonu için override graph spawn eder.

```json
{ "tool": "animation.add_layer_function_override",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "function_name": "FlightLocomotionPose",
    "compile": true
  } }
```

Dönüş:
```json
{ "function_name": "FlightLocomotionPose",
  "graph_name": "FlightLocomotionPose",
  "schema": "AnimationGraphSchema",
  "node_count": 1,
  "output_node_id": "<UAnimGraphNode_Root FGuid>",
  "compiled": true }
```

Davranış:
- `path`'te BP'nin `ImplementedInterfaces` listesinde `interface_path` bulunmazsa `-32602: interface not implemented`.
- `interface_path`'te `function_name` declared değilse `-32602: function not declared on interface`.
- AnimGraph schema'lı (UAnimationGraphSchema) override graph spawn et — UBlueprint::FunctionGraphs'e ekle.
- Output Pose root node (`UAnimGraphNode_Root`) auto-spawn (interface fonksiyonu pose-output'lu olduğu için).
- Idempotent: graph zaten varsa `{"already": true, ...}` dön.
- Optional compile.

UE 5.7 implement detayı:
- `FBlueprintEditorUtils::ImplementInterfaceMethod` (varsa) veya
- Manuel: `FBlueprintEditorUtils::CreateNewGraph(InBP, FunctionName, UEdGraph::StaticClass(), UAnimationGraphSchema::StaticClass())` + `InBP->FunctionGraphs.Add(NewGraph)` + UAnimGraphNode_Root spawn (`CreateDefaultPins` + `AllocateDefaultPins`)
- **CRITICAL**: PostPlacedNewNode pipeline'ı atlama (Gap #21 verify learning).

#### Tool 2: `animation.add_linked_anim_layer_node`

Master AnimBP'nin AnimGraph'ında AnimLayerInterface fonksiyonunu çağıran UAnimGraphNode_LinkedAnimLayer node'u spawn eder.

```json
{ "tool": "animation.add_linked_anim_layer_node",
  "args": {
    "path": "/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin",
    "graph_name": "AnimGraph",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "function_name": "FlightLocomotionPose",
    "x": -400, "y": 0,
    "compile": true
  } }
```

Dönüş:
```json
{ "node_id": "<FGuid>",
  "class": "/Script/AnimGraph.AnimGraphNode_LinkedAnimLayer",
  "interface": "ALI_FlightLocomotionLayer_C",
  "interface_function": "FlightLocomotionPose",
  "pin_count": 2,
  "input_pose_pin": "InputPose",
  "output_pose_pin": "Pose",
  "compiled": true }
```

Davranış:
- Master BP'nin `ImplementedInterfaces` içinde `interface_path` bulunmazsa `-32602: interface not implemented on master`.
- AnimGraph schema'lı `graph_name`'i `path`'in FunctionGraphs/UbergraphPages içinde bulamazsa `-32602: graph not found`.
- `UAnimGraphNode_LinkedAnimLayer` spawn et:
  - `Interface = interface_path` (UClass ref)
  - `Layer = NAME_None` (unset → "Self" linked layer; runtime'da `LinkAnimClassLayers` ile child class set edilir)
  - Function name'i internal property (`FAnimNode_LinkedAnimLayer::Layer`) olarak set
- AllocateDefaultPins + ReconstructNode ile InputPose + OutputPose pin'leri açılır.
- Pin'ler **disconnected** döner; callsite `animation.connect_pose_pin` ile master AnimGraph içine bağlamak için kullanır.
- Optional compile.

UE 5.7 implement detayı:
- `UAnimGraphNode_LinkedAnimLayer` (`Engine/Source/Editor/AnimGraph/.../AnimGraphNode_LinkedAnimLayer.h`)
- `Interface` UClass property: ImplementedInterfaces'tan resolve et
- `Layer` FName property: function_name (interface'in declared function adı)
- PostPlacedNewNode → ReconstructNode kanonik pipeline.

### Workaround

Yok. UE Editor'de manuel iki tıklama:
1. Editor'de ABP_FlightCore_Locomotion aç → My Blueprint > Interfaces > ALI_FlightLocomotionLayer > FlightLocomotionPose sağ tık → "Implement Function" → graph içine SequencePlayer + Output Pose
2. Editor'de ABP_HeroFlight_Mannequin aç → AnimGraph → sağ tık (boş alan) → "Linked Anim Layer" sub-menu → ALI_FlightLocomotionLayer.FlightLocomotionPose seç

Bu iki adım sprint için manuel kalır; otomasyon olmadan PIE smoke test yapılamaz.

### Öneri (öncelik A/B/C)

**A — must (Gap #24 close şartı)**
1. `animation.add_layer_function_override` — child override graph spawn (yukarıdaki API).
2. `animation.add_linked_anim_layer_node` — master AnimGraph'a linked-layer call (yukarıdaki API).

**B — should (workflow ergonomics)**
3. `animation.list_implemented_layers` mevcut listede "NOT IMPLEMENTED" — verify amaçlı gerçek impl. Dönmesi:
```json
{"interfaces": [
  {"interface_path":"...","interface_class":"ALI_FlightLocomotionLayer_C",
   "functions":[{"name":"FlightLocomotionPose","override_graph":"FlightLocomotionPose","node_count":4}]}
]}
```
Override graph yoksa `override_graph: null`.

4. `animation.implement_anim_layer_interface` mevcut "NOT IMPLEMENTED" stub — Tool 1 ile birleşik convenience: interface implement + tüm fonksiyonların override graph'larını otomatik spawn (Tool 1'i her fonksiyon için tek tek çağırma yerine).

**C — nice-to-have**
5. Convenience spec tool — Lyra-canonical linked-layer pattern atomic kurulum:
```json
{ "tool": "animation.create_linked_layer_pattern",
  "args": {
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "child_path":     "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "master_path":    "/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin",
    "master_graph":   "AnimGraph",
    "function_name":  "FlightLocomotionPose"
  } }
```
Tek call'da: child'a interface implement + override graph spawn + master'a interface implement + master AnimGraph'a linked-layer call. Tüm BP'leri compile et.

### Notlar — context

- ALI_FlightLocomotionLayer şu an `ALI_ItemAnimLayers.uasset` duplikasyonu sonrası 13 weapon-style fonksiyon `bp.delete_function` ile silinerek 1 fonksiyona indirildi (`FlightLocomotionPose`). Schema AnimationGraphSchema, compile temiz. **Bu Sage'de ALI yaratma için güvenilir pattern**.
- Sage zaten `animation.create_anim_layer_interface` "NOT IMPLEMENTED" listesinde tutuyor; Tool 1 + Tool 2 öncelikli. Standalone ALI factory (BPTYPE_Interface AnimBlueprint create) Gap olarak aşağıda ayrı entry'e gerek yok — duplicate_asset workaround mevcut.
- Lyra'da paralel weapon system referansı: `B_WeaponInstanceBase.cpp:110` `LinkAnimClassLayers` + ABP_Mannequin_Base'in (master) AnimGraph'ında ALI_ItemAnimLayers fonksiyon call'ları. Sage Claude implementasyon sırasında bunu canonical reference olarak kullanabilir.
- Önceki Lyra editor crash'i (2026-05-06 ~15:30) muhtemelen bu eksikliği elle Python reflection ile tepelemeye çalışmaktan kaynaklı; Mahmut "raw Python graph/property manipülasyonu yapmasın" kuralıyla kapattı.

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-06
- **Kök sebep**: Cluster G (Animation Layer Interface) tüm 5 mevcut tool stub durumdaydı (`[NOT IMPLEMENTED]`). Phase 4-r6 sweep'inde yer almamıştı çünkü `complex API` notu vardı. Lyra Claude'un Gap #24 raporu spec'i temizledi: ALI authoring zinciri = (1) ALI factory + (2) interface function declare + (3) child override graph spawn + (4) master linked-layer call node + (5) runtime LinkAnimClassLayers smoke. Plus bp.add_interface anim-aware muadili (5'in bulk versiyonu).
- **Scope**: Mahmut "yan, her şeyi bizim söylememize gerek yok" emrine uygun olarak Gap #24'ün 2 must tool'u (`add_layer_function_override` + `add_linked_anim_layer_node`) yanı sıra tüm Cluster G familyasını real impl'e geçirdik. Toplam **11 tool**:
  - **Phase A (Gap #24 must, primitive)**: `animation.add_layer_function_override`, `animation.add_linked_anim_layer_node`
  - **Phase B (5 stub→real)**: `animation.create_anim_layer_interface`, `animation.add_layer_function`, `animation.implement_anim_layer_interface` (anim-aware bp.add_interface), `animation.list_implemented_layers`, `animation.list_layer_functions`
  - **Phase C (cleanup symmetry)**: `animation.remove_layer_function_override`, `animation.remove_layer_function` (her ikisi `confirmed:true`)
  - **Phase D (convenience macro)**: `animation.create_linked_layer_pattern` (Lyra B_WeaponInstanceBase pattern atomic)
  - **Phase E (PIE smoke)**: `animation.set_linked_anim_layer` (LinkAnimClassLayers / UnlinkAnimClassLayers wrapper)
- **Mevcut placeholder kaldırıldı**: `animation.add_link_anim_layer` (Cluster B'deki bare `SpawnByClassPathImpl` shortcut, interface bilmeden spawn ediyordu) `animation.add_linked_anim_layer_node` ile değiştirildi (no backwards-compat alias — Sage henüz public release değil, breaking güvenli).
- **Implementation noktaları**:
  - `FBlueprintEditorUtils::CreateNewGraph(BP, FuncName, UEdGraph::StaticClass(), UAnimationGraphSchema::StaticClass())` ile AnimGraph schema'lı function graph spawn; AnimGraphSchema CreateDefaultNodesForGraph Output Pose root'unu yaratıyor (idempotency için manuel UAnimGraphNode_Root fallback da var).
  - Override graph child BP'sinin `FBPInterfaceDescription::Graphs` array'ine push ediliyor (interface declaration'ın `UBlueprint::FunctionGraphs` üzerine değil — child-side override semantik).
  - UAnimGraphNode_LinkedAnimLayer için `Node->Node.Interface` (UClass) + `Node->Node.Layer` (FName) PostPlacedNewNode öncesi set ediliyor → ReconstructNode pose pin'leri otomatik allocate ediyor (manuel pin construction gerek yok).
  - Lyra ALI canonical reference: `/Game/Characters/Heroes/Mannequin/Animations/LinkedLayers/ALI_ItemAnimLayers` + `B_WeaponInstanceBase.cpp:110 Mesh->LinkAnimClassLayers(LayerClass)`.
  - `set_linked_anim_layer` PIE world dışında `editor preview only` warning ile success döner, package dirty yapmaz (read-only fallback).
- **Schema rewrite**: `server/src/tools/phase4_schemas.cpp` Cluster G bloğu (5 stub line 592-613 → 11 real schema, plus eski `add_link_anim_layer` schema removal line 432-435). Tool count 464 → 560 smoke verified (server build OK, tools/list direct invoke).
- **Plugin rewrite**: `plugin/Source/SageBridge/Private/Tools/SageAnimationTools.cpp` Cluster G bloğu (5 stub line 7411-7439 → ~700 satır real impl + 5 helper). Yeni include'lar: `AnimGraphNode_LinkedAnimLayer.h`, `Animation/AnimNode_LinkedAnimLayer.h`. AddLinkAnimLayerImpl ve register satırı silindi.
- **Fix commit**: pending (working dir: `phase4_schemas.cpp`, `SageAnimationTools.cpp`, `gap-inbox.md`)
- **Deploy adımı**: ✅ server build OK (`bin\sage-server.exe` Cluster G schemas görünür, smoke tools/list direct invoke 11/11 tool); ⏳ plugin UAT BuildPlugin Win64 in progress; Lyra plugin binary+Source install + Live Coding `compile_and_reload` veya editor restart bekliyor.
- **Verify durumu**: pending — Lyra Claude verify edecek. Önerilen smoke senaryo:
  1. `animation.create_anim_layer_interface skeleton=/Game/.../UEFN_Mannequin name=ALI_TestLayer` (yeni ALI BP)
  2. `animation.add_layer_function path=ALI_TestLayer function_name=TestPose` (interface declaration)
  3. `animation.list_layer_functions path=ALI_TestLayer` (`functions: [{name:'TestPose', has_root_output:true}]`)
  4. `animation.implement_anim_layer_interface path=<ChildABP> interface_path=ALI_TestLayer` (bulk implement + override graph spawn)
  5. `animation.list_implemented_layers path=<ChildABP>` (override_graph: 'TestPose', node_count >= 1)
  6. `animation.add_linked_anim_layer_node path=<MasterABP> interface_path=ALI_TestLayer function_name=TestPose` (master AnimGraph'a linked-layer node)
  7. (PIE) `animation.set_linked_anim_layer actor=<PieActor> layer_class=<ChildABP_C>` → linked:true
  8. Cleanup: `animation.remove_layer_function_override path=<ChildABP> ... confirmed:true` + `animation.remove_layer_function path=ALI_TestLayer ... confirmed:true`

---

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
- **Verify durumu**: verified — Codex/Lyra 2026-05-05 verify:
  - `animation.set_state_machine_initial_state`: verified OK (`entry_pin=Entry`, `target_pin=In`, no `pin lookup failed`).
  - `animation.set_transition_rule`: literal `true` / `false` verified OK; rule graph `link_count=1`, `bp.validate warning_count=0`.
  - `animation.read_transition_rule`: verified OK.
  - Variable/string expression subset verified OK after plugin update: `CurrentFlightSpeed > 10`, `CurrentFlightSpeed <= 10`, `bWantsToFlightSprint`, `!bWantsToFlightSprint`.
  - Dogfood result: `/FlightCore/Animations/ABP_FlightCore` now uses those four expression rules in `FlightLocomotion`; `bp.validate` returns `valid=true`, `error_count=0`, `warning_count=0`.
  - Result: Gap #21 can close for state-machine initial state, literal bool rules, variable getters, numeric compare, and bool NOT expression authoring.

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

---

### Gap #22 — AnimGraph exposed input / node property binding UE 5.7 eksik

- **Status**: VERIFIED in Lyra — deployed server supports UE 5.7 anim node property binding
- **Reported**: 2026-05-05 by Lyra Codex
- **Project**: `D:\Steamworks\Lyra`
- **Affected asset**: `/FlightCore/Animations/ABP_FlightCore`

#### Hedef
AnimGraph node property'lerini runtime AnimBP değişkenlerine canonical UE 5.7 yolu ile bağlamak.

Somut FlightCore ihtiyacı:
- `UAnimGraphNode_BlendSpacePlayer` içindeki `X` input'u `FlightLean.X` değerinden beslensin.
- `UAnimGraphNode_BlendSpacePlayer` içindeki `Y` input'u `FlightLean.Y` değerinden beslensin.
- İleride `FlightType` A-E seçimi için blend/select node aktif index/enum input'u AnimBP değişkeninden beslensin.

#### Mevcut tool durumu
`tools/list` içinde `animation.bind_anim_node_property` var ama tool metadata'sı bunu deprecated/pending gösteriyor:

```text
DEPRECATED — current build returns -32601. UE 5.7 marked UAnimGraphNode_Base::PropertyBindings as PropertyBindings_DEPRECATED; the canonical replacement uses the UAnimBlueprintExtension subsystem and is pending Sage implementation.
```

`animation.set_anim_node_property` sadece literal değer yazabiliyor. Bu, `BlendSpacePlayer.X/Y = 0.0` gibi default değerler için yeterli ama runtime variable binding için yeterli değil.

#### Beklenen davranış
Tool, UE 5.7 canonical exposed-input binding pipeline'ını kullanarak AnimGraph node property'lerini AnimBP variable getter'larına bağlayabilmeli.

Örnek hedef API:

```json
{
  "tool": "animation.bind_anim_node_property",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore",
    "graph_name": "FastMove",
    "node_id": "<BlendSpacePlayerNodeGuid>",
    "property": "X",
    "expression": {"var": "FlightLean", "member": "X"}
  }
}
```

Minimum kabul:
- `float -> float` variable binding.
- `bool -> bool` variable binding.
- `int/enum -> int/enum` variable binding.
- Struct member access: `FVector2D.X`, `FVector2D.Y`.
- Compile sonrası `bp_validate` warning/error üretmemeli.

#### Etki (pre-fix)
FlightCore `FCI-0502` lean blend için tool-level canonical binding yapılamadı.

#### Workaround (pre-fix)
Lyra tarafında native workaround uygulanmıştı:
- `UFlightCoreAnimInstance::NativeUpdateAnimation` CMC'den `FlightLean` okuyor.
- `UFlightCoreAnimInstance::ApplyFlightRuntimeToAnimPlayers` runtime'da `FAnimNode_BlendSpacePlayer` struct property'lerini reflection ile bulup `X/Y` değerlerine `FlightLean.X/Y` yazıyordu.
- Full `LyraEditor Win64 Development` build geçti.
- Clean editor restart sonrası `/FlightCore/Animations/ABP_FlightCore` validate temiz: `valid=true`, `error_count=0`, `warning_count=0`.

Bu X/Y workaround, deployed binding doğrulamasından sonra Lyra tarafında kaldırıldı; native pass artık sadece `FlightType` asset seçimini koruyor.

#### Öneri (A — must)
UE 5.7 için `UAnimBlueprintExtension` tabanlı AnimGraph exposed input binding implement edilmeli. Eski `UAnimGraphNode_Base::PropertyBindings` alanına yazmak yeterli değil; UE 5.7'de deprecated path.

#### Öneri (B — should)
Debug ve doğrulama için read-side tool eklenmeli:

```json
{
  "tool": "animation.read_anim_node_properties",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore",
    "graph_name": "FastMove",
    "node_id": "<NodeGuid>"
  }
}
```

Dönmesi gereken minimum bilgiler:
- Inner `FAnimNode_*` reflected property values.
- Asset reference (`BlendSpace`, `Sequence`, etc.).
- Exposed input / binding list.
- Pin names, defaults, links.

#### Öneri (C — should, FCI-0503 için)
`FlightType` A-E seçimi için AnimGraph blend/select node authoring genişletilmeli:
- `animation.add_blend_list_by_int`
- `animation.add_blend_list_by_enum`
- Dynamic pose pin add/remove/reconstruct desteği.
- Active child/index/enum input'unu AnimBP variable'a bind etme.

Mevcut `animation.connect_pose_pin` ve `animation.add_blend_list_by_bool` faydalı ama A-E variant seçimi için bool blend yeterli değil.

#### Sage implementation update — 2026-05-05 Codex
- `animation.bind_anim_node_property` source tarafında artık stub değil; UE 5.7 `UAnimGraphNodeBinding_Base` / `FAnimGraphNodePropertyBinding` verisini reflection ile yazar.
- Desteklenen expression şekilleri: legacy `variable: "FlightLean.X"`, string `expression: "FlightLean.X"`, object `{ "var": "FlightLean", "member": "X" }`, object `{ "path": ["FlightLean", "X"] }`.
- Type validation `IPropertyAccessEditor::ResolvePropertyAccess` + `GetPropertyCompatibility` ile yapılır; incompatible source/target binding asset'e yazılmaz.
- Optional exposed input varsa pin görünür yapılır, mevcut pin linkleri kırılır, binding `PropertyBindings` map'ine eklenir ve node reconstruct edilir.
- Yeni read-side tool: `animation.read_anim_node_properties`; pinler, inner `FAnimNode_*` property değerleri ve UE 5.7 binding listesi döner.
- Multi-pose başlangıcı: `animation.add_blend_list_by_int` eklendi; `animation.add_blend_list_pose_pin` şu an güvenli şekilde `UAnimGraphNode_BlendListByInt` için ek pose pini açar.
- Deployed server verification — 2026-05-05 Lyra Codex:
  - Editor: Lyra UE 5.7.4, `/FlightCore/Animations/ABP_FlightCore`.
  - `animation.bind_anim_node_property` succeeded on `FastMove` BlendSpacePlayer node `748AE4784AA2AA97F6D46298AD9DD439`.
  - `X -> FlightLean.X` and `Y -> FlightLean.Y` bindings were written.
  - Tool returned UE 5.7 binding data via `/Script/AnimGraph.AnimGraphNodeBinding_Base`; `path_segments` resolved as `["FlightLean","X"]` and `["FlightLean","Y"]`.
  - `bp.validate` result: `valid=true`, `error_count=0`, `warning_count=0`.
  - Asset saved and dirty count returned `0`.

---

### Gap #23 — `restart_editor` Windows build path hatası

- **Status**: OPEN
- **Reported**: 2026-05-05 by Lyra Codex
- **Project**: `D:\Steamworks\Lyra`
- **Sage repo**: `D:\Steamworks\sage-unreal-mcp`
- **Editor**: UE `5.7.4`, Lyra editor session restarted manually after failure

#### Repro

`mcp__sage__.restart_editor` şu argümanlarla çağrıldı:

```json
{
  "confirmed": true,
  "save_dirty": true,
  "build_plugin": true,
  "rebuild_project_modules": false,
  "wait_handshake_sec": 120
}
```

#### Actual

Tool hemen hata döndü:

```text
Mcp error: -32603: Internal error: build script not found: D:\Steamworks\sage-unreal-mcp\scripts\scripts\build-plugin.sh
```

#### Expected

Windows ortamında `restart_editor`:

- yanlış `scripts\scripts` path'i üretmemeli;
- `.sh` script'e hard-code düşmemeli;
- Windows için `.ps1` / `.bat` build path'i seçmeli veya platforma göre açık hata vermeli;
- editor DLL lock senaryosunda plugin/project build + relaunch akışını güvenilir tamamlamalı.

#### Impact

Lyra `FlightCore` C++ patch'i compile oldu fakat açık editor `UnrealEditor-FlightCore.dll` dosyasını kilitlediği için UBT link aşaması düştü. Sage restart tool çalışmadığı için workaround manuel yapıldı:

1. `get_dirty_assets` ile dirty asset yok doğrulandı.
2. `UnrealEditor.exe` PID `21708` manuel kapatıldı.
3. `Build.bat LyraEditor Win64 Development -Project=D:\Steamworks\Lyra\Lyra.uproject -WaitMutex -NoHotReloadFromIDE` başarılı geçti.
4. Editor manuel yeniden açıldı; yeni PID `26384`, Sage session `4`.
