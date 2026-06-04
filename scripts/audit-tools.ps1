param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'

function Get-RegexGroupValues {
    param(
        [string]$Text,
        [string]$Pattern
    )
    [regex]::Matches($Text, $Pattern) | ForEach-Object { $_.Groups[1].Value }
}

function Read-AllText {
    param([string]$Path)
    Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
}

function Get-ArrayRegisteredToolNames {
    param(
        [string]$Text,
        [string]$ArrayPattern,
        [string]$NamePattern
    )
    [regex]::Matches($Text, $ArrayPattern) | ForEach-Object {
        $body = $_.Groups[1].Value
        Get-RegexGroupValues $body $NamePattern
    }
}

$serverRoot = Join-Path $RepoRoot 'server/src'
$pluginRoot = Join-Path $RepoRoot 'plugin/Source/SageBridge/Private'

$serverNames = Get-ChildItem $serverRoot -Recurse -Filter '*.cpp' |
    ForEach-Object {
        $text = Read-AllText $_.FullName
        Get-RegexGroupValues $text '\.name\s*=\s*"([^"]+)"'
        Get-RegexGroupValues $text 'regLocal\(\s*registry\s*,\s*"([^"]+)"'
        Get-ArrayRegisteredToolNames $text 'const\s+char\*\s+\w*Tools\[\]\s*=\s*\{([\s\S]*?)\};' '"([^"]+)"'
    } |
    Where-Object { $_ -ne 'sage-unreal-mcp' } |
    Sort-Object -Unique

$pluginNames = Get-ChildItem $pluginRoot -Recurse -Filter '*.cpp' |
    ForEach-Object {
        $text = Read-AllText $_.FullName
        Get-RegexGroupValues $text 'RegisterHandler\(TEXT\("([^"]+)"\)'
        Get-ArrayRegisteredToolNames $text 'static\s+const\s+TCHAR\*\s+\w*Tools\[\]\s*=\s*\{([\s\S]*?)\};' 'TEXT\("([^"]+)"\)'
    } |
    Sort-Object -Unique

$serverOnlyExact = @(
    'ping',
    'restart_editor',
    'wait_for_editor',
    'list_editors',
    'get_active_editor',
    'set_active_editor',
    'class_hierarchy',
    'query_graph',
    'impact_of',
    'references_to',
    'find_unused',
    'index_slot',
    'index_status'
)

$serverOnlyPrefixes = @(
    'source.',
    'decision.',
    'risk.',
    'cppreflect.',
    'network.',
    'pipeline.'
)

$sourceIntelligenceServerOnlyExact = @(
    'search_unreal_api',
    'get_by_fqn',
    'get_class_members',
    'get_class_reference',
    'get_function_signature',
    'get_include_path',
    'search_deprecated',
    'get_deprecation_warnings',
    'lookup_docs',
    'lookup_class',
    'find_callers',
    'find_callees',
    'reflect.rebuild_reflection_index',
    'set_unreal_engine_path',
    'get_unreal_engine_path',
    'set_unreal_project_path',
    'get_unreal_project_path',
    'status'
)

function Test-ServerOnlyTool {
    param([string]$Name)
    if ($serverOnlyExact -contains $Name) { return $true }
    if ($sourceIntelligenceServerOnlyExact -contains $Name) { return $true }
    foreach ($prefix in $serverOnlyPrefixes) {
        if ($Name.StartsWith($prefix)) { return $true }
    }
    return $false
}

$schemaWithoutPlugin = Compare-Object $serverNames $pluginNames |
    Where-Object {
        $_.SideIndicator -eq '<=' -and
        -not (Test-ServerOnlyTool $_.InputObject) -and
        $_.InputObject -notmatch '^index_'
    } |
    ForEach-Object { $_.InputObject }

$pluginWithoutSchema = Compare-Object $serverNames $pluginNames |
    Where-Object { $_.SideIndicator -eq '=>' } |
    ForEach-Object { $_.InputObject }

$phase4Path = Join-Path $RepoRoot 'server/src/tools/phase4_schemas.cpp'
$phase4Text = Read-AllText $phase4Path
$schemaStubMatches = [regex]::Matches(
    $phase4Text,
    'Tool\{\.name="([^"]+)"(?:(?!Tool\{\.name=)[\s\S])*?\.description="([^"]*(?:NOT IMPLEMENTED|STUB|DEPRECATED|pending)[^"]*)"'
) | ForEach-Object {
    [pscustomobject]@{
        tool = $_.Groups[1].Value
        description = $_.Groups[2].Value
    }
}

$pluginStubMatches = Get-ChildItem (Join-Path $pluginRoot 'Tools') -Filter '*.cpp' |
    ForEach-Object {
        $path = $_.FullName
        Select-String -LiteralPath $path -Pattern '\[NOT IMPLEMENTED\]\s*([A-Za-z0-9_.]+)' |
            ForEach-Object {
                [pscustomobject]@{
                    tool = $_.Matches[0].Groups[1].Value
                    file = Split-Path $path -Leaf
                    line = $_.LineNumber
                }
            }
    }

$noteReturns = Get-ChildItem (Join-Path $pluginRoot 'Tools') -Filter '*.cpp' |
    ForEach-Object {
        $path = $_.FullName
        Select-String -LiteralPath $path -Pattern 'SetStringField\(TEXT\("note"\)' |
            ForEach-Object {
                [pscustomobject]@{
                    file = Split-Path $path -Leaf
                    line = $_.LineNumber
                    text = $_.Line.Trim()
                }
            }
    }

$result = [pscustomobject]@{
    repo_root = (Resolve-Path $RepoRoot).Path
    server_tool_count = $serverNames.Count
    plugin_handler_count = $pluginNames.Count
    schema_without_plugin = @($schemaWithoutPlugin)
    plugin_without_schema = @($pluginWithoutSchema)
    schema_stub_count = @($schemaStubMatches).Count
    schema_stubs = @($schemaStubMatches)
    plugin_not_implemented_count = @($pluginStubMatches).Count
    plugin_not_implemented = @($pluginStubMatches)
    note_return_count = @($noteReturns).Count
    note_returns = @($noteReturns)
}

if ($Json) {
    $result | ConvertTo-Json -Depth 6
    exit 0
}

"Sage tool audit"
"repo_root: $($result.repo_root)"
"server_tool_count: $($result.server_tool_count)"
"plugin_handler_count: $($result.plugin_handler_count)"
"schema_without_plugin: $($result.schema_without_plugin.Count)"
$result.schema_without_plugin | ForEach-Object { "  - $_" }
"plugin_without_schema: $($result.plugin_without_schema.Count)"
$result.plugin_without_schema | ForEach-Object { "  - $_" }
"schema_stub_count: $($result.schema_stub_count)"
"plugin_not_implemented_count: $($result.plugin_not_implemented_count)"
"note_return_count: $($result.note_return_count)"
