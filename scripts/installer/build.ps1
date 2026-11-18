# Builds build-installer\Lila-<version>-Setup.exe from an already built config, files-<version>.json (the install's file
# list), a Lila-<version>-Patch-<from>.exe for each of the last few releases that published a file list, and latest.json
# for the updater. Release.cmd publishes them to the GitHub release v<version>; the updater reads latest.json from the
# newest release.
param(
    [string]$Config = "Release",
    [string]$BuildDir = "build",
    [string]$Notes = "",
    # How many earlier releases get a patch. 0 builds only the full installer.
    [int]$Patches = 3,
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$stage = Join-Path $repo "build-installer\content"
$output = Join-Path $repo "build-installer"
if (-not (Test-Path $Iscc)) {
    $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (-not (Test-Path $Iscc)) { throw "Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup" }
}

$version = (Select-String -Path "$repo\CMakeLists.txt" -Pattern 'project\(Lila VERSION ([0-9.]+)').Matches[0].Groups[1].Value
& "$repo\scripts\steam\stage.ps1" -Config $Config -BuildDir $BuildDir -Destination $stage -AppLocalCrt
python "$PSScriptRoot\patches.py" files $stage $version
if ($LASTEXITCODE -ne 0) { throw "Listing the install's files failed" }
Copy-Item "$stage\lila-files.json" "$output\files-$version.json"

& $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DRepoDir=$repo" "/DOutputDir=$output" "$PSScriptRoot\Lila.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
$exe = "$output\Lila-$version-Setup.exe"
$release = "https://github.com/lila-3d/Lila/releases"

# Earlier releases' file lists come from their GitHub releases; one without a list (older than the patch system) gets no patch.
$patchList = @()
$previous = Join-Path $output "previous"
Remove-Item $previous -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem $output -Filter "Lila-$version-Patch-*.exe" | Remove-Item
if ($Patches -gt 0 -and (Get-Command gh -ErrorAction SilentlyContinue)) {
    # A release without the asset makes gh fail; that only means no patch from it.
    $ErrorActionPreference = "Continue"
    $tags = gh release list --repo lila-3d/Lila --limit 20 --json tagName --jq ".[].tagName" | Where-Object { $_ -ne "v$version" } | Select-Object -First $Patches
    foreach ($tag in $tags) {
        $from = $tag.Substring(1)
        gh release download $tag --repo lila-3d/Lila --pattern "files-$from.json" --dir $previous --clobber 2>$null
        $old = "$previous\files-$from.json"
        if (-not (Test-Path $old)) { "No file list in $tag, so no patch from $from"; continue }
        $patchDir = "$output\patch-$from"
        $deletes = "$output\patch-$from-deletes.iss"
        "Patch from ${from}: " + (python "$PSScriptRoot\patches.py" stage $stage "$stage\lila-files.json" $old $patchDir $deletes)
        if ($LASTEXITCODE -ne 0) { throw "Staging the patch from $from failed" }
        & $Iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DRepoDir=$repo" "/DOutputDir=$output" "/DPatchFrom=$from" "/DPatchDir=$patchDir" "/DPatchDeletes=$deletes" "$PSScriptRoot\Lila.iss"
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed on the patch from $from ($LASTEXITCODE)" }
        $patch = "$output\Lila-$version-Patch-$from.exe"
        $patchList += [ordered]@{
            from = $from
            installer = "$release/download/v$version/Lila-$version-Patch-$from.exe"
            sha256 = (Get-FileHash $patch -Algorithm SHA256).Hash.ToLower()
            size = (Get-Item $patch).Length
        }
    }
    $ErrorActionPreference = "Stop"
} elseif ($Patches -gt 0) {
    "GitHub CLI not found, so no patches: install it with winget install GitHub.cli"
}

$manifest = [ordered]@{
    version = $version
    notes = $Notes
    page = "$release/tag/v$version"
    installer = "$release/download/v$version/Lila-$version-Setup.exe"
    sha256 = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
    size = (Get-Item $exe).Length
    patches = @($patchList)
}
[IO.File]::WriteAllText("$output\latest.json", ($manifest | ConvertTo-Json -Depth 4))
"Built $exe and $output\latest.json"
