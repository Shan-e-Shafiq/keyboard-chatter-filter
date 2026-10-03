#Requires -Version 5.1
<#
.SYNOPSIS
    Installs keyboard-chatter-filter on Windows.

.DESCRIPTION
    Downloads the prebuilt release for this CPU from GitHub Releases, verifies its SHA-256 checksum
    (and its Authenticode signature when the release is signed), installs it to
    "%ProgramFiles%\keyboard-chatter-filter", registers the Windows service and starts it.

    Run from an elevated PowerShell (Run as administrator):

        irm https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.ps1 | iex

    Prefer to review first? Download the script, read it, then run it:

        irm https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.ps1 -OutFile install.ps1
        .\install.ps1

    Environment variables (all optional):
        KCF_REPO               GitHub "owner/repo" to install from
        KCF_VERSION            release tag, e.g. v0.1.0 (default: latest)
        KCF_REQUIRE_SIGNATURE  1 = refuse binaries without a valid Authenticode signature
#>
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

function Install-KeyboardChatterFilter {
    $program = 'keyboard-chatter-filter'
    $repo = if ($env:KCF_REPO) { $env:KCF_REPO } else { 'Shan-e-Shafiq/keyboard-chatter-filter' }
    $version = if ($env:KCF_VERSION) { $env:KCF_VERSION } else { 'latest' }
    if ($repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw "KCF_REPO must look like owner/repo" }
    if ($version -notmatch '^(latest|v[0-9A-Za-z.+-]+)$') { throw "KCF_VERSION must be 'latest' or a tag like v0.1.0" }

    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Installing a Windows service needs administrator rights. Open PowerShell with 'Run as administrator' and run the command again."
    }

    $nativeArch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    $arch = switch ($nativeArch) {
        'AMD64' { 'x86_64' }
        'ARM64' { 'arm64' }
        default { throw "Unsupported CPU architecture: $nativeArch" }
    }

    $asset = "$program-windows-$arch.zip"
    $base = if ($version -eq 'latest') { "https://github.com/$repo/releases/latest/download" }
            else { "https://github.com/$repo/releases/download/$version" }

    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("kcf-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        Write-Host "Downloading $asset ($version) from github.com/$repo"
        $zip = Join-Path $temp $asset
        $sums = Join-Path $temp 'SHA256SUMS'
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $zip
        Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums

        $expected = $null
        foreach ($line in Get-Content $sums) {
            $parts = $line -split '\s+', 2
            if ($parts.Count -eq 2 -and $parts[1].TrimStart('*') -eq $asset) { $expected = $parts[0].ToLowerInvariant() }
        }
        if (-not $expected) { throw "$asset is not listed in SHA256SUMS" }
        $actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { throw "Checksum mismatch for $asset (expected $expected, got $actual)" }
        Write-Host "Checksum verified (SHA-256 $actual)"

        $extract = Join-Path $temp 'extract'
        Expand-Archive -Path $zip -DestinationPath $extract
        $exe = Join-Path $extract "$program.exe"
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "The archive does not contain $program.exe" }

        $signature = Get-AuthenticodeSignature -FilePath $exe
        if ($signature.Status -eq 'Valid') {
            Write-Host "Authenticode signature valid: $($signature.SignerCertificate.Subject)"
        } elseif ($env:KCF_REQUIRE_SIGNATURE -eq '1') {
            throw "$program.exe has no valid Authenticode signature ($($signature.Status))"
        } else {
            Write-Host "Note: this release is not Authenticode-signed; integrity was verified with SHA-256."
        }

        $installDir = Join-Path $env:ProgramFiles $program
        $target = Join-Path $installDir "$program.exe"
        if (Get-Service -Name $program -ErrorAction SilentlyContinue) {
            Write-Host "Stopping the running service for the upgrade"
            Stop-Service -Name $program -Force -ErrorAction SilentlyContinue
            (Get-Service -Name $program).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
        }
        New-Item -ItemType Directory -Force -Path $installDir | Out-Null
        for ($attempt = 1; ; $attempt++) {
            try { Copy-Item -LiteralPath $exe -Destination $target -Force; break }
            catch { if ($attempt -ge 10) { throw }; Start-Sleep -Milliseconds 500 }  # agents may still be exiting
        }
        Write-Host "Installed $target"

        $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
        if (($machinePath -split ';') -notcontains $installDir) {
            [Environment]::SetEnvironmentVariable('Path', ($machinePath.TrimEnd(';') + ';' + $installDir), 'Machine')
            Write-Host "Added $installDir to the system PATH (new terminals)"
        }

        & $target install
        if ($LASTEXITCODE -ne 0) { throw "Registering the service failed (exit code $LASTEXITCODE)" }
        Write-Host ""
        & $target status
        Write-Host ""
        Write-Host "Done. Useful commands (in a new terminal):"
        Write-Host "  $program status      show what the filter is doing"
        Write-Host "  $program config      show the configuration file and settings"
        Write-Host "  $program uninstall   remove everything again (elevated terminal)"
    }
    finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Install-KeyboardChatterFilter
