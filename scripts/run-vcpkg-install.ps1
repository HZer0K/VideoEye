param(
    [ValidateSet("release", "debug")]
    [string]$Config = "release"
)

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Triplet = "x64-windows-$Config"

# Run vcpkg install with full MSVC environment loaded (vcvars64).
$logFile = Join-Path $ProjectRoot "vcpkg-install.log"

# Remove problematic env vars
Remove-Item Env:HTTP_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
Remove-Item Env:http_proxy -ErrorAction SilentlyContinue
Remove-Item Env:https_proxy -ErrorAction SilentlyContinue

# Load MSVC environment via vcvars64.bat (same pattern as build_ninja.ps1)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) { Write-Error "vswhere: no VS with C++ toolchain"; exit 1 }

$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { Write-Error "vcvars64.bat not found: $vcvars"; exit 1 }

cmd /c "`"$vcvars`" x64 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2].TrimEnd()
    }
}

Write-Output "MSVC environment loaded. MSBuild check:"
$msbuild = Get-Command msbuild -ErrorAction SilentlyContinue
if ($msbuild) { Write-Output "  msbuild: $($msbuild.Source)" } else { Write-Output "  msbuild: NOT FOUND" }

if (-not $env:VCPKG_ROOT) { Write-Error "VCPKG_ROOT is not set"; exit 1 }

Set-Location $ProjectRoot
Write-Output "Running vcpkg install (triplet: $Triplet)..."

& "$env:VCPKG_ROOT\vcpkg.exe" install `
    --triplet $Triplet `
    --host-triplet $Triplet `
    --overlay-triplets=scripts/triplets `
    --overlay-ports=scripts/overlay-ports `
    --x-manifest-root=. `
    --x-install-root=vcpkg_installed 2>&1 | Tee-Object -FilePath $logFile

Write-Output "vcpkg exit code: $LASTEXITCODE"
exit $LASTEXITCODE
