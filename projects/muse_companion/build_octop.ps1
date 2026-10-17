# build_octop.ps1 -- firmware build for projects/muse_companion (Octop backend).
# Keep this file pure ASCII: Windows PowerShell 5.1 mis-decodes BOM-less UTF-8
# and the parser then fails on the non-ASCII bytes.
#
# Environment rules (from .agents/skills):
#   1. IDF_PATH and IDF_TOOLS_PATH must stay a registered pair; switching pairs
#      requires a fullclean:
#        D:\esp-tools\esp-idf <-> D:\esp-tools\espressif   (py3.12)
#        D:\esp-mo\esp-idf    <-> %USERPROFILE%\.espressif (py3.13)
#   2. When launched from Git Bash, MSYS2 re-injects MSYSTEM into children and
#      idf_tools.py fatally rejects it. Remove it inside this process.
#   3. The default component-manager cache under %LOCALAPPDATA% cannot be
#      resolved by git-for-windows, so pin it to a plain NTFS path on D:.
#   4. ninja's .ninja_deps records /D:/ style paths for GSP generated files and
#      then errors on incremental builds; delete that single file each run.
#   5. This machine's idf.py always configures with -DCCACHE_ENABLE=True, even
#      with IDF_CCACHE_ENABLE unset, so compiler invocations are ccache-wrapped.
#      That shortens rebuilds only; the link step and therefore every reported
#      image size are real. For an honest timing baseline, set CCACHE_DISABLE=1.
param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# Resolve the workspace from this script's own location, so any clone builds
# without editing absolute paths. The script lives at <workspace>/projects/muse_companion.
$projectDir = $PSScriptRoot
$workspace  = (Get-Item (Join-Path $projectDir "..\..")).FullName
$project    = "projects/muse_companion"
$build      = Join-Path $projectDir "build"

# ESP-IDF is a machine-level toolchain, not part of this repo. IDF_PATH and
# IDF_TOOLS_PATH must stay a registered pair (see rule 1 above). Export your own
# pair before running; the defaults below are the original author's Windows
# layout and are only used when the environment does not already provide them.
$idf      = if ($env:IDF_PATH)       { $env:IDF_PATH }        else { "D:\esp-tools\esp-idf" }
$idfTools = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH }  else { "D:\esp-tools\espressif" }
$runner   = Join-Path $workspace "submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if (Test-Path Env:MSYSTEM) { Remove-Item Env:MSYSTEM -Force }
# Keep the component-manager cache on a plain NTFS path (rule 3); default to a
# repo-local directory that git ignores, overridable via the environment.
if (-not $env:IDF_COMPONENT_CACHE_PATH) {
    $env:IDF_COMPONENT_CACHE_PATH = Join-Path $workspace ".component-manager-cache"
}
$env:IDF_PY_BUILD_JOBS = "4"

if (Test-Path "$build\.ninja_deps") {
    Write-Host "[build] removing stale .ninja_deps"
    Remove-Item "$build\.ninja_deps" -Force
}

$arguments = @("--project", $project, "build")
if ($Clean) { $arguments += "--fullclean" }

Write-Host "[build] building $project ..."
Push-Location $workspace
try {
    $env:IDF_PATH = $idf
    $env:IDF_TOOLS_PATH = $idfTools
    python $runner @arguments
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK"
