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

## Gap #28 — `animation.add_linked_anim_layer_node` Layer property silently atlanıyor — node "Layer=None" ile spawn ediliyor, response yanıltıcı

- **Status**: ✅ FIXED — deploy/verify pending
- **Reported**: 2026-05-06 19:30 tarafından Lyra Claude / FlightCore asset wiring sprint (Gap #27 v2 fix verify oturumu)
- **Project**: `D:\Steamworks\Lyra`
- **Editor**: Lyra UE 5.7.4 (post Gap #27 v2 fix deploy — engine flow by-pass çalışıyor, master AnimGraph mevcut weapon ALI node'ları intact)
- **Affected assets**:
  - `/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin` (master, ALI_FlightLocomotionLayer implement edilmiş)
  - `/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C` (1 fonksiyon: `FlightLocomotionPose`)
- **Related**: Gap #27 v2 fix doğrulandı (master mevcut node'lar intact). Bu gap **yeni eklenen FlightCore linked-layer node**'unun Layer property write tarafında.

### Hedef

State-aware blend chain'inin son adımı: master AnimGraph'a yeni FlightLocomotionPose çağrısı yapan UAnimGraphNode_LinkedAnimLayer node ekle.

### Denenen tool çağrısı

```json
{ "tool": "animation.add_linked_anim_layer_node",
  "args": {
    "path": "/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin",
    "graph_name": "AnimGraph",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "function_name": "FlightLocomotionPose",
    "x": 1450, "y": 450,
    "compile": false
  } }
```

Response (yanıltıcı — başarılı görünüyor):
```json
{
  "class": "/Script/AnimGraph.AnimGraphNode_LinkedAnimLayer",
  "interface": "ALI_FlightLocomotionLayer_C",
  "interface_function": "FlightLocomotionPose",
  "node_id": "CDA94EAA4AE4A8F4B739D0BB24F1054C",
  "output_pose_pin": "Pose",
  "pin_count": 1
}
```

### Sonuç / hata

**`bp.search_nodes path=master keyword=ALI_FlightLocomotionLayer`** → 1 hit:
```json
{"id": "CDA94EAA4AE4A8F4B739D0BB24F1054C", "title": "ALI_FlightLocomotionLayer - None"}
```

Yani node title `ALI_FlightLocomotionLayer - None` — Layer **NOT SET** UE-side.

**`animation.read_anim_node_properties`** raporu (raw CDO property okuma):
```json
{
  "properties": [
    {"name": "Interface", "value": ".../ALI_FlightLocomotionLayer_C"},   // ✓ doğru
    {"name": "Layer", "value": "FlightLocomotionPose"}                    // ✓ CDO doğru — AMA UE compile fark etmiyor
  ]
}
```

**CDO property seviyesinde Layer="FlightLocomotionPose" SET. Ama UE compile, node title, validate "None" görüyor.**

**`bp.validate path=master`**:
```text
"Linked anim layer node ALI_FlightLocomotionLayer - None does not specify a layer." (error)
"Missing allocated node for AnimGraphNode_LinkedAnimLayer_4 ..." (error)
```

### Workaround denemeleri (hepsi başarısız)

1. `animation.set_anim_node_property property=Layer value=FlightLocomotionPose` → response `{"set": true}` ama compile hala 2 error.
2. `bp.refresh_nodes` (633 node reconstruct) → compile hala 2 error.
3. `bp.search_nodes "FlightLocomotionPose"` → 0 hits (Layer adı title'da yok hala).

Workaround **bulunamadı**. Sadece manuel UE Editor work:
- Master ABP'yi editor'de aç → AnimGraph → CDA94EAA node'una çift tıkla → Details panel → Settings > Layer dropdown'undan "FlightLocomotionPose" seç
- Bu UE Editor UI dropdown click `UAnimGraphNode_LinkedAnimLayer::ChangeLayer` pipeline'ını çağırır → CDO Layer set + ReconstructNode + interface property cache update + title regen

### Beklenen davranış

`animation.add_linked_anim_layer_node` `function_name` parametresi alıyor — bu UE-side `Layer` FName property'sine yazılmalı. Plus **UE'nin canonical "ChangeLayer" pipeline'ı** çağrılmalı:

```cpp
void UAnimGraphNode_LinkedAnimLayer::SetLayer(FName NewLayer)
{
    Node.Layer = NewLayer;
    PostEditChangeProperty(PropertyChange);  // tetikleyici
    ReconstructNode();                        // pin'leri yeniden allocate et, interface fonksiyonu binding kur
    // Plus: title cache + visualization update
}
```

Sage tool muhtemelen `Node.Layer = FName` raw set yapıyor ama `PostEditChangeProperty` + `ReconstructNode` adımlarını atlıyor. Sonuç: CDO property seviyesinde set ama UE compile path'inde Layer="None" görünüyor.

### Öneri (öncelik A/B/C)

**A — must (Gap #28 close şartı)**

1. `animation.add_linked_anim_layer_node` Layer set sırasında **ChangeLayer** pipeline'ı:
```cpp
NodePtr->Node.Interface = InterfaceClass;
NodePtr->Node.Layer = FunctionName;
// CRITICAL: trigger UE's reconstruction pipeline
FProperty* LayerProperty = FindFProperty<FProperty>(FAnimNode_LinkedAnimLayer::StaticStruct(), TEXT("Layer"));
FPropertyChangedEvent PropertyChange(LayerProperty, EPropertyChangeType::ValueSet);
NodePtr->PostEditChangeProperty(PropertyChange);
NodePtr->ReconstructNode();
```

2. `animation.set_anim_node_property property=Layer` aynı eksiklikte: raw CDO write ama UE pipeline atlanıyor. Aynı fix gerekir.

**B — should**

3. Response payload doğrulama: tool dönmeden önce `bp.search_nodes` benzeri internal check ile **node title'ında Layer adı görünüyor mu** kontrol et. Eğer title hala `<Interface> - None` ise tool error dönsün — silently başarı raporlama:
```json
{
  "error": "Layer property write succeeded at CDO level but UE pipeline did not reconstruct the node — see Gap #28",
  "node_id": "...",
  "expected_layer": "FlightLocomotionPose",
  "actual_layer_in_title": "None"
}
```

**C — nice-to-have**

4. `animation.repair_linked_anim_layer_nodes` convenience tool — projedeki tüm UAnimGraphNode_LinkedAnimLayer node'larını tara, `Node.Layer` CDO property'si dolu ama node title `<Interface> - None` olanları **ChangeLayer** pipeline'ı ile fix et. Eski sprint'lerden artık corrupt master ABP'leri kurtarmak için.

### Notlar — context

- Gap #27 v2 fix doğrulandı:
  - `bp.add_interface` response: `used_direct_anim_layer_interface_add: true`, `interface_graph_guids_regenerated: 1`, `linked_layer_engine_side_effect_count: 0`, `linked_layer_snapshot_node_count: 14` ✓
  - Master AnimGraph'taki mevcut 4 weapon ALI LinkedAnimLayer node Layer property'leri intact (LeftHandPose_OverrideState dahil) ✓
- Gap #28 ayrı tetikleyici: **yeni eklenen FlightCore node**'unun Layer write'ı eksik.
- Workaround: master ABP rebuild (6. duplikasyon!) + ALI implement YOK + state-aware blend deferred. Multiplayer C++ infra (StateComponent + CMC + Pawn BP wire + GameFeatureData AddComponents + ABP_FlightCore_Locomotion child) ready.

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-06
- **Kök sebep**: `AddLinkedAnimLayerNodeImpl` (Cluster G, `SageAnimationTools.cpp:8125`) `Node->Node.Layer = FName(*FuncName)` direct write yapıyordu ama UE'nin **`PostEditChangeProperty(Layer)` event'i** atlanıyordu. `UAnimGraphNode_LinkedAnimLayer::PostEditChangeProperty` Layer property için `ChangeLayer` pipeline'ını tetikler — bu pipeline node title cache'ini regen eder, Interface UClass binding'ini günceller, internal pin allocation map'ini refresh eder. CDO seviyesinde Layer set OK görünüyordu ama UE compile path'i (`bp.validate`, `bp.search_nodes title`, runtime layer binding) "None" görüyordu. Aynı eksik `SetAnimNodePropertyImpl`'da da vardı — generic property set sonrası PostEditChangeProperty fire edilmiyordu. Mahmut'un Gap #26 `SetLinkedLayerNameForRename` helper'ında zaten doğru pattern (FProperty find + FPropertyChangedEvent + PostEditChangeProperty) kullanmıştı; aynı pattern spawn time + generic set time için yoktu.
- **Fix scope (3 nokta + 1 yeni tool, atomic commit)**:
  1. **`AddLinkedAnimLayerNodeImpl` ChangeLayer pipeline** (A-must, `SageAnimationTools.cpp:8125`): spawn pipeline yeniden sıralandı — (a) NewObject + GUID + position + AddNode, (b) Interface/Layer/InstanceClass direct set (PostPlacedNewNode'a hazırlık), (c) PostPlacedNewNode + AllocateDefaultPins (canonical spawn), (d) `FirePropertyChange("Interface")` + `FirePropertyChange("Layer")` + (varsa) `FirePropertyChange("InstanceClass")` lambda'sı `FAnimNode_LinkedAnimLayer::StaticStruct()->FindPropertyByName(...)` + `FPropertyChangedEvent(P, ValueSet)` + `Node->PostEditChangeProperty(E)` üçlüsünü çağırır, (e) ReconstructNode (idempotent finalize, title cache regen).
  2. **`SetAnimNodePropertyImpl` generic PostEditChangeProperty** (A-must extended, `SageAnimationTools.cpp:5269`): SetPropertyValueAtPtr başarılı sonrası, wrapper class property'si varsa onu, yoksa inner struct property'sini kullanarak `FPropertyChangedEvent(EventProp, ValueSet)` + `AnimNode->PostEditChangeProperty(ChangeEvent)` + `AnimNode->ReconstructNode()`. Tüm anim node property'leri için ChangeLayer-tier pipeline tetiklenir (Layer/Interface/InstanceClass yanı sıra StateMachine sub-graph rebind, vb.).
  3. **B-should response title verification** (`SageAnimationTools.cpp:8195-8210`): tool dönmeden önce `Node->GetNodeTitle(ENodeTitleType::FullTitle)` ile rendered title oku, function adı title'da yoksa response'a `_warning` + `node_title` + `layer_resolved: false` ekle. Silently başarı raporlama yerine kullanıcıya pipeline failure'ını surface et.
  4. **C-nice yeni tool `animation.repair_linked_anim_layer_nodes`** (`SageAnimationTools.cpp:8779`): projedeki tüm UAnimGraphNode_LinkedAnimLayer node'larını sweep et (path verilirse single BP scope, yoksa AssetRegistry + TObjectIterator full scan), Layer FName set ama node title `<Interface> - None` olanları detect et, her biri için ChangeLayer pipeline'ı (PostEditChangeProperty(Interface) + PostEditChangeProperty(Layer) + ReconstructNode) fire et. `dry_run` flag desteği. Eski Sage spawn path'leriyle bozulan ABP'leri kurtarır. Local graph traversal (`CollectAnimGraphs` lambda — `FBpGraphEntry/CollectAllGraphs` SageBlueprintTools.cpp anonymous namespace içinde, cross-TU erişilemez; LinkedAnimLayer node'ları sadece anim graph schema'lı graph'larda yaşar, FunctionGraphs + UbergraphPages + MacroGraphs + ImplementedInterfaces[].Graphs taraması yeterli).
- **Schema güncellemeleri** (`phase4_schemas.cpp`): `animation.repair_linked_anim_layer_nodes` yeni schema (path/dry_run/compile optional). Tool count 560 → 561.
- **Fix commit**: pending (working tree).
- **Deploy adımı**: ✅ server build OK (`bin\sage-server.exe` 18:25 — phase4_schemas.cpp recompile, sage-server.exe relink); ⏳ plugin UAT BuildPlugin Win64 in progress (ilk pass `FBpGraphEntry`/`CollectAllGraphs` cross-TU erişim hatası vermişti, local `CollectAnimGraphs` lambda ile fix); Lyra + HeroFlight deploy bekliyor.
- **Verify durumu**: pending — Lyra Claude verify edecek. Önerilen smoke senaryo:
  1. Master ABP rebuild (mevcut bozuk node temiz başlangıç için): `delete_asset` + `duplicate_asset ABP_Mannequin_Base → ABP_HeroFlight_Mannequin` + `bp.add_interface ALI_FlightLocomotionLayer_C` + `bp.reparent UFlightCoreAnimInstance`.
  2. `animation.add_linked_anim_layer_node path=master graph_name=AnimGraph interface_path=ALI_FlightLocomotionLayer function_name=FlightLocomotionPose x=1450 y=450`.
  3. Response yeni field'lar: `node_title: "ALI_FlightLocomotionLayer - FlightLocomotionPose"`, `layer_resolved: true` (önceden Layer=None idi, artık fix). `_warning` ALAN olmamalı.
  4. `bp.search_nodes path=master keyword=FlightLocomotionPose` → 1 hit, `title: "ALI_FlightLocomotionLayer - FlightLocomotionPose"`.
  5. `bp.validate path=master` → 0 error 0 warning.
  6. **Repair tool dry_run smoke**: `animation.repair_linked_anim_layer_nodes path=master dry_run=true` → `needs_repair_count: 0`, `repaired_node_count: 0`, `details: []` (yeni spawn pipeline temiz).
  7. **Repair tool full project scan** (eski bozuk node varsa): `animation.repair_linked_anim_layer_nodes dry_run=false compile=true` → `repaired_node_count: <N>`, `affected_blueprints[]` listele, her detail `repaired: true`.

---

## Gap #27 — `bp.add_interface` AnimLayerInterface eklendiğinde master AnimGraph'ta sessiz yan etki: mevcut node Layer property bozulması + Layer=None ek node spawn

- **Status**: ✅ FIXED — second fix deployed 2026-05-06 19:12; Lyra verify pending
- **Reported**: 2026-05-06 18:35 tarafından Lyra Claude / FlightCore asset wiring sprint (Gap #26 fix deploy sonrası)
- **Project**: `D:\Steamworks\Lyra`
- **Editor**: Lyra UE 5.7.4 (post Gap #26 fix — bp.rename_function interface-aware artık çalışıyor; bu gap **bp.add_interface**'in farklı yan etkisi)
- **Related**: Gap #26 (bp.rename_function side-effect, FIXED) — aynı tip cross-BP rename impact analysis ama farklı tetikleyici (rename değil interface ekleme).

### Hedef

Master ABP'ye AnimLayerInterface'i clean ekle — mevcut master AnimGraph'taki başka interface'lerin node'larına dokunmadan. Lyra-canonical pattern: master ABP **ABP_Mannequin_Base** Lyra core duplikasyonu zaten ALI_ItemAnimLayers interface'ini implement eder ve AnimGraph'ında 4-5 LinkedAnimLayer node ile weapon ALI fonksiyonlarını çağırır. FlightCore plugin master'a **ek olarak** ALI_FlightLocomotionLayer interface'ini eklemek istiyor — mevcut weapon ALI node'ları intact kalmalı.

Akış:
1. duplicate_asset ABP_Mannequin_Base → ABP_HeroFlight_Mannequin (master fresh duplikasyon)
2. bp_reparent ABP_HeroFlight_Mannequin → UFlightCoreAnimInstance (parent class değişimi, bIsFlightActive UPROPERTY inherited gelir)
3. **bp_add_interface ALI_FlightLocomotionLayer_C** ← BU ADIM YAN ETKİ ÜRETİYOR
4. animation_add_linked_anim_layer_node ile FlightLocomotionPose call ekle
5. ...

### Denenen tool çağrısı

```json
{ "tool": "bp.add_interface",
  "args": {
    "path": "/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C"
  } }
```

Sonuç: `{"already": false, "interface": "ALI_FlightLocomotionLayer_C", ...}` — başarılı görünüyordu.

### Sonuç / hata

Yan etki: master ABP'nin mevcut AnimGraph'ında **iki problem** üretildi:

**Problem 1**: Master AnimGraph'taki **mevcut bir UAnimGraphNode_LinkedAnimLayer node**'unun (Interface=ALI_ItemAnimLayers, Layer=`LeftHandPose_OverrideState`) Layer property'si silently `FlightLocomotionPose` olarak değişti. Hemen Gap #26 ile aynı: cross-interface Layer name collision.

`bp.search_nodes path=master keyword=FlightLocomotionPose`:
```json
{"hits": [{"class": "AnimGraphNode_LinkedAnimLayer", "graph": "AnimGraph", "id": "4E18B10B484F1B194FD6A789AB6B5576", "title": "ALI_ItemAnimLayers - FlightLocomotionPose"}]}
```

Bu node ALI_ItemAnimLayers (Lyra core) interface'ini kullanıyor, ama Layer="FlightLocomotionPose" — ALI_ItemAnimLayers'ta **böyle bir fonksiyon yok**. ALI_ItemAnimLayers Lyra core asset, dokunulmadı (14 weapon-side fonksiyona sahip — `LeftHandPose_OverrideState` dahil).

**Problem 2**: Master AnimGraph'ta otomatik bir yeni Layer=None node spawn edildi (Interface=ALI_FlightLocomotionLayer_C, Layer=None).

`bp.validate path=master`:
```text
"Linked anim layer node ALI_ItemAnimLayers - FlightLocomotionPose uses invalid layer 'FlightLocomotionPose'." (error)
"Linked anim layer node ALI_FlightLocomotionLayer - None does not specify a layer." (error)
"Missing allocated node for AnimGraphNode_LinkedAnimLayer_4 ..." (error)
"Missing allocated node for AnimGraphNode_LinkedAnimLayer_2 ..." (error)
```

**Tetikleyici testi (kontrollü)**:
1. duplicate_asset ABP_Mannequin_Base → ABP_HeroFlight_Mannequin → bp.validate → 0 error ✓
2. bp_reparent ABP_HeroFlight_Mannequin → UFlightCoreAnimInstance → bp.validate → 0 error ✓
3. **bp_add_interface ABP_HeroFlight_Mannequin + ALI_FlightLocomotionLayer_C** → bp.validate → 4 errors (yan etki tetiklendi)

ALI_FlightLocomotionLayer'ın tek fonksiyonu `FlightLocomotionPose`. ALI_ItemAnimLayers'ta `FlightLocomotionPose` yok. Sage `bp_add_interface` muhtemelen master AnimGraph'taki mevcut LinkedAnimLayer node'larından birini (örn. ID=4E18B10B... başlangıçta `LeftHandPose_OverrideState`) update ediyor — eklenen interface'in fonksiyon adına göre, interface property'sini check etmeden.

### Beklenen davranış

`bp.add_interface` **hiçbir AnimGraph node'una dokunmamalı**. UInterface implementation = sadece BP'nin `ImplementedInterfaces` listesine entry ekleme operasyonu. AnimGraph'taki node mutations bu tool'un scope'u dışı.

Plus: yeni interface fonksiyonları için master AnimGraph'a otomatik node spawn ETMEMELI. Kullanıcı `animation.add_linked_anim_layer_node` ile manuel olarak ekler. (Gap #24 Cluster G zaten bu primitive'i sağlıyor.)

### Workaround

Master ABP rebuild:
```text
1. delete_asset ABP_HeroFlight_Mannequin
2. duplicate_asset ABP_Mannequin_Base → ABP_HeroFlight_Mannequin (3. kez!)
3. bp_reparent → UFlightCoreAnimInstance
4. bp_add_interface SKIP (state-aware blend ayrı sprint'e bırakıldı)
5. master compile → 0 error ✓
6. B_Hero_HeroFlight Mesh.AnimClass = master tekrar set
```

Mevcut state: master ABP weapon ALI intact (Lyra ground gameplay tam), ALI_FlightLocomotionLayer master tarafından implement edilmedi → CMC.LinkAnimClassLayers fire etse de master'da hiç fire etmez. State-aware blend (`Movement.Mode.Flight` tag bool ile BlendByBool) Gap #27 fix gelene kadar erteleniyor.

### Öneri (öncelik A/B/C)

**A — must (Gap #27 close şartı)**

1. `bp.add_interface` cross-BP node mutation YAPMAMALI. Sadece `BP->ImplementedInterfaces.Add(InterfaceClass)` + Compile. AnimGraph mutations ayrı tool (`animation.add_linked_anim_layer_node`) tarafından yapılır.

2. Eğer Sage'in mevcut `bp.add_interface`'i AnimLayerInterface için **otomatik convenience** mantığı (`ImplementNewInterface` UE flow'u) kullanıyorsa: bu davranışı **opt-in** flag'la sakla:
```json
{ "tool": "bp.add_interface",
  "args": { ..., "auto_spawn_layer_call": false }  // default false, opt-in true
}
```

3. AnimLayerInterface tetiklenen Layer rename davranışı: rename impact analysis Gap #26 fix'i interface-aware idi. Aynı pattern bp_add_interface tarafında uygulanmalı — yeni interface ekleme **mevcut node'ların Layer property'sini güncelleme yetkisine sahip değil** (interface match olsa bile, çünkü yeni interface'in fonksiyon adı mevcut interface'in fonksiyon adıyla çakışıyor olabilir).

**B — should**

4. Response payload'a side-effect rapor ekle:
```json
{
  "already": false,
  "interface": "ALI_FlightLocomotionLayer_C",
  "linked_layer_nodes_modified": 0,
  "linked_layer_nodes_spawned": 0
}
```
Bu yan etkilerin görünür olmasını sağlar.

**C — nice-to-have**

5. Gap #26 + Gap #27 birlikte ele alınmalı: `bp.rename_function` ve `bp.add_interface` rename impact'i için **canonical interface match policy**:
   - `Node->Node.Interface == HitTestInterfaceClass` strict eşleşme
   - Layer name eşleşmesi tek başına yeterli **DEĞİL**
   - Şüpheli mutation noktaları için unit test: ABP_Mannequin_Base × ALI_FlightLocomotionLayer (FullBody_* kullanılan + LeftHandPose_OverrideState × farklı interface FlightLocomotionPose senaryosu)

### Notlar — context

- Gap #26 fix doğru çalışıyor (rename çağrısı yan etki üretmedi — kontrollü test geçildi). Bu gap **rename değil interface ekleme** tarafında.
- Master ABP_HeroFlight_Mannequin Lyra `ABP_Mannequin_Base` duplikasyonu — content team Lyra'nın master ABP'sinin (5 LinkedAnimLayer node + state machine + ApplyAdditive + LayeredBoneBlend + ControlRig + RotateRootBone + Slot + Inertialization) zenginliğini koruyor. **Bu mannequin master Lyra'nın hero AnimBP iskeleti**, sürekli tekrar duplikasyon riski iş büyütüyor.
- Workaround zincirinden geçince Sage'e güvensizlik: AnimLayerInterface eklemek için her seferinde master ABP'yi sıfırlamak gerekir. Production-grade plugin asset workflow için Gap #27 close ŞART.
- Mevcut FlightCore sprint state stable: master ABP intact (weapon ALI çalışıyor, ground gameplay temiz), ALI_FlightLocomotionLayer master tarafından implement edilmedi (state-aware blend bekleniyor). Multiplayer C++ infra (StateComponent + CMC + GameFeatureData + Pawn BP wire) ready.

### Verify result — Lyra Claude 2026-05-06 18:55 (RE-OPEN)

Sage fix deploy edildi, kontrollü test sonucu **partial fix**:

**B önerisi uygulandı ✓**: `bp.add_interface` response payload'a side-effect tracking field'ları eklendi:
```json
{
  "already": false,
  "anim_layer_interface": true,
  "linked_layer_changes": [],
  "linked_layer_engine_side_effect_count": 0,
  "linked_layer_nodes_modified": 0,
  "linked_layer_nodes_removed": 0,
  "linked_layer_nodes_restored": 0,
  "linked_layer_nodes_spawned": 0
}
```

**A önerisi uygulanmadı ✗**: Tool response'u "0 modified" rapor ediyor AMA UE engine-side gerçek mutation hala oluyor. Kontrollü test:

1. Master ABP fresh duplikasyon (ABP_Mannequin_Base) → `bp.validate` → **0 error** ✓
2. `bp.reparent` → UFlightCoreAnimInstance → `bp.validate` → **0 error** ✓
3. `bp.add_interface` ALI_FlightLocomotionLayer_C → response **0 yan etki** rapor ediyor
4. Hemen `bp.validate` → **2 error**:
   ```
   "Linked anim layer node ALI_ItemAnimLayers - FlightLocomotionPose uses invalid layer 'FlightLocomotionPose'." (error)
   "Missing allocated node for AnimGraphNode_LinkedAnimLayer_2 ..." (error)
   ```
5. `bp.search_nodes keyword=FlightLocomotionPose` → 1 hit:
   ```json
   {"id": "D3A78B57422BB03D38E31CBCA2DFE0F4", "title": "ALI_ItemAnimLayers - FlightLocomotionPose"}
   ```

Yani master AnimGraph'taki mevcut bir `ALI_ItemAnimLayers` LinkedAnimLayer node'unun Layer property'si "FlightLocomotionPose" olarak silently değişti — Sage tool response'u "0 modified" rapor etmesine rağmen.

### Kök sebep tahmini (Lyra Claude)

Sage'in `bp.add_interface` UE'nin `FBlueprintEditorUtils::ImplementNewInterface` flow'unu çağırıyor olabilir. Bu UE engine flow'u **kendi içinde** AnimLayerInterface eklendiğinde master AnimGraph'taki mevcut LinkedAnimLayer node'larını "rename collision" mantığıyla update ediyor. Sage tool **bu engine-side mutation'ı algılamıyor**, response'unda 0 modified rapor ediyor.

Fix yönü:
1. Sage tool **UE engine flow'unu by-pass** etmeli — `BP->ImplementedInterfaces.Add(InterfaceClass)` direct mutation + manuel `BP->Modify()` + `KismetEditorUtilities::CompileBlueprint` (engine ImplementNewInterface convenience'ı atla).
2. Veya: engine flow'u çağırmadan önce master AnimGraph'taki **LinkedAnimLayer node Layer property snapshot'ı** al, çağrı sonrası karşılaştır, fark varsa **revert** + bilgi response'a ekle.

### Workaround (devam)

Master ABP rebuild + ALI implement etmeme stratejisi sürdürülüyor. State-aware blend Gap #27 close edilene kadar deferred.

5. duplikasyon (2026-05-06 19:00):
- delete + duplicate ABP_Mannequin_Base → ABP_HeroFlight_Mannequin
- bp_reparent → UFlightCoreAnimInstance
- **bp_add_interface SKIP** (yine yan etki)
- Mesh.AnimClass restore → 0 error

Multiplayer C++ infra ready, master AnimGraph intact, FlightCore ALI master tarafından implement edilmedi.

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-06
- **Kök sebep**: First fix sadece response/snapshot tarafini kapatti ama iki kritik nokta eksikti: scanner `CollectAllGraphs` ile master AnimGraph'taki gerçek `UAnimGraphNode_LinkedAnimLayer` node'unu her durumda görmüyordu; daha önemlisi UE `ConformAnimLayersByGuid`, AnimLayerInterface conform sırasında `Node->Node.Interface` kontrol etmeden sadece graph GUID eşleşmesiyle Layer rename yapıyor. Duplicated ALI graph GUID'i Lyra core ALI graph GUID'iyle çakışınca sonraki compile/validate yine `ALI_ItemAnimLayers` node'unu `FlightLocomotionPose` adına çekiyordu.
- **Fix commit**: pending (working tree: `plugin/Source/SageBridge/Private/Tools/SageBlueprintTools.cpp`, `server/src/main.cpp`, `gap-inbox.md`)
- **Deploy adımı**: ✅ server + plugin full build OK (`scripts/build-all.ps1 debug`, 2026-05-06 19:11); ✅ Lyra + HeroFlight `Plugins/SageBridge` full payload deploy (`SageBridge.uplugin`, `Binaries/Win64/*`, `Source/`); server baslatilmadi.
- **Verify durumu**: pending - second fix smoke: response `used_direct_anim_layer_interface_add=true`, `linked_layer_snapshot_node_count>0`; duplicated ALI collision varsa `interface_graph_guids_regenerated>0`; hemen sonraki `bp.validate` 0 error olmali ve `bp.search_nodes keyword=FlightLocomotionPose` mevcut `ALI_ItemAnimLayers` node'larini döndürmemeli.

---

## Gap #26 — `bp.rename_function` interface-agnostic Layer rename — proje genelinde LinkedAnimLayer node'larının Layer property'sini silently bozuyor

- **Status**: ✅ FIXED — deploy DONE (Lyra + HeroFlight 18:15), Lyra Claude verify pending
- **Reported**: 2026-05-06 17:55 tarafından Lyra Claude / FlightCore asset wiring sprint (Gap #25 verify sonrası)
- **Project**: `D:\Steamworks\Lyra`
- **Editor**: Lyra UE 5.7.4 (post Cluster G + Gap #25 fix deploy)
- **Affected assets**:
  - `/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C` (rename hedefi — `LeftHandPose_OverrideState` → `FlightLocomotionPose`)
  - `/FlightCore/Animations/ABP_HeroFlight_Mannequin.ABP_HeroFlight_Mannequin` (ABP_Mannequin_Base duplikasyonu — yan etki kurbanı, master AnimGraph'taki LinkedAnimLayer node'unun Layer property'si silently değişti)

### Hedef

ALI_FlightLocomotionLayer (ALI_ItemAnimLayers'tan duplikasyon) içindeki tek fonksiyonu rename etmek — `LeftHandPose_OverrideState` → `FlightLocomotionPose`. Yalnızca **bu interface BP'sinin kendi function graph'ı** etkilensin; **diğer interface'leri (ALI_ItemAnimLayers Lyra core)** kullanan node'lar etkilenmesin.

### Denenen tool çağrısı

```json
{ "tool": "bp.rename_function",
  "args": {
    "path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer",
    "old_name": "LeftHandPose_OverrideState",
    "new_name": "FlightLocomotionPose"
  } }
```

Sonuç: `{"actual": "FlightLocomotionPose", "blueprint": "ALI_FlightLocomotionLayer", ...}` — başarılı görünüyordu.

### Sonuç / hata

Beklenmeyen yan etki: ABP_HeroFlight_Mannequin'in (master ABP, ABP_Mannequin_Base duplikasyonu) AnimGraph'ında **mevcut bir UAnimGraphNode_LinkedAnimLayer node**'unun Layer property'si `LeftHandPose_OverrideState` → `FlightLocomotionPose` olarak silently değişti. **AMA** o node'un Interface property'si **`ALI_ItemAnimLayers`** (Lyra core, dokunulmadı, hala 14 weapon-side fonksiyona sahip — `LeftHandPose_OverrideState` dahil).

Sonuç: master AnimGraph'ta geçersiz kombinasyon — `Interface=ALI_ItemAnimLayers, Layer=FlightLocomotionPose` (ALI_ItemAnimLayers'ta `FlightLocomotionPose` yok).

`bp.validate` master ABP'yi compile ederken:

```text
"In use pin Input Pose no longer exists on node ALI_ItemAnimLayers - FlightLocomotionPose. Please refresh node or break links to remove pin." (error)
"Linked anim layer node ALI_ItemAnimLayers - FlightLocomotionPose uses invalid layer 'FlightLocomotionPose'." (error)
"Missing allocated node for AnimGraphNode_LinkedAnimLayer_2 while searching for node links — likely due to the node having outstanding errors." (error)
```

`bp.search_nodes path=ABP_HeroFlight_Mannequin keyword=FlightLocomotionPose`:
```json
{"hits": [{"class": "AnimGraphNode_LinkedAnimLayer", "graph": "AnimGraph", "id": "AF97636943A0D319615CFAAE72088F24", "title": "ALI_ItemAnimLayers - FlightLocomotionPose"}]}
```

Yani master ABP'deki **ALI_ItemAnimLayers** interface'ini kullanan bir node'un Layer property'si rename'in yan etkisiyle değişti. ALI_ItemAnimLayers Lyra core asset, dokunulmadı; master ABP duplikasyon zamanı doğru node yapısıyla geldi (Interface=ALI_ItemAnimLayers, Layer=LeftHandPose_OverrideState). Sage rename projeyi tarayıp aynı adlı **tüm Layer property'lerini** güncellemiş — interface eşleşmesini kontrol etmemiş.

### Beklenen davranış

`bp.rename_function` rename impact analysis cross-BP olmalı, **AMA interface boundaries kullanmalı**:

- Bir UAnimGraphNode_LinkedAnimLayer node'u rename edilecekse: **`Node->Node.Interface == RenamedInterfaceBP->GeneratedClass`** kontrolü zorunlu
- Aynı isimde fonksiyon farklı interface'lerde olabilir; bu **kasıtlı isim çakışması** (örn. ALI_ItemAnimLayers ve ALI_FlightLocomotionLayer'ın ikisinde de `LeftHandPose_OverrideState` adlı fonksiyon olabilir, farklı amaçlar için)
- Sage rename sadece **rename hedef interface'i (path)** ile interface property eşleşen node'ların Layer property'sini güncelleme

Pseudo-code:
```cpp
void RenameLayerFunction(UAnimBlueprint* InterfaceBP, FName OldName, FName NewName)
{
    UAnimBlueprintGeneratedClass* InterfaceClass = InterfaceBP->GeneratedClass;
    for (UAnimGraphNode_LinkedAnimLayer* Node : ScanAllLinkedAnimLayerNodesInProject())
    {
        // GUARD: only update if THIS node's interface matches
        if (Node->Node.Interface == InterfaceClass && Node->Node.Layer == OldName)
        {
            Node->Node.Layer = NewName;
            Node->ReconstructNode();
        }
        // else: don't touch — different interface, name collision is intentional
    }
}
```

Plus minor: rename impact node listesi response'a eklensin (`updated_node_count`, `affected_blueprints[]`) — sessiz yan etki bug'larının fark edilmesini kolaylaştırır.

### Workaround

1. Sage rename öncesi: hedef interface'in fonksiyon adının **proje genelinde başka bir BP'de aynı isimde** olup olmadığını kontrol et. Çakışma varsa rename hedef adını farklı seç (örn. `FlightLocomotion_Pose` veya `FlightCore_LocomotionPose` prefix ile).

2. Sage rename sonrası master ABP bozulmuşsa: master'ı silip ABP_Mannequin_Base'den **yeniden duplike** et. ALI_FlightLocomotionLayer interface implement adımı tekrar yapma — master AnimGraph intact kalsın (state-aware blend ayrı sprint).

Mevcut workaround uygulandı:
```text
delete_asset /FlightCore/Animations/ABP_HeroFlight_Mannequin
duplicate_asset /Game/.../ABP_Mannequin_Base → /FlightCore/Animations/ABP_HeroFlight_Mannequin
editor_set_property B_Hero_HeroFlight.CharacterMesh0.AnimClass = ABP_HeroFlight_Mannequin_C
bp.compile master + Pawn → 0 error 0 warning
```

### Öneri (öncelik A/B/C)

**A — must**
1. `bp.rename_function` rename impact analysis: UAnimGraphNode_LinkedAnimLayer node'larının Layer property'si interface-aware update edilmeli (yukarıda pseudo-code). Aynı pattern UAnimGraphNode_LinkedAnimGraph için de geçerli.

2. Response payload'ına side-effect rapor ekle:
```json
{
  "actual": "FlightLocomotionPose",
  "blueprint": "ALI_FlightLocomotionLayer",
  "updated_node_count": 0,
  "skipped_node_count": 1,
  "affected_blueprints": []
}
```
Şu an silently 1 node update edildi, response'ta hiç bilgi yok.

**B — should**
3. Cluster G `add_layer_function` yeni fonksiyon eklerken proje genelinde aynı adlı fonksiyon olup olmadığını **kontrol et** + warning dön. Aynı adlı fonksiyon farklı interface'te varsa `collision_warning: ["BP1.func", "BP2.func"]` field'ı ile kullanıcı bilinçli karar versin.

**C — nice-to-have**
4. `animation.rebuild_anim_blueprint_from_master` — yan etki sonrası master ABP'yi temizlemek için convenience tool: silmek + master'ından (parent_class veya ImplementedInterfaces[0]) duplike etmek + intact restore.

### Notlar — context

- Sage Cluster G fix (Gap #24) interface override graph spawn + master AnimGraph'a LinkedAnimLayer node ekleme tool'larını ekledi — verify sırasında Lyra ALI_ItemAnimLayers Lyra-core asset'iyle isim çakışması yan etkisi keşfedildi.
- Rename öncesi: ALI_FlightLocomotionLayer'da 14 weapon-style fonksiyon vardı (ALI_ItemAnimLayers duplikasyonu sebebiyle). Ben 13'ünü `bp.delete_function` ile sildim, sonra `LeftHandPose_OverrideState`'i rename ettim. Delete cascade master'a yansımadı (silinen fonksiyonlar master'daki node'lar zaten ALI_ItemAnimLayers'ı işaret ediyor, "ALI_FlightLocomotionLayer.<deleted>" bağı yoktu). Ama rename **tek bir interface'i hedefliyor olsa bile** yan etki cross-BP gözlemlendi.
- ABP_HeroFlight_Mannequin'in kendi `LeftHandPose_OverrideState` override fonksiyonu rename'den etkilenmedi (override fonksiyon graph'ı intact kaldı). Yan etki sadece master AnimGraph'taki **LinkedAnimLayer node'unun Layer property** field'ında. Yani rename impact analysis'in görünmez tarafı.
- Workaround sonrası FlightCore asset wiring stable: master ABP weapon ALI intact + ABP_FlightCore_Locomotion + ALI_FlightLocomotionLayer + Pawn BP wire + GameFeatureData ekli. Master'da FlightLocomotion linked-layer call'ı ŞUİAN YOK (state-aware blend ayrı sprint).

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-06
- **Kök sebep**: UE built-in `FBlueprintEditorUtils::RenameGraph` interface-agnostic — bir anim layer interface'in fonksiyon adı değişince UE proje genelinde TÜM `UAnimGraphNode_LinkedAnimLayer` node'larının `FAnimNode_LinkedAnimLayer::Layer` FName property'sini eski → yeni isim diye günceller, **`Node->Node.Interface` UClass eşleşmesini kontrol etmez**. Sage'in mevcut `BpRenameFunctionImpl` sadece `RenameGraph` çağırıp UE'nin agnostic davranışını kabul ediyordu; aynı isim farklı interface'lerde yaşıyorsa (örn. ALI_ItemAnimLayers Lyra-core ve ALI_FlightLocomotionLayer duplikasyonu ikisinde de `LeftHandPose_OverrideState`) silently corruption yaratıyordu.
- **Fix scope (Mahmut tarafından, 4 dosya, snapshot + reconcile pattern)**:
  1. **`BpRenameFunctionImpl` interface-aware snapshot+reconcile** (`SageBlueprintTools.cpp:3510`):
     - **Pre-rename**: `CaptureLinkedLayerRenameSnapshot` proje genelinde (`LoadAnimBlueprintsForLinkedLayerRenameScan` AssetRegistry filter + `TObjectIterator<UAnimBlueprint>` loaded-but-unsaved BP'ler için fallback) tüm `UAnimGraphNode_LinkedAnimLayer` node'larını gez, eski layer adına eşleşenleri iki set'e ayır:
       - `TargetOldLayerNodeKeys` — `Node->Node.Interface == RenamedInterfaceClass` (BPGC reload'a dayanıklı `IsSameGeneratedInterfaceAsset` ile `ClassGeneratedBy` fallback).
       - `OtherInterfaceOldLayerNodeKeys` — interface farklı ama eski layer adıyla aynı (collision riski).
     - **Rename**: `FBlueprintEditorUtils::RenameGraph` çağrı.
     - **Post-rename reconcile**: `ReconcileLinkedLayerRename` snapshot'la kıyas:
       - Target snapshot'taki node hâlâ OldLayer'da → **manuel** NewLayer set + `PostEditChangeProperty` veya `ReconstructNode` (UE rename onları düzgün update etmediyse).
       - Target snapshot'taki node NewLayer'da → already_updated, sayım.
       - Other-interface snapshot'taki node UE rename tarafından silently NewLayer'a çevrildiyse → **revert to OldLayer** + ReconstructNode (Gap #26 ana fix).
     - **Response payload zenginleştirildi**: `linked_layer_rename_applicable`, `pre_rename_target_node_count`, `pre_rename_other_interface_old_name_count`, `updated_node_count`, `manual_updated_node_count`, `repaired_node_count`, `skipped_node_count`, `affected_blueprints[]`, `linked_layer_changes[]` (her change `action ∈ {updated, already_updated, repaired_other_interface}` + `from_layer/to_layer` + interface).
     - Helper inventory: `LoadAnimBlueprintsForLinkedLayerRenameScan`, `IsSameGeneratedInterfaceAsset`, `LinkedLayerNodeKey`, `FLinkedLayerRenameSnapshot`/`FLinkedLayerRenameImpact` struct'lar, `ForEachLinkedAnimLayerNodeInProject` callback wrapper, `SetLinkedLayerNameForRename`, `AddLinkedLayerRenameChange`, `StringSetToJsonArray`, `GetAnimLayerInterfaceClassForRename` (sadece BPTYPE_Interface anim BP'ler için class döner; diğer BP türleri için no-op).
  2. **`add_layer_function` collision_warnings** (`SageAnimationTools.cpp` + `phase4_schemas.cpp`, B-should öneri): `BuildAnimLayerFunctionCollisionWarnings(CurrentBP, FunctionName)` proje genelinde diğer ALI'lerde aynı isimde fonksiyon var mı tara, varsa response'a `collision_warnings: [{type, blueprint, graph_name, interface_class, message}]` + `collision_warning_count` field'ları ekle (creation block etmeden, sadece warn). Helper: `LoadAnimLayerInterfacesForCollisionScan` (engine/script/temp/transient hariç).
  3. **Schema rename**: `animation.add_play_montage_notify_window` → `animation.add_slot_node` (description zaten "UAnimGraphNode_Slot" diyordu — schema name uyumsuzdu, fix). Plus `animation.add_layer_function` description'da `collision_warnings` + `collision_warning_count` mention.
  4. **Bonus fix'ler**: `RemoveVirtualBoneImpl` `path` → `skeleton` parameter rename (anim tool param naming convention). `restart_orchestrator.cpp` Windows path quoting (`quoteShellArg`), `normalizeRepoRoot` (yanlış set edilmiş `SAGE_REPO_ROOT=.../scripts` durumunu parent'a normalize), `findBuildPluginScript` + `buildPluginCommand` Windows-aware refactor (`set SAGE_UE_ROOT && powershell.exe -NoProfile ...`).
- **C nice-to-have (`animation.rebuild_anim_blueprint_from_master`)**: implement edilmedi — Lyra Claude'un workaround'u (`delete_asset` + `duplicate_asset` + `editor_set_property AnimClass`) zaten çalışıyor; convenience tool şimdilik gerekmez.
- **Fix commit**: pending (working tree). 4 fix dosyası + scripts/audit-tools.ps1 (yeni audit script) + bu triage.
- **Deploy adımı**: ✅ server build (Mahmut 2026-05-06 18:07, sage-server.exe 7.56 MB); ✅ plugin UAT BuildPlugin Win64 OK (`UnrealEditor-SageBridge.dll` 3037696 bytes 18:15, +28 KB Gap #26 helper'lar); ✅ Lyra + HeroFlight deploy 18:15 (her iki editor kapalı, file lock yok); ✅ tools/list smoke (560 tool, schema rename + add_layer_function collision_warnings description doğrulandı).
- **Verify durumu**: pending — Lyra Claude verify edecek. Önerilen smoke senaryo:
  1. Test ALI duplike et: `duplicate_asset /Game/.../ALI_ItemAnimLayers → /Game/Test/ALI_FlightLocomotionLayer_Test`. Master ABP'de mevcut LinkedAnimLayer node'u Interface=ALI_ItemAnimLayers Layer=`LeftHandPose_OverrideState` (Lyra-core, dokunulmamış).
  2. `bp.rename_function path=ALI_FlightLocomotionLayer_Test old_name=LeftHandPose_OverrideState new_name=FlightTestPose`.
  3. Response doğrula: `linked_layer_rename_applicable: true`, `pre_rename_target_node_count: 0` (test ALI henüz kullanılmıyor), `pre_rename_other_interface_old_name_count: 1` (master'daki ALI_ItemAnimLayers node'u), `updated_node_count: 0`, `repaired_node_count: 1` (Gap #26 ana fix — UE silently dokundu, Sage geri aldı), `linked_layer_changes[0].action: "repaired_other_interface"`.
  4. Master ABP `bp.validate` → 0 error 0 warning (ALI_ItemAnimLayers - LeftHandPose_OverrideState intact).
  5. `animation.add_layer_function path=ALI_FlightLocomotionLayer_Test function_name=LeftHandPose_OverrideState` (ALI_ItemAnimLayers'la collision) → `collision_warnings: [{type:"same_named_anim_layer_function", blueprint:"/Game/.../ALI_ItemAnimLayers", interface_class:"...ALI_ItemAnimLayers_C", message:"linked-layer renames must stay interface-aware"}]`, `collision_warning_count: 1`. Function yine de oluşturulur (block etmez).

---

## Gap #25 — Anim node spawn tool'ları interface override graph'larını graph_name lookup'ında bulamıyor

- **Status**: ✅ FIXED — deploy/verify pending (Lyra plugin install + Live Coding/restart)
- **Reported**: 2026-05-06 17:25 tarafından Lyra Claude / FlightCore asset wiring sprint (Gap #24 verify oturumu)
- **Project**: `D:\Steamworks\Lyra`
- **Editor**: Lyra UE 5.7.4 (Sage Cluster G post-deploy, plugin .dll 3.0 MB install OK)
- **Affected assets**:
  - `/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion` (parent `UFlightCoreAnimInstance`)
  - `/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C` (interface, 1 fonksiyon: `FlightLocomotionPose`)
- **Related**: Gap #24 (Cluster G real impl) — bu gap, Cluster G tool'larıyla yaratılan override graph'lara mevcut node-injection tool'larının (Cluster A/D) erişememesinden kaynaklanıyor. Cross-cluster graph lookup divergence.

### Hedef

Cluster G ile yaratılan child override graph'a (FlightLocomotionPose) bir SequencePlayer node ekleyip Output Pose'a bağlayarak MVP locomotion layer'ı tamamlamak.

Akış:
1. `animation.add_layer_function_override` ile `FlightLocomotionPose` override graph spawn (Gap #24 Tool A — verified working).
2. Bu graph'ın **içine** `animation.add_sequence_player` veya `animation.add_animgraph_node` ile bir UAnimGraphNode_SequencePlayer ekle.
3. `animation.connect_pose_pin` ile sequence player'ı Output Pose'a bağla.
4. Compile + verify.

Adım 2'de tool'lar `graph_name="FlightLocomotionPose"` lookup'ını yapamıyor.

### Denenen tool çağrıları

**Önce Tool A doğrulandı** (Gap #24 Cluster G, çalışıyor):

```json
{ "tool": "animation.add_layer_function_override",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "interface_path": "/FlightCore/Animations/ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "function_name": "FlightLocomotionPose",
    "compile": false
  } }
```

Sonuç: `{"already": true, "compiled": false, "function_name": "FlightLocomotionPose", "graph_name": "FlightLocomotionPose", "output_node_id": "F19B84214469C90EFB2B22A05358DAE1", "schema": "AnimationGraphSchema"}` ✓

**Force re-create teyit**: önce `animation.remove_layer_function_override` (`removed_node_count: 2` — graph gerçekten 2 node'la vardı), sonra `animation.add_layer_function_override` tekrar (yine `already=true`, yeni `output_node_id: F19B...`).

`bp_full_dump` CDO doğrulaması:

```text
"AnimGraphNode_Root_1": "(Result=(LinkID=-1,SourceLinkID=-1),Name=\"FlightLocomotionPose\",LayerGroup=\"ItemAnimLayers\",...)"
"AnimGraphNode_LinkedInputPose": "(Name=\"InputPose\",Graph=\"FlightLocomotionPose\",InputPose=(LinkID=-1,SourceLinkID=-1),bIsOutputLinked=False,...)"
```

Yani **graph fiziksel olarak BP'de var**, içinde 2 default node (UAnimGraphNode_Root + UAnimGraphNode_LinkedInputPose). Editor UI'da açılır + düzenlenebilir durumda.

**Şimdi node injection — fail:**

```json
{ "tool": "animation.add_sequence_player",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "graph_name": "FlightLocomotionPose",
    "sequence": "/FlightCore/SuperheroFlight/Characters/Mannequins/Animations/Flight/Idle/A_Flight_Idle_A.A_Flight_Idle_A",
    "loop": true,
    "x": -300, "y": 0
  } }
```

```
MCP error -32602: graph 'FlightLocomotionPose' not found
```

```json
{ "tool": "animation.add_animgraph_node",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion",
    "graph_name": "FlightLocomotionPose",
    "node_class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer",
    "x": -300, "y": 0
  } }
```

```
MCP error -32602: graph 'FlightLocomotionPose' not found on ABP_FlightCore_Locomotion
```

### Sonuç / hata

`graph_name="FlightLocomotionPose"` Sage'in node-injection tool'ları (`animation.add_sequence_player`, `animation.add_animgraph_node`, muhtemelen tüm Cluster A/D `graph_name` parametreli tool'lar) tarafından bulunamıyor. Hata "graph not found" — graph fiziksel olarak var (Cluster G ile spawn edildi, CDO + LinkedInputPose node + Root_1 node mevcut), Editor UI'da görünür.

Diğer tutarsızlıklar:
- `bp.list_functions` → `count: 2 (AnimGraph + EventGraph)` — FlightLocomotionPose function listesinde **gözükmüyor**.
- `animation.read_anim_blueprint` → `anim_graph_count: 1 (AnimGraph)` — FlightLocomotionPose yine **gözükmüyor**.
- `bp.full_dump.functions[]` → 2 entry (ubergraph: EventGraph, function: AnimGraph) — FlightLocomotionPose **listesinde yok** ama CDO property'lerinde Root_1 + LinkedInputPose Graph="FlightLocomotionPose" referans veriyor.
- `animation.list_layer_functions` (interface tarafı) → `FlightLocomotionPose` declared **görüyor** (interface BP, Cluster G).
- `animation.implement_anim_layer_interface` → `functions_already: ["FlightLocomotionPose"]` (child BP, Cluster G) — already-implemented mantığı çalışıyor.
- `animation.add_layer_function_override` (re-call) → `already: true, output_node_id: F19B...` — Cluster G internal state graph'ı tutuyor.

**Yani Cluster G (yeni interface override tool'ları) graph'ı doğru şekilde spawn ediyor + tracking yapıyor, ama Cluster A/D (eski generic anim node spawn tool'ları) bu graph'ları kendi lookup mantığında görmüyor.** Cross-cluster collection mismatch.

### Beklenen davranış

`animation.add_animgraph_node` ve `animation.add_sequence_player` (ve aynı `graph_name` parametresini kullanan tüm Cluster A/D tool'ları), Cluster G ile yaratılan AnimLayerInterface override graph'larını `graph_name` lookup'ında bulmalı. Override graph'lar normal function graph'lar gibi UBlueprint'in graph collection'larında yer alıyor; Cluster A/D `graph_name` resolver bu collection'ı taramamış olabilir.

Beklenen sonuç:

```json
{ "tool": "animation.add_sequence_player",
  "args": { "path": "...", "graph_name": "FlightLocomotionPose", "sequence": "..." } }
→ { "node_id": "<FGuid>", "class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "sequence": "...", "loop": true, "rate": 1.0 }
```

Ek tutarlılık:
- `bp.list_functions` ve `animation.read_anim_blueprint` interface override graph'larını da listelemeli (`kind: "anim_layer_function"` veya `interface_function: true` flag'iyle ayırarak). Şu an gizli kalıyor.
- `bp.full_dump.functions[]` interface override graph'larını da içermeli.

### Workaround

Yok. Editor UI üzerinden manuel düzenleme:
1. ABP_FlightCore_Locomotion'u editor'de aç → My Blueprint > Interfaces > ALI_FlightLocomotionLayer > FlightLocomotionPose üzerine çift tıkla (graph zaten oluşmuş, açılır).
2. Boş graph içine sağ tık → Anim Sequence Player → A_Flight_Idle_A seç.
3. SequencePlayer Output → Output Pose Result wire et.
4. Compile.

`bp.export_nodes_t3d` + `bp.import_nodes_t3d` (varsa) ile T3D inject edilebilir mi denenebilir ama bu graph_name lookup'a tabiyse muhtemelen aynı hata.

### Öneri (öncelik A/B/C)

**A — must (sprint critical)**
Sage'in `graph_name` resolver'ında, anim BP context'inde override fonksiyonları için ek collection taranması.

Pseudo-code:
```cpp
UEdGraph* ResolveAnimGraphByName(UBlueprint* BP, FName GraphName)
{
    // Mevcut: BP->FunctionGraphs içinde ara
    for (UEdGraph* G : BP->FunctionGraphs) { if (G->GetFName() == GraphName) return G; }
    // Mevcut: state machine sub-graphs içinde ara
    // ...
    
    // Cluster G fix: AnimLayerInterface override graphs içinde ara
    if (UAnimBlueprint* ABP = Cast<UAnimBlueprint>(BP))
    {
        // FBlueprintEditorUtils::GetAllGraphs(BP, AllGraphs) tüm graph türlerini kapsayabilir
        TArray<UEdGraph*> AllGraphs;
        FBlueprintEditorUtils::GetAllGraphs(BP, AllGraphs);
        for (UEdGraph* G : AllGraphs)
        {
            if (G->GetFName() == GraphName) return G;
        }
    }
    return nullptr;
}
```

`FBlueprintEditorUtils::GetAllGraphs` daha kapsayıcı — function graphs + macro graphs + ubergraph + anim graph + interface override graphs hepsini döner. Cluster A/D tool'larının graph_name resolver'ı bunu kullanmalı.

**B — should**
`bp.list_functions`, `animation.read_anim_blueprint`, `bp.full_dump.functions[]` interface override graph'larını da göstersin. Yeni `kind: "interface_override"` veya `kind: "anim_layer_function"` enum value ekle. Override graph'lar invisible kalırsa bu Gap gibi confusion yaratır.

**C — nice-to-have**
`animation.list_layer_function_overrides`:

```json
{ "tool": "animation.list_layer_function_overrides",
  "args": {
    "path": "/FlightCore/Animations/ABP_FlightCore_Locomotion.ABP_FlightCore_Locomotion"
  } }
→ {
  "implemented_interfaces": [{
    "interface_path": "...ALI_FlightLocomotionLayer.ALI_FlightLocomotionLayer_C",
    "override_functions": [
      {"function_name": "FlightLocomotionPose", "graph_name": "FlightLocomotionPose", "node_count": 2, "output_node_id": "F19B..."}
    ]
  }]
}
```

Bu `animation.list_implemented_layers`'ın daha detaylı çocuk-side variant'ı.

### Notlar — context

- Gap #24 verify oturumu sırasında yakalandı; Cluster G core fix doğru çalışıyor (graph spawn + tracking) ama Cluster A/D eski tool'ları yeni graph'ları görmüyor — **regression değil, eksik integration**.
- Editor UI'da override graph'lar normal görünüyor; sadece Sage tool'ları ile programatik node injection bloklu. **Sprint critical** çünkü locomotion state machine tüm node injection bu tool'lara bağlı.
- `animation.add_linked_anim_layer_node` (Cluster G) master AnimGraph'a node ekleyebildi (`A2AF5CFA...`) — bu tool kendi `graph_name="AnimGraph"` lookup'ında master'ın root AnimGraph'ı buluyor (root AnimGraph normal function graph collection'da). Dolayısıyla Cluster G tool'larının kendi graph lookup'ları farklı (kendi spawn ettikleri graph'ları takip ediyorlar) ama Cluster A/D'nin lookup'ı uyumsuz.

---

### Sage Claude triage (geliştirici doldurur)

- **Triage tarihi**: 2026-05-06
- **Kök sebep**: `ResolveAnimGraphTarget` (Cluster A/D'nin tüm `graph_name` parametreli tool'larını besleyen merkezi resolver, `SageAnimationTools.cpp:4186`) sadece `AnimBP->FunctionGraphs` + state machine sub-graph'ları + state bound-graph'ları arıyordu. Cluster G `animation.add_layer_function_override` override graph'ı `BP->ImplementedInterfaces[<i>].Graphs` (FBPInterfaceDescription'ın graph collection'ı) içine spawn ediyor — bu collection resolver'ın görüş alanında değildi. Aynı sebeple `bp.list_functions` (manuel `BP->FunctionGraphs/UbergraphPages/MacroGraphs` traversal'i, `SageBlueprintTools.cpp:471`) + `animation.read_anim_blueprint` (manuel `BP->FunctionGraphs` traversal'i, `SageAnimationTools.cpp:1595`) + `CollectAllGraphs` (4 root collection'ı dolaşan helper, `SageBlueprintTools.cpp:107` — `bp.list_graphs` + `bp.full_dump`'ı besler) interface override graph'larını gizliyordu. Cross-cluster collection mismatch.
- **Fix scope (4 Edit, 2 dosya, 1 atomic commit)**:
  1. **`ResolveAnimGraphTarget`** (Cluster A/D resolver) — yeni 4. fallback: `AnimBP->ImplementedInterfaces[<i>].Graphs` içinde FName eşleşme. Bu, `add_animgraph_node`, `add_sequence_player`, `add_state_machine_node`, `connect_pose_pin`, `set_anim_node_property`, `read_anim_node_properties`, `list_animgraph_nodes` ve `graph_name` parametresi alan TÜM Cluster A/D + C tool'larını otomatik unblocks.
  2. **`bp.list_functions`** (`BpListFunctionsImpl`) — `ImplementedInterfaces[<i>].Graphs` traversal'i `kind:"interface_override"` + `interface_function:true` + `interface` + `interface_path` field'larıyla eklendi. `bp.full_dump.functions[]` bu tool'un sonucunu kullandığı için otomatik kapsanıyor.
  3. **`animation.read_anim_blueprint`** (`ReadAnimBlueprintImpl`) — `ImplementedInterfaces[<i>].Graphs` her entry için anim/event schema split + `interface_function:true` flag. `anim_graph_count` artık total anim-schema-bound graph sayısı (FunctionGraphs anim'leri + override anim'leri); önceden `FunctionGraphs.Num()` döndürüyordu (yanlış, schema filter etmiyor + override gözükmüyor).
  4. **`CollectAllGraphs`** (`SageBlueprintTools.cpp` ortak helper) — `ImplementedInterfaces` traversal'i eklendi, `kind:"interface_override"`, `ParentName`=interface class adı. `WalkComposites` recurse override graph içinde de çalışıyor (override içine collapse'lenmiş composite'ler için). Bu fix `bp.list_graphs`, `FindFunctionGraph` (composite descend fallback'i), ve diğer `CollectAllGraphs` consumer'larını otomatik unblocks.
- **Mahmut'un B-should fix'i de dahil**: `kind:"interface_override"` enum value, `interface_function:true` flag — Lyra Claude'un confusion riski kapanıyor.
- **C nice-to-have (`animation.list_layer_function_overrides`)**: implement edilmedi — `animation.list_implemented_layers` (Cluster G, mevcut tool) zaten aynı bilgiyi veriyor: per-interface entries + per-function `override_graph` + `node_count`. Yeni tool eklemek redundant.
- **Fix commit**: pending (working dir: `SageAnimationTools.cpp`, `SageBlueprintTools.cpp`)
- **Deploy adımı**: ⏳ Plugin UAT BuildPlugin Win64 in progress; Lyra editor açık (Cluster G yüklü) — Live Coding `compile_and_reload` muhtemel, gerekirse close+install+open; HeroFlight kapalı, doğrudan install.
- **Verify durumu**: pending — Lyra Claude verify edecek. Önerilen smoke senaryo (Lyra'da `ABP_FlightCore_Locomotion` üzerinde):
  1. `bp.list_functions path=ABP_FlightCore_Locomotion` → `FlightLocomotionPose` entry görünmeli (`kind:"interface_override", interface_function:true, interface:"ALI_FlightLocomotionLayer_C"`).
  2. `animation.read_anim_blueprint path=ABP_FlightCore_Locomotion` → `anim_graphs[]` içinde `FlightLocomotionPose` görünmeli (`interface_function:true`); `anim_graph_count` 2 olmalı (AnimGraph + FlightLocomotionPose).
  3. `bp.list_graphs path=ABP_FlightCore_Locomotion` → `FlightLocomotionPose` entry (`kind:"interface_override", parent:"ALI_FlightLocomotionLayer_C"`).
  4. **Sprint critical**: `animation.add_sequence_player path=ABP_FlightCore_Locomotion graph_name="FlightLocomotionPose" sequence=A_Flight_Idle_A loop=true` → `{node_id, class, sequence, loop, rate}` (önceden `-32602: graph not found`).
  5. `animation.connect_pose_pin path=ABP_FlightCore_Locomotion graph_name="FlightLocomotionPose" from_node=<seq_id> from_pin="Pose" to_node=<root_id> to_pin="Result"` → connected (Output Pose'a bağla).
  6. `bp.full_dump path=ABP_FlightCore_Locomotion include_function_graphs=true` → `functions[]` içinde `FlightLocomotionPose` 3 node ile.

---

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

- **Status**: FIXED (source patched; running server not restarted)
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
#### Sage implementation update - 2026-05-06 Codex

- Windows script selection fixed in `restart_orchestrator.cpp`: `restart_editor` now chooses `scripts/build-plugin.ps1` on Win32 and `scripts/build-plugin.sh` on Mac/Linux.
- Accidental `SAGE_REPO_ROOT=...\sage-unreal-mcp\scripts` is normalized back to repo root, so `scripts\scripts\build-plugin.*` is not constructed.
- UBT project rebuild command now quotes `Build.bat` and `-Project=...` paths correctly on Windows.
- Follow-up hardening added after Lyra deploy smoke: Step 4 now deploys the full packaged plugin payload into the project plugin (`SageBridge.uplugin`, every `Binaries/<platform>` file including PDB/modules, and `Source`) instead of only swapping DLL/modules.
- Safety guard: recursive `Source` replacement is constrained to `<Project>/Plugins/SageBridge/Source`.
- Verification done without touching the running server: `restart_orchestrator.cpp.obj` compiles via Ninja object target, and `git diff --check` is clean. The currently running `sage-server.exe` must be restarted later to pick up this source change.
