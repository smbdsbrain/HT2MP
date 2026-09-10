[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $repoRoot
try {
    $tracked = @(git ls-files)
    if ($LASTEXITCODE -ne 0) {
        throw 'git ls-files failed'
    }

    $allowedProfile = 'apps/launcher/assets/HT2MP.pl1'
    $forbiddenPath = [regex]'(?i)(^|/)(build[^/]*|dist|out|runtime|screenshots|private|re|listofgames)/'
    $forbiddenExtension = [regex]'(?i)\.(exe|dll|pl1|pdb|obj|lib|a|o|log|dmp|sav|save|ini|csv|trace|pem|key|pfx|p12|token|secret)$'
    $violations = [System.Collections.Generic.List[string]]::new()

    foreach ($path in $tracked) {
        $normalized = $path.Replace('\', '/')
        if ($forbiddenPath.IsMatch($normalized)) {
            $violations.Add("forbidden tracked path: $normalized")
        }
        if ($normalized -ne $allowedProfile -and $forbiddenExtension.IsMatch($normalized)) {
            $violations.Add("forbidden tracked file type: $normalized")
        }
    }

    if ($allowedProfile -notin $tracked) {
        $violations.Add("canonical profile is not tracked: $allowedProfile")
    } else {
        $expectedHash = 'F25D4DC627B366C50CA30615A0DF46B43FA4E4B20D94DEA7658199A72A8DBA34'
        $actualHash = (Get-FileHash -LiteralPath $allowedProfile -Algorithm SHA256).Hash
        if ($actualHash -ne $expectedHash) {
            $violations.Add('canonical profile SHA-256 mismatch')
        }
    }

    $privateAppMarker = 'OpenAI' + '.Codex_'
    $contentPatterns = @(
        [regex]'(?i)[A-Z]:[\\/]+Users[\\/]+[^<\s\\/]+',
        [regex]'(?i)/home/[^<\s/]+',
        [regex]'(?i)/Users/[^<\s/]+',
        [regex]::new([regex]::Escape($privateAppMarker)),
        [regex]'gh[pousr]_[A-Za-z0-9_]{20,}',
        [regex]'github_pat_[A-Za-z0-9_]{20,}',
        [regex]'AKIA[0-9A-Z]{16}',
        [regex]'-----BEGIN (RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----'
    )
    $textExtensions = @(
        '', '.c', '.cc', '.cpp', '.h', '.hpp', '.cmake', '.json', '.md',
        '.ps1', '.py', '.sh', '.txt', '.yml', '.yaml', '.def', '.gitignore',
        '.gitattributes'
    )
    $emailPattern = [regex]'[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}'
    $ipv4Pattern = [regex]'(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])'

    foreach ($path in $tracked) {
        $normalized = $path.Replace('\', '/')
        if ($normalized -eq $allowedProfile -or
            $normalized -eq 'tools/audit-public-tree.ps1') {
            continue
        }
        $extension = [System.IO.Path]::GetExtension($normalized).ToLowerInvariant()
        if ($extension -notin $textExtensions -and
            [System.IO.Path]::GetFileName($normalized) -ne 'CMakeLists.txt') {
            continue
        }
        $content = Get-Content -LiteralPath $path -Raw
        foreach ($pattern in $contentPatterns) {
            if ($pattern.IsMatch($content)) {
                $violations.Add("private or secret-like content: $normalized")
                break
            }
        }
        foreach ($match in $emailPattern.Matches($content)) {
            if ($normalized -ne 'LICENSE' -or $match.Value -ne 'sam@hocevar.net') {
                $violations.Add("unexpected email address: $normalized")
            }
        }
        foreach ($match in $ipv4Pattern.Matches($content)) {
            $octets = @($match.Value.Split('.') | ForEach-Object { [int]$_ })
            if (($octets | Where-Object { $_ -gt 255 }).Count -ne 0) {
                continue
            }
            if ($match.Value -notin @('0.0.0.0', '127.0.0.1')) {
                $violations.Add("unexpected literal IP address: $normalized")
            }
        }
    }

    if ($violations.Count -ne 0) {
        $violations | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
        throw "Public-tree audit failed with $($violations.Count) finding(s)."
    }

    Write-Output "Public-tree audit passed for $($tracked.Count) tracked files."
} finally {
    Pop-Location
}
