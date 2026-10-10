# Copies the newest DLSS model (nvngx_dlss.dll) that the NVIDIA driver itself downloaded to
# C:\ProgramData\NVIDIA\NGX\models\dlss into bin\DS3DLAA\. Nothing is downloaded: only files with a valid
# NVIDIA Corporation signature are considered.
param([string]$OutDir = "$PSScriptRoot\bin\DS3DLAA")

$candidates = Get-ChildItem "$env:ProgramData\NVIDIA\NGX\models\dlss\versions\*\files\*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
    $sig = Get-AuthenticodeSignature $_.FullName
    $info = $_.VersionInfo
    if ($sig.Status -eq 'Valid' -and $sig.SignerCertificate.Subject -match '^CN=NVIDIA Corporation,' -and $info.FileDescription -match 'DLSS') {
        [pscustomobject]@{ Path = $_.FullName; Version = [version]($info.ProductVersion -replace ',', '.') }
    }
}
$best = $candidates | Sort-Object Version -Descending | Select-Object -First 1
if (-not $best) {
    Write-Host "No NVIDIA-signed DLSS model found in $env:ProgramData\NVIDIA\NGX\models\dlss. Update the NVIDIA driver (or run a DLSS game once)."
    exit 1
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item $best.Path "$OutDir\nvngx_dlss.dll" -Force
Write-Host "DLSS $($best.Version) copied from $($best.Path)"
