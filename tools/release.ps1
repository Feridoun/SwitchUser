# Builds a release into build\dist:
#   SwitchUser-Setup-<version>.exe   Inno Setup installer (what winget installs)
#   SwitchUser.exe                   the program on its own ("SwitchUser.exe install")
#   SHA256SUMS.txt
#
# Signed with the certificate named by -CertThumbprint or
# $env:SWITCHUSER_SIGN_THUMBPRINT (see tools\sign.ps1; SimplySign Desktop must
# be connected). -AllowUnsigned builds without signing, for testing only.
#
# Needs Visual Studio 2019+ (C++ workload), the Windows SDK and Inno Setup 6
# (winget install JRSoftware.InnoSetup).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$CertThumbprint = $env:SWITCHUSER_SIGN_THUMBPRINT,
    [switch]$AllowUnsigned
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$signScript = Join-Path $PSScriptRoot 'sign.ps1'

if (-not $CertThumbprint -and -not $AllowUnsigned) {
    throw 'No signing certificate: pass -CertThumbprint, set SWITCHUSER_SIGN_THUMBPRINT, or use -AllowUnsigned for a test build.'
}
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup).' }

# 1. Build
& cmd /c "`"$root\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$exe = "$root\SwitchUser.exe"

# 2. Sign the program
if ($CertThumbprint) { & $signScript -Path $exe -CertThumbprint $CertThumbprint }

# 3. Installer (Inno signs setup.exe and its uninstaller through the "certum"
# tool; $f is the file to sign and $q a quote, as Inno strips real quotes).
$dist = "$root\build\dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null
$isccArgs = @("/DMyAppVersion=$Version", "/O$dist")
if ($CertThumbprint) {
    $isccArgs += '/DSignBuild=1'
    $isccArgs += "/Scertum=powershell -NoProfile -ExecutionPolicy Bypass -File `$q$signScript`$q -CertThumbprint $CertThumbprint -Description `$qSwitchUser Setup`$q -Path `$f"
}
& $iscc @isccArgs "$root\installer\SwitchUser.iss" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed (exit $LASTEXITCODE)." }
$setup = "$dist\SwitchUser-Setup-$Version.exe"
if (-not (Test-Path $setup)) { throw "Installer not found at $setup." }
Copy-Item $exe $dist

# 4. Check signatures, write checksums
if ($CertThumbprint) {
    foreach ($file in "$dist\SwitchUser.exe", $setup) {
        $signature = Get-AuthenticodeSignature $file
        if ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate) {
            throw "$file is not validly signed and timestamped ($($signature.Status))."
        }
    }
}
Get-ChildItem $dist -File | Where-Object Name -ne 'SHA256SUMS.txt' | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
} | Set-Content "$dist\SHA256SUMS.txt" -Encoding ascii

Write-Host ''
Get-ChildItem $dist | ForEach-Object { Write-Host ('  {0,-35} {1,8:N0} KB' -f $_.Name, ($_.Length / 1KB)) }
if (-not $CertThumbprint) { Write-Host '  UNSIGNED: for testing only, do not publish.' -ForegroundColor Red }
