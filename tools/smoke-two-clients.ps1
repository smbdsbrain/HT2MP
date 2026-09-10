[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SteamSource,

    [Parameter(Mandatory = $true)]
    [string] $DriverSave,

    [string] $Token = '',
    [string] $Server = '127.0.0.1:28020',
    [string] $Listen = '0.0.0.0:28020',
    [string] $BuildRoot = (Join-Path $PSScriptRoot '..\build\windows-x86'),
    [int] $StartupTimeoutSeconds = 120,
    [ValidateSet('application', 'windowed', 'fullscreen', 'desktop')]
    [string] $Presentation = 'windowed',
    # dxcfg `display=`: `desktop` renders at the desktop mode (one window
    # covers the whole screen), `application` keeps the game's own TRUCK.INI
    # resolution so two windows can sit side by side.
    [ValidateSet('application', 'desktop')]
    [string] $Display = 'desktop',
    [switch] $SkipPrepare,
    [switch] $PrepareOnly,
    # Place both game windows side by side without overlap once both clients
    # are live. A fully covered window is throttled by the desktop compositor,
    # which starves its game loop and therefore its telemetry.
    [switch] $Arrange,
    # Start the read-only x86 runtime observer on both king.exe processes.
    [switch] $Observer,
    [int] $ObserverSeconds = 600,
    [int] $ObserverHz = 40,
    # Sidecar per-instance network trace CSVs (`--trace-dir`), off by default.
    [string] $TraceDir = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Token)) {
    $Token = [guid]::NewGuid().ToString('N')
}
if ($Token -notmatch '^[0-9a-fA-F]{32}$') {
    throw 'Token must contain exactly 32 hexadecimal digits.'
}
if (-not (Test-Path -LiteralPath $SteamSource -PathType Container)) {
    throw "Steam source directory does not exist: $SteamSource"
}
if (-not (Test-Path -LiteralPath $DriverSave -PathType Leaf)) {
    throw "Driver save does not exist: $DriverSave"
}

$resolvedBuild = [IO.Path]::GetFullPath($BuildRoot)
$client = Join-Path $resolvedBuild 'bin\Release\ht2mp-client.exe'
$coordinator = Join-Path $resolvedBuild 'apps\coordinator\Release\ht2mp-coordinator.exe'
$injector = Join-Path $resolvedBuild 'bin\Release\ht2mp-injector32.exe'
$bridge = Join-Path $resolvedBuild 'bin\Release\ht2mp-bridge32.dll'
$observerExe = Join-Path $resolvedBuild 'tools\runtime_observer\Release\ht2mp-runtime-observer.exe'
if ($Observer -and -not (Test-Path -LiteralPath $observerExe -PathType Leaf)) {
    throw "Runtime observer is missing: $observerExe"
}
foreach ($required in @($client, $coordinator, $injector, $bridge)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build artifact is missing: $required"
    }
}

$localAppData = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData)
$runtimeRoot = Join-Path $localAppData 'HT2MP\runtime'
$p1Runtime = Join-Path $runtimeRoot 'steam-8138acee--p1'
$p2Runtime = Join-Path $runtimeRoot 'steam-8138acee--p2'

function Invoke-CheckedClient {
    param([string[]] $Arguments)
    & $client @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ht2mp-client failed with exit code $LASTEXITCODE"
    }
}

if (-not $SkipPrepare) {
    foreach ($instance in @('p1', 'p2')) {
        Invoke-CheckedClient @(
            'stage', '--edition', 'steam', '--source', $SteamSource,
            '--instance', $instance)
        Invoke-CheckedClient @(
            'seed-driver', '--edition', 'steam', '--save', $DriverSave,
            '--instance', $instance)
    }
}

# Run both copies through the bundled DirectX 1-7 wrapper in explicit windowed
# presentation, backed by the current desktop display mode.
foreach ($runtime in @($p1Runtime, $p2Runtime)) {
    $graphicsConfig = Join-Path $runtime 'dxcfg.ini'
    $text = [IO.File]::ReadAllText($graphicsConfig)
    if ($text -match '(?im)^display\s*=') {
        $text = [Text.RegularExpressions.Regex]::Replace(
            $text, '(?im)^display\s*=\s*[^\r\n]+$', "display=$Display")
    } else {
        $text = [Text.RegularExpressions.Regex]::Replace(
            $text, '(?im)^\[dxcfg\]\s*$', "[dxcfg]`r`ndisplay=$Display")
    }
    $updated = [Text.RegularExpressions.Regex]::Replace(
        $text, '(?im)^presentation\s*=\s*\w+\s*$', "presentation=$Presentation")
    if ($updated -eq $text -and
        $text -notmatch "(?im)^presentation\s*=\s*$Presentation\s*$") {
        throw "Could not select $Presentation presentation in $graphicsConfig"
    }
    [IO.File]::WriteAllText(
        $graphicsConfig, $updated, [Text.ASCIIEncoding]::new())
}

if ($PrepareOnly) {
    Write-Host "Prepared isolated runtimes:`n  $p1Runtime`n  $p2Runtime"
    return
}

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$coordinatorLog = Join-Path $runtimeRoot "smoke-coordinator-$runStamp.log"
$coordinatorErrorLog = Join-Path $runtimeRoot "smoke-coordinator-$runStamp.err.log"
$coordinatorProcess = $null
$startedClients = [Collections.Generic.List[object]]::new()

try {
$coordinatorProcess = Start-Process -FilePath $coordinator `
    -ArgumentList @('--profile', 'steam-8138acee', '--listen', $Listen,
                    '--token', $Token) `
    -RedirectStandardOutput $coordinatorLog `
    -RedirectStandardError $coordinatorErrorLog `
    -WindowStyle Hidden -PassThru

Start-Sleep -Milliseconds 500
if ($coordinatorProcess.HasExited) {
    throw "Coordinator exited during startup. See $coordinatorErrorLog"
}

function Start-SmokeClient {
    param(
        [string] $Instance,
        [string] $PlayerName,
        [string] $Runtime
    )
    $logDirectory = Join-Path $Runtime 'logs'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $stdout = Join-Path $logDirectory "sidecar-$runStamp.log"
    $stderr = Join-Path $logDirectory "sidecar-$runStamp.err.log"
    $arguments = @(
        'run', '--edition', 'steam', '--instance', $Instance,
        '--online-mode', '--server', $Server, '--token', $Token,
        '--name', $PlayerName, '--auto-enter-world',
        '--injector', $injector, '--bridge', $bridge)
    if ($TraceDir -ne '') {
        $instanceTrace = Join-Path $TraceDir "$Instance-$runStamp"
        New-Item -ItemType Directory -Path $instanceTrace -Force | Out-Null
        $arguments += @('--trace-dir', $instanceTrace)
    }
    $process = Start-Process -FilePath $client `
        -ArgumentList $arguments `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
        -WindowStyle Hidden -PassThru
    return [pscustomobject]@{
        Instance = $Instance
        PlayerName = $PlayerName
        Runtime = $Runtime
        KingPath = Join-Path $Runtime 'king.exe'
        KingPathSuffix = "\HT2MP\runtime\steam-8138acee--$Instance\king.exe"
        Sidecar = $process
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Wait-ForOnlineTick {
    param([pscustomobject] $Entry)
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        if ($Entry.Sidecar.HasExited) {
            throw "Sidecar $($Entry.Instance) exited. See $($Entry.Stderr)"
        }
        if (Test-Path -LiteralPath $Entry.Stderr -PathType Leaf) {
            $stream = [IO.FileStream]::new(
                $Entry.Stderr, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite)
            try {
                $reader = [IO.StreamReader]::new($stream)
                try { $logText = $reader.ReadToEnd() }
                finally { $reader.Dispose() }
            } finally {
                $stream.Dispose()
            }
            if ($logText -match 'exact-profile hook: calls=[1-9][0-9]*') {
                return
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Sidecar $($Entry.Instance) did not reach a live AI tick within $StartupTimeoutSeconds seconds. See $($Entry.Stderr)"
}

$firstClient = Start-SmokeClient -Instance 'p1' `
    -PlayerName 'PLAYER_A' -Runtime $p1Runtime
$startedClients.Add($firstClient)
Wait-ForOnlineTick $firstClient
$secondClient = Start-SmokeClient -Instance 'p2' `
    -PlayerName 'PLAYER_B' -Runtime $p2Runtime
$startedClients.Add($secondClient)
$clients = @($firstClient, $secondClient)
Wait-ForOnlineTick $secondClient

$deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
$gameProcesses = @{}
do {
    foreach ($entry in $clients) {
        if ($entry.Sidecar.HasExited) {
            throw "Sidecar $($entry.Instance) exited. See $($entry.Stderr)"
        }
    }
    foreach ($process in @(Get-Process -Name king -ErrorAction SilentlyContinue)) {
        try {
            $path = $process.Path
            foreach ($entry in $clients) {
                if ([string]::Equals($path, $entry.KingPath,
                        [StringComparison]::OrdinalIgnoreCase) -or
                    $path.EndsWith($entry.KingPathSuffix,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    $gameProcesses[$entry.Instance] = $process
                }
            }
        } catch {
            # A process can exit between enumeration and Path access.
        }
    }
    if ($gameProcesses.Count -eq 2) { break }
    Start-Sleep -Milliseconds 250
} while ([DateTime]::UtcNow -lt $deadline)

if ($gameProcesses.Count -ne 2) {
    throw "Both direct Steam runtime copies did not remain alive within $StartupTimeoutSeconds seconds. Check the per-instance logs."
}

Write-Host 'Two-client Steam smoke is running.'
Write-Host "Coordinator PID: $($coordinatorProcess.Id)"
foreach ($entry in $clients) {
    Write-Host ("{0}: sidecar PID={1}, king PID={2}, runtime={3}, log={4}" -f `
        $entry.PlayerName, $entry.Sidecar.Id,
        $gameProcesses[$entry.Instance].Id, $entry.Runtime, $entry.Stdout)
}

if ($Arrange) {
    # Move without activating or resizing; focus stays where the launcher left
    # it, so this is safe after both clients reported a live AI tick.
    Add-Type -Namespace Ht2mp -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
public static System.Collections.Generic.List<IntPtr> FindVisible(uint pid) {
    var list = new System.Collections.Generic.List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
}
'@
    $screenWidth = [Ht2mp.Win]::GetSystemMetrics(0)
    $screenHeight = [Ht2mp.Win]::GetSystemMetrics(1)
    $flags = [uint32](0x0001 -bor 0x0010)  # NOSIZE | NOACTIVATE; HWND_TOP raises the window without focusing it
    $cursorX = 0
    foreach ($entry in $clients) {
        $gamePid = [uint32]$gameProcesses[$entry.Instance].Id
        $windows = [Ht2mp.Win]::FindVisible($gamePid)
        if ($windows.Count -eq 0) {
            Write-Warning "No visible window for $($entry.PlayerName) (PID $gamePid)"
            continue
        }
        $hwnd = $windows[0]
        $rect = New-Object Ht2mp.Win+RECT
        [void][Ht2mp.Win]::GetWindowRect($hwnd, [ref]$rect)
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        $x = $cursorX
        $y = 0
        if ($x + $width -gt $screenWidth) {
            # Not enough horizontal room: fall back to the opposite corner so the
            # overlap is as small as the desktop allows.
            $x = [Math]::Max(0, $screenWidth - $width)
            $y = [Math]::Max(0, $screenHeight - $height)
        }
        [void][Ht2mp.Win]::SetWindowPos($hwnd, [IntPtr]::Zero, $x, $y, 0, 0, $flags)
        Write-Host ("{0}: window {1}x{2} moved to ({3},{4})" -f `
            $entry.PlayerName, $width, $height, $x, $y)
        $cursorX = $x + $width
    }
}

if ($Observer) {
    foreach ($entry in $clients) {
        $logDirectory = Join-Path $entry.Runtime 'logs'
        $csv = Join-Path $logDirectory "observer-$runStamp.csv"
        $observerOut = Join-Path $logDirectory "observer-$runStamp.log"
        $observerErr = Join-Path $logDirectory "observer-$runStamp.err.log"
        $observerArgs = @(
            '--pid', $gameProcesses[$entry.Instance].Id,
            '--profile', 'steam-8138acee',
            '--duration', $ObserverSeconds, '--hz', $ObserverHz, '--csv', $csv)
        # The bridge prints the remote actor's VehicleInstance as vi=<hex>;
        # sample it too when it is already known.
        $remoteMatch = $null
        if (Test-Path -LiteralPath $entry.Stderr) {
            $remoteMatch = Select-String -LiteralPath $entry.Stderr `
                -Pattern 'vi=([0-9a-fA-F]{8})' | Select-Object -Last 1
        }
        if ($null -ne $remoteMatch -and $remoteMatch.Matches[0].Groups[1].Value -ne '00000000') {
            $observerArgs += @('--vehicle', $remoteMatch.Matches[0].Groups[1].Value)
        }
        $observerProcess = Start-Process -FilePath $observerExe `
            -ArgumentList $observerArgs `
            -RedirectStandardOutput $observerOut -RedirectStandardError $observerErr `
            -WindowStyle Hidden -PassThru
        Write-Host ("{0}: observer PID={1}, csv={2}" -f `
            $entry.PlayerName, $observerProcess.Id, $csv)
    }
}
Write-Host 'The script deliberately leaves coordinator, sidecars and both visible game windows running.'
} catch {
    foreach ($entry in $startedClients) {
        if ($null -ne $entry.Sidecar -and -not $entry.Sidecar.HasExited) {
            Stop-Process -Id $entry.Sidecar.Id -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($process in @(Get-Process -Name king -ErrorAction SilentlyContinue)) {
        try {
            $path = $process.Path
            if ($path.EndsWith(
                    '\HT2MP\runtime\steam-8138acee--p1\king.exe',
                    [StringComparison]::OrdinalIgnoreCase) -or
                $path.EndsWith(
                    '\HT2MP\runtime\steam-8138acee--p2\king.exe',
                    [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {
            # The game may finish while startup cleanup enumerates processes.
        }
    }
    if ($null -ne $coordinatorProcess -and -not $coordinatorProcess.HasExited) {
        Stop-Process -Id $coordinatorProcess.Id -Force -ErrorAction SilentlyContinue
    }
    throw
}
