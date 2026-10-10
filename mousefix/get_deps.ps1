# Downloads the third-party files the build needs but the repository doesn't hold: binaries (dearxan's
# static library, Microsoft's texconv) and NVIDIA's DLSS SDK headers, whose license doesn't allow
# redistributing them as source. Each file is checked against the SHA-256 of the copy that was reviewed
# (see the SOURCE.txt files); a mismatch stops the build. Files already present and matching are kept.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent $PSScriptRoot

$ngx = 'https://raw.githubusercontent.com/NVIDIA/DLSS/374959484e79a640feaba44c93ac8cfb0a03f5b5/'
$ngxDir = 'mousefix\third_party\ngx\'
$files = @(
    @{ Path = $ngxDir + 'include\nvsdk_ngx.h';              Url = $ngx + 'include/nvsdk_ngx.h';              Sha = 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848' },
    @{ Path = $ngxDir + 'include\nvsdk_ngx_defs.h';         Url = $ngx + 'include/nvsdk_ngx_defs.h';         Sha = 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e' },
    @{ Path = $ngxDir + 'include\nvsdk_ngx_helpers.h';      Url = $ngx + 'include/nvsdk_ngx_helpers.h';      Sha = '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471' },
    @{ Path = $ngxDir + 'include\nvsdk_ngx_helpers_d3d.h';  Url = $ngx + 'include/nvsdk_ngx_helpers_d3d.h';  Sha = 'ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba' },
    @{ Path = $ngxDir + 'include\nvsdk_ngx_loader.h';       Url = $ngx + 'include/nvsdk_ngx_loader.h';       Sha = 'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75' },
    @{ Path = $ngxDir + 'include\nvsdk_ngx_params.h';       Url = $ngx + 'include/nvsdk_ngx_params.h';       Sha = '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc' },
    @{ Path = $ngxDir + 'LICENSE.txt';                      Url = $ngx + 'LICENSE.txt';                      Sha = 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0' },
    @{ Path = 'mousefix\third_party\dearxan\lib\dearxan.lib'; Zip = 'lib/dearxan.lib';
       Url = 'https://github.com/tremwil/dearxan/releases/download/v0.5.6/dearxan-0.5.6.zip'; Sha = '265c8f730eb93202cde75b401bb653f8276673709173dbe0fd1e9c569cd10d23' },
    @{ Path = 'tools\texconv.exe';
       Url = 'https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe'; Sha = 'dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06' }
)

function Get-Sha($file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower() }

foreach ($f in $files) {
    $dest = Join-Path $root $f.Path
    if ((Test-Path -LiteralPath $dest) -and (Get-Sha $dest) -eq $f.Sha) { continue }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    $tmp = "$dest.download"
    try {
        if ($f.Zip) {
            $zip = "$dest.zip"
            Invoke-WebRequest -UseBasicParsing -Uri $f.Url -OutFile $zip
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $archive = [IO.Compression.ZipFile]::OpenRead($zip)
            try {
                $entry = $archive.Entries | Where-Object { $_.FullName -eq $f.Zip }
                if (-not $entry) { throw "$($f.Zip) is missing from $($f.Url)" }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $tmp, $true)
            }
            finally { $archive.Dispose(); Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue }
        }
        else { Invoke-WebRequest -UseBasicParsing -Uri $f.Url -OutFile $tmp }
        $sha = Get-Sha $tmp
        if ($sha -ne $f.Sha) { throw "$($f.Path) from $($f.Url) has SHA-256 $sha, expected $($f.Sha). Not using it." }
        Move-Item -Force -LiteralPath $tmp -Destination $dest
        Write-Host "Downloaded $($f.Path)"
    }
    catch {
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
        Write-Host "Could not get $($f.Path): $_"
        exit 1
    }
}
