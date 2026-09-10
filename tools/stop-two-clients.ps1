[CmdletBinding()]
param(
    [switch] $KeepCoordinator
)

# Stops everything tools/smoke-two-clients.ps1 leaves running: both sidecars,
# both staged king.exe copies, any runtime observers and (by default) the
# coordinator. Only the p1/p2 runtime copies are touched; a Steam-launched or
# GOG king.exe is never terminated.

$ErrorActionPreference = 'Continue'

function Stop-Named {
    param([string] $Name, [scriptblock] $Filter = { $true })
    foreach ($process in @(Get-Process -Name $Name -ErrorAction SilentlyContinue)) {
        try {
            if (& $Filter $process) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                Write-Host ("stopped {0} PID={1}" -f $Name, $process.Id)
            }
        } catch {
            # The process may exit while it is being inspected.
        }
    }
}

Stop-Named -Name 'ht2mp-runtime-observer'
Stop-Named -Name 'ht2mp-client'
Stop-Named -Name 'king' -Filter {
    param($process)
    $path = $null
    try { $path = $process.Path } catch { return $false }
    return ($null -ne $path) -and (
        $path.EndsWith('\HT2MP\runtime\steam-8138acee--p1\king.exe',
            [StringComparison]::OrdinalIgnoreCase) -or
        $path.EndsWith('\HT2MP\runtime\steam-8138acee--p2\king.exe',
            [StringComparison]::OrdinalIgnoreCase))
}
if (-not $KeepCoordinator) {
    Stop-Named -Name 'ht2mp-coordinator'
}
