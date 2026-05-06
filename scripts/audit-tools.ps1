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

$serverRoot = Join-Path $RepoRoot 'server/src'
$pluginRoot = Join-Path $RepoRoot 'plugin/Source/SageBridge/Private'

$serverNames = Get-ChildItem $serverRoot -Recurse -Filter '*.cpp' |
    ForEach-Object { Get-RegexGroupValues (Read-AllText $_.FullName) '\.name\s*=\s*"([^"]+)"' } |
    Where-Object { $_ -ne 'sage-unreal-mcp' } |
    Sort-Object -Unique

$pluginNames = Get-ChildItem $pluginRoot -Recurse -Filter '*.cpp' |
    ForEach-Object { Get-RegexGroupValues (Read-AllText $_.FullName) 'RegisterHandler\(TEXT\("([^"]+)"\)' } |
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

$schemaWithoutPlugin = Compare-Object $serverNames $pluginNames |
    Where-Object {
        $_.SideIndicator -eq '<=' -and
        $serverOnlyExact -notcontains $_.InputObject -and
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
