# Stages a self-contained Lila install into build-steam\content for SteamPipe, or -Destination for the installer.
param(
    [string]$Config = "Release",
    [string]$PythonDir = $(if ($env:LILA_PYTHON_DIR) { $env:LILA_PYTHON_DIR } else { "C:\Python314" }),
    [string]$Destination = "",
    # Steam installs the VC++ runtime itself; a per-user installer can't, so it ships the DLLs beside lila.exe.
    [switch]$AppLocalCrt,
    [string]$VsDir = $(if ($env:LILA_VS) { $env:LILA_VS } else { "C:\Program Files\Microsoft Visual Studio\2022\Community" })
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bin = Join-Path $repo "build\bin\$Config"
$out = if ($Destination) { $Destination } else { Join-Path $repo "build-steam\content" }
if (-not (Test-Path "$bin\lila.exe")) { throw "No lila.exe in $bin. Build $Config first." }
# python-site holds the PySide6 lila.ui needs, bound to the Qt beside lila.exe; the bin copy below carries it.
if (-not (Test-Path "$bin\python-site\PySide6\QtWidgets.pyd")) { throw "No PySide6 in $bin\python-site. Build with LILA_PYSIDE on." }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null

# Test executables and build leftovers stay behind.
robocopy $bin $out /E /NFL /NDL /NJH /NJS /NP /XD __pycache__ `
    /XF *.pdb *.ilk *.old* *.iconbak imgui.ini *_tests.exe lila_crashtest.exe | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy bin failed" }
# The motion model is not in git; a release without it would ship Motion broken.
if (-not (Test-Path "$out\models\motionbricks\mmm_v6_soma_fp16.onnx")) { throw "No models\motionbricks\mmm_v6_soma_fp16.onnx in $bin. Configure with LILA_MOTIONBRICKS_MODEL_DIR and build." }

robocopy "$repo\resources" "$out\resources" /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy "$repo\python" "$out\python" /E /NFL /NDL /NJH /NJS /NP /XD tests __pycache__ | Out-Null

# Embedded interpreter: stdlib plus numpy, which Lila's own scripts use.
$py = "$out\python-runtime"
robocopy "$PythonDir\Lib" "$py\Lib" /E /NFL /NDL /NJH /NJS /NP /XD site-packages test idlelib turtledemo __pycache__ | Out-Null
robocopy "$PythonDir\DLLs" "$py\DLLs" /E /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($pkg in "numpy", "numpy.libs") {
    robocopy "$PythonDir\Lib\site-packages\$pkg" "$py\Lib\site-packages\$pkg" /E /NFL /NDL /NJH /NJS /NP /XD __pycache__ tests | Out-Null
}
Copy-Item "$PythonDir\python.exe", "$PythonDir\pythonw.exe", "$PythonDir\python3.dll", "$PythonDir\python314.dll", "$PythonDir\LICENSE.txt" $py
Copy-Item "$repo\LICENSE", "$repo\THIRD-PARTY-NOTICES.md" $out

# Steam achievements load this at startup; installs from elsewhere don't have it, so they never talk to Steam.
if (-not $Destination) {
    $steamApi = "$repo\third_party\steamworks\sdk\redistributable_bin\win64\steam_api64.dll"
    if (-not (Test-Path $steamApi)) { throw "No $steamApi. Extract the Steamworks SDK to third_party\steamworks\sdk." }
    Copy-Item $steamApi $out
}

if ($AppLocalCrt) {
    $crt = Get-Item "$VsDir\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" | Sort-Object FullName | Select-Object -Last 1
    if (-not $crt) { throw "No VC++ runtime under $VsDir\VC\Redist" }
    Copy-Item "$($crt.FullName)\*.dll" $out
}

$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"Staged {0:N0} MB to {1}" -f $size, $out
