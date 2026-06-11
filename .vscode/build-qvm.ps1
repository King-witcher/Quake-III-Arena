<#
.SYNOPSIS
    Builds the baseq3 QVMs (qagame.qvm, cgame.qvm, ui.qvm) with the original
    lcc + q3asm bytecode toolchain and drops them in run\baseq3\vm.

.DESCRIPTION
    Mirrors the original code\game\game.bat, code\cgame\cgame.bat and
    code\q3_ui\q3_ui.bat compile loops, but instead of writing the .qvm files to
    the hard-coded \quake3\baseq3\vm path it writes them into the project's
    run\baseq3\vm folder, which the launch configs use as fs_basepath.

    The module list (and assembly order) for each VM is read straight from the
    matching .q3asm file, so this stays in sync with the original sources. The
    "-o" line in those files is ignored; we pass our own -o to q3asm.

    Entries that contain a path separator (e.g. ..\g_syscalls) are the
    hand-written syscall trampolines that ship as .asm in the tree, so they are
    NOT compiled - q3asm reads them directly. Every other entry is an lcc
    bytecode compile of the matching .c source.
#>
[CmdletBinding()]
param(
    # Project root (the folder that contains "code" and "run"). The VS Code task
    # passes -Root "${workspaceFolder}". If omitted we fall back to this script's
    # location, but only when it is known (it is empty when the body is pasted /
    # "run selection" instead of executed as a file).
    [string]$Root
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    if ($PSScriptRoot) {
        $Root = Split-Path -Parent $PSScriptRoot
    } else {
        throw "Could not determine the project root. Run this script as a file or pass -Root <project folder>."
    }
}

$code  = Join-Path $Root 'code'
$bin   = Join-Path $code 'win32\mod-sdk-setup\bin'
$lcc   = Join-Path $bin  'lcc.exe'
$q3asm = Join-Path $bin  'q3asm.exe'
# Our QVMs go into the run\dev mod folder, which the engine searches BEFORE the
# baseq3 paks (fs_game=dev), so they override the retail vm/*.qvm in the paks.
$outVm = Join-Path $Root 'run\dev\vm'

foreach ($tool in @($lcc, $q3asm)) {
    if (-not (Test-Path $tool)) { throw "QVM toolchain missing: $tool" }
}

# lcc finds its q3cpp/q3rcc siblings next to itself; make sure the tools are on
# PATH too, and keep the include/lib env clean exactly like the original .bat.
$env:PATH    = "$bin;$env:PATH"
$env:INCLUDE = ''
$env:LIBRARY = ''

New-Item -ItemType Directory -Force -Path $outVm | Out-Null

$incCgame = Join-Path $code 'cgame'
$incGame  = Join-Path $code 'game'
$incUi    = Join-Path $code 'ui'
$incQ3ui  = Join-Path $code 'q3_ui'

# out   : qvm basename the engine loads (VM_Create name)
# dir   : folder holding the .q3asm list + module sources
# list  : the .q3asm file describing the link order
# defs  : extra lcc defines on top of -DQ3_VM
# incs  : include dirs (mirrors the matching .bat)
$targets = @(
    @{ out = 'qagame'; dir = (Join-Path $code 'game');  list = 'game.q3asm';  defs = @();          incs = @($incCgame, $incGame, $incUi)   }
    @{ out = 'cgame';  dir = (Join-Path $code 'cgame'); list = 'cgame.q3asm'; defs = @('-DCGAME'); incs = @($incCgame, $incGame, $incUi)   }
    @{ out = 'ui';     dir = (Join-Path $code 'q3_ui'); list = 'q3_ui.q3asm'; defs = @();          incs = @($incCgame, $incGame, $incQ3ui) }
)

function Resolve-Source {
    param([string]$Name, [string]$SrcDir)
    foreach ($d in @($SrcDir, $incGame)) {
        $p = Join-Path $d "$Name.c"
        if (Test-Path $p) { return $p }
    }
    throw "cannot find C source for module '$Name' (looked in $SrcDir and $incGame)"
}

foreach ($m in $targets) {
    Write-Host ""
    Write-Host "==== building $($m.out).qvm ====" -ForegroundColor Cyan

    $srcDir = $m.dir
    $vmDir  = Join-Path $srcDir 'vm'
    New-Item -ItemType Directory -Force -Path $vmDir | Out-Null
    Get-ChildItem -Path $vmDir -Filter '*.asm' -ErrorAction SilentlyContinue | Remove-Item -Force

    # Read the link order from the .q3asm file (skip the "-o ..." override line).
    $entries = Get-Content (Join-Path $srcDir $m.list) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' -and $_ -notmatch '^-o(\s|$)' }

    $includeArgs = $m.incs | ForEach-Object { "-I$_" }

    Push-Location $vmDir
    try {
        foreach ($e in $entries) {
            if ($e -match '[\\/]') { continue }   # hand-written syscall .asm - no compile
            $src = Resolve-Source -Name $e -SrcDir $srcDir
            $lccArgs = @('-DQ3_VM') + $m.defs + @('-S', '-Wf-target=bytecode', '-Wf-g') + $includeArgs + @($src)
            & $lcc @lccArgs
            if ($LASTEXITCODE -ne 0) { throw "lcc failed on $src (exit $LASTEXITCODE)" }
        }

        # q3asm: our -o wins because we never pass -f, so the in-file -o is unread.
        $outBase = Join-Path $outVm $m.out
        & $q3asm '-o' $outBase @entries
        if ($LASTEXITCODE -ne 0) { throw "q3asm failed for $($m.out) (exit $LASTEXITCODE)" }
    }
    finally {
        Pop-Location
    }

    Write-Host "  -> $(Join-Path $outVm "$($m.out).qvm")" -ForegroundColor Green
}

Write-Host ""
Write-Host "QVMs written to $outVm" -ForegroundColor Green
