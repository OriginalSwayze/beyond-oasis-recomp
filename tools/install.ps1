# The installer's engine: Setup.exe unpacks the source into <Install>\source and runs this,
# hidden, reading its progress. It can also be run by hand.
#
#   install.ps1 -Install <folder> -Rom <rom>[|<rom>...] [-NoSystemTools]
#
# Checks the ROMs (any versions in config/versions.json: each is a language), recompiles them
# to C and builds the game, then puts BeyondOasis.exe in <Install>\ (its settings.ini there
# remembers each version's ROM; settings, saves and save states there are kept on a
# reinstall). The build tools are used from PATH when they will do, else
# downloaded once into <Install>\source\toolchain (the publishers' own releases, checked
# against pinned SHA-256s; nothing is installed into Windows).
#
# Progress goes to stdout as "@@ <percent> <what>", a failure as "@@error <why>" (exit 1),
# and everything else the tools print as it comes.
param([Parameter(Mandatory)][string]$Install, [Parameter(Mandatory)][string]$Rom, [switch]$NoSystemTools)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Tools = Join-Path $Root 'toolchain'
Set-Location $Root
# Windows' own curl and tar, not whichever comes first on PATH: Git's tar reads C:\... as a host.
$Curl = Join-Path $env:SystemRoot 'System32\curl.exe'
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'

function Progress($percent, $text) { [Console]::Out.WriteLine("@@ $percent $text"); [Console]::Out.Flush() }
function Say($text) { [Console]::Out.WriteLine($text); [Console]::Out.Flush() }

# ---- Tools ------------------------------------------------------------------------------
# The same as builder.ps1's. Clang must be a MinGW one (llvm-mingw): the MSVC-flavoured LLVM
# would need Visual Studio's libraries.
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
    & $Curl -L --fail --silent --show-error -o $zip $tool.Url
    if ($LASTEXITCODE -ne 0) { throw "Could not download $($tool.Name). Check the internet connection and try again." }
    $sha = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($sha -ne $tool.Sha) { Remove-Item $zip; throw "$($tool.Name): the download does not match its published SHA-256. Not using it." }
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory $tmp | Out-Null
    & $Tar -xf $zip -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "Could not unpack $($tool.Name)." }
    Remove-Item $zip
    $inner = @(Get-ChildItem $tmp)    # most zips hold one top folder; Ninja's holds ninja.exe alone
    $src = if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) { $inner[0].FullName } else { $tmp }
    $dest = Join-Path $Tools $tool.Dir
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $src $dest
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
}

# A native tool, its output passed on; throws with $why if it fails.
function Run($why) {
    $exe = $args[0]
    $rest = @($args | Select-Object -Skip 1)    # an array even with one argument
    # Windows PowerShell turns a native tool's stderr lines into errors when they are merged
    # into its output: the tools' warnings must not stop the install.
    $ErrorActionPreference = 'Continue'
    & $exe @rest 2>&1 | ForEach-Object { Say "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw $why }
}

function Main {
    New-Item -ItemType Directory -Force $Install | Out-Null

    Progress 2 'Checking the build tools'
    $missing = @()
    foreach ($t in $Needed) {
        $portable = Join-Path (Join-Path (Join-Path $Tools $t.Dir) $t.Bin) $t.Exe
        if (Test-Path $portable) { $t.Path = Split-Path $portable }
        elseif (-not (OnPath $t)) { $missing += $t }
    }
    if ($missing.Count) {
        New-Item -ItemType Directory -Force $Tools | Out-Null
        $k = 0
        foreach ($t in $missing) {
            Progress (4 + [int](36 * $k / $missing.Count)) "Downloading $($t.Name) ($($t.MB) MB)"
            Fetch $t
            $t.Path = Join-Path (Join-Path $Tools $t.Dir) $t.Bin
            $k++
        }
    }
    foreach ($t in $Needed) { if ($t.Path) { $env:PATH = "$($t.Path);$env:PATH" } }
    if (-not (Test-Path (Join-Path $Root 'third_party\sdl2\CMakeLists.txt'))) { throw 'The source is incomplete: third_party is empty.' }

    Progress 40 'Checking your ROMs'
    $roms = @($Rom -split '\|' | Where-Object { $_ })
    if (Test-Path (Join-Path $Root 'local.json')) { Remove-Item (Join-Path $Root 'local.json') }   # only the ROMs given
    Run 'A file is not Beyond Oasis or The Story of Thor, unchanged.' node tools/setup.mjs @roms
    Progress 45 'Finding the game''s code'
    Run 'The disassembler failed.' node tools/disasm.mjs
    if (-not (Test-Path (Join-Path $Root 'build\cmake\build.ninja'))) {
        Progress 50 'Setting up the build'
        Run 'CMake could not set up the build.' cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    }
    Progress 55 'Recompiling and building (a few minutes)'
    Run 'The build failed.' cmake --build build/cmake --target BeyondOasis
    Copy-Item (Join-Path $Root 'build\cmake\BeyondOasis.exe') $Install -Force

    # The game remembers each version's ROM in settings.ini next to it (rom_<id>), and plays
    # the language chosen before if that version is still there, else the first.
    $ini = Join-Path $Install 'settings.ini'
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $paths = (Get-Content (Join-Path $Root 'local.json') -Raw | ConvertFrom-Json).roms
    $ids = @($paths.PSObject.Properties | ForEach-Object { $_.Name })
    $lines = @()
    $language = $null
    if (Test-Path $ini) {
        $old = [IO.File]::ReadAllLines($ini, $utf8)
        $chosen = $old | Where-Object { $_ -match '^language=(.+)$' } | ForEach-Object { $Matches[1] } | Select-Object -First 1
        if ($chosen -and $ids -contains $chosen) { $language = $chosen }
        $lines = @($old | Where-Object { $_ -notmatch '^(rom_path|rom_[a-z]+|language)=' })
    }
    if (-not $language) { $language = if ($ids -contains 'us') { 'us' } else { $ids[0] } }
    $head = @("rom_path=$($paths.$language)", "language=$language") + @($ids | ForEach-Object { "rom_$_=$($paths.$_)" })
    [IO.File]::WriteAllLines($ini, [string[]]($head + $lines), $utf8)
    Progress 100 'Done'
}

try { Main; exit 0 }
catch { [Console]::Out.WriteLine("@@error $($_.Exception.Message)"); exit 1 }
