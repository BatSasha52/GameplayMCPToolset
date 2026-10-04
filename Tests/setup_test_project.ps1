# Creates the throwaway test project used by gameplay_smoke_test.py.
#
#   powershell -File Tests/setup_test_project.ps1 [-Destination <dir>] [-AnimMCPToolset <path to AnimMCPToolset repo>]
#
# Any existing copy at the destination is updated in place (Saved/Content are kept).
# The project gets this plugin (and AnimMCPToolset, used only to build a fixture Animation Blueprint
# with a state machine) as directory junctions under Plugins/, so it always builds the live sources.
param(
	[string]$Destination = "$env:TEMP\GMCPTest",
	[string]$AnimMCPToolset = ""
)
$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path -Parent $PSScriptRoot

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item -Recurse -Force "$PSScriptRoot\TestProject\*" $Destination
# Rules files are stored as *.cs.in: UnrealBuildTool scans every *.cs under a plugin folder, so real
# Target.cs/Build.cs files inside this repo would break any project that uses the plugin.
Get-ChildItem -Recurse -Path "$Destination\Source" -Filter "*.cs.in" | ForEach-Object {
	Move-Item -Force $_.FullName ($_.FullName.Substring(0, $_.FullName.Length - 3))
}
New-Item -ItemType Directory -Force -Path "$Destination\Plugins" | Out-Null

if (-not (Test-Path "$Destination\Plugins\GameplayMCPToolset")) {
	New-Item -ItemType Junction -Path "$Destination\Plugins\GameplayMCPToolset" -Target $PluginRoot | Out-Null
}
if ($AnimMCPToolset -eq "") { $AnimMCPToolset = Join-Path (Split-Path -Parent $PluginRoot) "AnimMCPToolset" }
if ((Test-Path $AnimMCPToolset) -and -not (Test-Path "$Destination\Plugins\AnimMCPToolset")) {
	New-Item -ItemType Junction -Path "$Destination\Plugins\AnimMCPToolset" -Target $AnimMCPToolset | Out-Null
}
if (-not (Test-Path "$Destination\Plugins\AnimMCPToolset")) {
	# The fixture Animation Blueprint is optional; drop the plugin reference so the project still opens.
	$uproject = Get-Content "$Destination\GMCPTest.uproject" -Raw
	$uproject = $uproject -replace '\s*\{ "Name": "AnimMCPToolset", "Enabled": true \},', ''
	Set-Content -Encoding utf8 "$Destination\GMCPTest.uproject" $uproject
	Write-Host "AnimMCPToolset not found: pie_get_anim_state will be tested without a state machine fixture."
}
Write-Host "Test project ready at $Destination"
