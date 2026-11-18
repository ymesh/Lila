# Uploads the PDBs and binaries of a built config to Sentry, so crash reports from that build show function names and lines.
# Needs sentry-cli (winget install Sentry.sentry-cli) and an auth token in SENTRY_AUTH_TOKEN or %USERPROFILE%\.sentryclirc.
# -Check only verifies sentry-cli and the token.
param(
    [string]$Config = "Release",
    [string]$BuildDir = "build",
    [switch]$Check
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."

function Find-SentryCli {
    foreach ($name in "sentry-cli", "sentry") {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    $exe = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Sentry.sentry-cli_*\sentry.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($exe) { return $exe.FullName }
    throw "sentry-cli not found. Install it with: winget install Sentry.sentry-cli"
}

$cli = Find-SentryCli
# Windows PowerShell turns a native command's stderr into a terminating error under "Stop".
$ErrorActionPreference = "Continue"
& $cli info *> $null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) {
    throw "sentry-cli has no auth token. Create one at https://sentry.io/settings/auth-tokens/ (org token), then run: setx SENTRY_AUTH_TOKEN <token>"
}
if ($Check) { return }

$bin = Join-Path $repo "$BuildDir\bin\$Config"
if (-not (Get-ChildItem $bin -Filter lila.pdb -ErrorAction SilentlyContinue)) { throw "No lila.pdb in $bin. Build $Config first." }
# Sentry skips files it already has, so the unchanged third-party DLLs cost nothing after the first upload.
$ErrorActionPreference = "Continue"
& $cli debug-files upload --org jared-taylor-uj --project lila --wait $bin
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -ne 0) { throw "sentry-cli debug-files upload failed ($LASTEXITCODE)" }
"Uploaded symbols from $bin to Sentry"
