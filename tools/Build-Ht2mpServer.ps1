param(
    [ValidateSet("windows-x64", "linux-x64")]
    [string]$Platform = "windows-x64",

    [string]$WslDistribution = "Ubuntu-24.04",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $repoRoot "dist\server"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

function Get-PackageVersion([string] $CachePath) {
    $entry = Get-Content -LiteralPath $CachePath |
        Where-Object { $_ -like 'HT2MP_PACKAGE_VERSION:STRING=*' } |
        Select-Object -First 1
    if (-not $entry) {
        throw "HT2MP_PACKAGE_VERSION is missing from $CachePath"
    }
    return $entry.Substring($entry.IndexOf('=') + 1)
}

if ($Platform -eq "windows-x64") {
    & cmake --preset server-windows-x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake --build --preset build-server-windows-x64 --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not $SkipTests) {
        & ctest --preset test-server-windows-x64
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & cmake --build --preset package-server-windows-x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $cache = Join-Path $repoRoot "build\server-windows-x64\CMakeCache.txt"
    $version = Get-PackageVersion $cache
    $artifact = Join-Path $distDir "HT2MP-server-$version-windows-x86_64.zip"
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Expected package was not produced: $artifact"
    }
    Write-Output $artifact
    exit 0
}

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    throw "wsl.exe is required to build the Linux package from Windows"
}
$resolvedRepo = [System.IO.Path]::GetFullPath($repoRoot)
if ($resolvedRepo.Length -lt 3 -or $resolvedRepo[1] -ne ':') {
    throw "The repository must be on a Windows drive for WSL path translation"
}
$drive = [char]::ToLowerInvariant($resolvedRepo[0])
$linuxRepo = "/mnt/$drive/" + $resolvedRepo.Substring(3).Replace('\', '/')
$linuxScript = "$linuxRepo/tools/build-server.sh"
if ($SkipTests) {
    & wsl.exe -d $WslDistribution -- bash $linuxScript --skip-tests
} else {
    & wsl.exe -d $WslDistribution -- bash $linuxScript
}
exit $LASTEXITCODE
