# The one-click builder: "Build Beyond Oasis.bat" runs this.
#
# Builds BeyondOasis.exe from the user's own ROM on their own PC:
#   1. finds the build tools, or downloads portable copies of them into toolchain\ (the
#      publishers' own releases, checked against pinned SHA-256s; nothing is installed into
#      Windows, and deleting the folder removes them)
#   2. asks for the ROM and checks it is Beyond Oasis (USA)
#   3. recompiles the ROM to C and builds the game
#   4. puts a "Beyond Oasis" shortcut on the desktop
# Run it again after updating the source to rebuild. -NoSystemTools ignores tools already on
# PATH and uses (or downloads) the portable ones; -NoShortcut leaves the desktop alone;
# -Unattended (for testing) answers yes to the download, never starts the game or waits
# for Enter, and needs the ROM already picked (local.json).
param([switch]$NoSystemTools, [switch]$NoShortcut, [switch]$Unattended)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Tools = Join-Path $Root 'toolchain'
Set-Location $Root
# Windows' own curl and tar (10 1803 and later), not whichever comes first on PATH: Git's
# tar reads C:\... as a remote host.
$Curl = Join-Path $env:SystemRoot 'System32\curl.exe'
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'

function Say($text) { Write-Host $text }
function Step($text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail($text) { throw $text }
function Ask($question) {
    if ($Unattended) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

# ---- 1. Tools ------------------------------------------------------------------------
# Each: where its portable copy lives, the download, its SHA-256, and how to tell whether a
# copy already on PATH will do. Clang must be a MinGW one (llvm-mingw): the MSVC-flavoured
# LLVM from llvm.org would also need Visual Studio's libraries.
$Needed = @(
    @{ Name = 'Node.js 22.20.0'; Dir = 'node'; Bin = '.'; Exe = 'node.exe'; MB = 34
       Url = 'https://nodejs.org/dist/v22.20.0/node-v22.20.0-win-x64.zip'
       Sha = 'bb819d6eb8f5bfda294bbc83a7e4ec6539da67c4233d54b0d655b9248b15e29d'
       Ok = { $v = (& node --version); $v -match '^v(\d+)' -and [int]$Matches[1] -ge 18 } },
    @{ Name = 'CMake 3.31.12'; Dir = 'cmake'; Bin = 'bin'; Exe = 'cmake.exe'; MB = 45
       Url = 'https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-windows-x86_64.zip'
       Sha = '0c4baa40f28b3f8225eb3fdf6946c987b4fe901403b4eaf2fbbd9378100aaa0c'
       Ok = { $v = (& cmake --version) | Select-Object -First 1; $v -match '(\d+)\.(\d+)' -and ([int]$Matches[1] -gt 3 -or [int]$Matches[2] -ge 20) } },
    @{ Name = 'Ninja 1.13.2'; Dir = 'ninja'; Bin = '.'; Exe = 'ninja.exe'; MB = 1
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
       Sha = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
       Ok = { $null -ne (& ninja --version) } },
    @{ Name = 'LLVM-MinGW 20260616 (Clang)'; Dir = 'llvm-mingw'; Bin = 'bin'; Exe = 'clang.exe'; MB = 179
       Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip'
       Sha = 'b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35'
       Ok = { $m = (& clang -dumpmachine); $m -match 'mingw|windows-gnu' } }
)

function OnPath($tool) {
    if ($NoSystemTools) { return $false }
    try { return [bool](& $tool.Ok) } catch { return $false }
}

function Fetch($tool) {
    $zip = Join-Path $Tools "$($tool.Dir).zip"
    $tmp = Join-Path $Tools "$($tool.Dir).tmp"
    Say "Downloading $($tool.Name) (about $($tool.MB) MB)..."
    & $Curl -L --fail --silent --show-error -o $zip $tool.Url
    if ($LASTEXITCODE -ne 0) { Fail "Could not download $($tool.Url)" }
    $sha = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($sha -ne $tool.Sha) { Remove-Item $zip; Fail "$($tool.Name): the download does not match its published SHA-256 (got $sha). Not using it." }
    Say "  checked, unpacking..."
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory $tmp | Out-Null
    & $Tar -xf $zip -C $tmp
    if ($LASTEXITCODE -ne 0) { Fail "Could not unpack $zip" }
    Remove-Item $zip
    # Most zips hold one top folder; Ninja's holds ninja.exe on its own.
    $inner = @(Get-ChildItem $tmp)
    $src = if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) { $inner[0].FullName } else { $tmp }
    $dest = Join-Path $Tools $tool.Dir
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $src $dest
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
}

function Main {
    Write-Host 'Beyond Oasis Recomp builder' -ForegroundColor White
    Say 'This builds the game for this PC from your own Beyond Oasis (USA) ROM.'
    Say 'No part of the game comes with it: you need your own copy of the cartridge.'

    Step 'Build tools'
    $missing = @()
    foreach ($t in $Needed) {
        $portable = Join-Path (Join-Path (Join-Path $Tools $t.Dir) $t.Bin) $t.Exe
        if (Test-Path $portable) { $t.Path = Split-Path $portable; Say "  $($t.Name): in toolchain\" }
        elseif (OnPath $t) { Say "  $($t.Exe): found on this PC" }
        else { $missing += $t }
    }
    if ($missing.Count) {
        $mb = 0
        foreach ($t in $missing) { $mb += $t.MB }
        Say ''
        Say 'These are needed and will be downloaded from their publishers into the toolchain'
        Say 'folder here (nothing is installed into Windows; delete the folder to remove them):'
        foreach ($t in $missing) { Say "  $($t.Name)  $($t.MB) MB  $($t.Url)" }
        if (-not (Ask "Download them ($mb MB)?")) { Fail 'Stopped: the build needs these tools.' }
        New-Item -ItemType Directory -Force $Tools | Out-Null
        foreach ($t in $missing) { Fetch $t; $t.Path = Join-Path (Join-Path $Tools $t.Dir) $t.Bin }
    }
    foreach ($t in $Needed) { if ($t.Path) { $env:PATH = "$($t.Path);$env:PATH" } }

    if (-not (Test-Path (Join-Path $Root 'third_party\sdl2\CMakeLists.txt'))) {
        if (Get-Command git -ErrorAction SilentlyContinue) {
            Say 'Fetching the third-party code (git submodules)...'
            & git submodule update --init --recursive
        }
        if (-not (Test-Path (Join-Path $Root 'third_party\sdl2\CMakeLists.txt'))) {
            Fail "third_party\ is empty. Download the 'Beyond Oasis Recomp builder' zip from the Releases page (GitHub's 'Source code' zip leaves the third-party code out)."
        }
    }

    Step 'Your ROM'
    $rom = $null
    $local = Join-Path $Root 'local.json'
    if (Test-Path $local) {
        $saved = (Get-Content $local -Raw | ConvertFrom-Json).romPath
        if ($saved -and (Test-Path $saved)) {
            & node tools/setup.mjs $saved
            if ($LASTEXITCODE -eq 0) { $rom = $saved }
        }
    }
    if (-not $rom -and $Unattended) { Fail 'No ROM picked yet (local.json).' }
    while (-not $rom) {
        Say 'Pick your Beyond Oasis (USA) ROM (.md, .bin or .gen) in the window that opens.'
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = 'Your Beyond Oasis (USA) ROM'
        $dlg.Filter = 'Genesis ROMs (*.md;*.bin;*.gen)|*.md;*.bin;*.gen|All files (*.*)|*.*'
        if ($dlg.ShowDialog() -ne 'OK') { Fail 'Stopped: no ROM picked.' }
        & node tools/setup.mjs $dlg.FileName
        if ($LASTEXITCODE -eq 0) { $rom = $dlg.FileName }
        else { Say 'That file is not Beyond Oasis (USA), unchanged and unheadered (.smd dumps need converting). Pick another.' }
    }

    Step 'Recompiling and building (a few minutes the first time)'
    & node tools/disasm.mjs | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 'The disassembler failed (see above).' }
    $build = Join-Path $Root 'build\cmake'
    if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
        & cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        if ($LASTEXITCODE -ne 0) { Fail 'CMake could not configure the build (see above).' }
    }
    & cmake --build build/cmake
    if ($LASTEXITCODE -ne 0) { Fail 'The build failed (see above).' }
    $exe = Join-Path $build 'BeyondOasis.exe'

    # The game remembers its ROM in settings.ini; set it there rather than in the shortcut,
    # so moving the ROM later only needs picking it again in the game.
    $ini = Join-Path $build 'settings.ini'
    $lines = @()
    $utf8 = New-Object System.Text.UTF8Encoding $false
    if (Test-Path $ini) { $lines = @([IO.File]::ReadAllLines($ini, $utf8) | Where-Object { $_ -notmatch '^rom_path=' }) }
    [IO.File]::WriteAllLines($ini, [string[]](@("rom_path=$rom") + $lines), $utf8)

    $lnk = '(none)'
    if (-not $NoShortcut) {
        $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Beyond Oasis.lnk'
        $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
        $sc.TargetPath = $exe
        $sc.WorkingDirectory = $build
        $sc.Description = 'Beyond Oasis Recomp'
        $sc.Save()
    }

    Step 'Done'
    Say "The game:          $exe"
    Say "Desktop shortcut:  $lnk"
    Say 'Saves, states and settings.ini live next to the game. Run the builder again after updating to rebuild.'
    if (-not $Unattended -and (Ask 'Play now?')) { Start-Process -FilePath $exe -WorkingDirectory $build }
}

try { Main; $code = 0 }
catch { Write-Host ''; Write-Host $_.Exception.Message -ForegroundColor Red; $code = 1 }
if (-not $Unattended) { Read-Host 'Press Enter to close' | Out-Null }
exit $code
