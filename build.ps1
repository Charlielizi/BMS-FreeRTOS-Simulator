param([switch]$Tests)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$kernel = Join-Path $root 'third_party\FreeRTOS-Kernel'
$zig = if ($env:BMS_ZIG_EXE) { $env:BMS_ZIG_EXE } else {
    $installed = Get-Command zig -ErrorAction SilentlyContinue
    if ($installed) { $installed.Source } else {
        Join-Path $root '..\开发练习\BMS桌面版\.tools\zig-x86_64-windows-0.17.0\zig.exe'
    }
}
if (-not (Test-Path -LiteralPath $zig)) {
    throw "找不到 Zig 编译器。请设置 BMS_ZIG_EXE 指向 Zig 0.17.0：$zig"
}
$zigVersion = & $zig version
if ($LASTEXITCODE -ne 0 -or $zigVersion -ne '0.17.0') {
    throw "需要 Zig 0.17.0，当前为 $zigVersion。请设置 BMS_ZIG_EXE。"
}
if (-not (Test-Path -LiteralPath (Join-Path $kernel 'portable\MSVC-MingW\port.c'))) {
    throw '缺少 FreeRTOS Kernel V11.3.1 Windows 端口，请检查 third_party/FreeRTOS-Kernel。'
}
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$src = Join-Path $root 'src'
$flags = @(
    '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-DWIN32_LEAN_AND_MEAN',
    "-I$src", "-I$(Join-Path $kernel 'include')", "-I$(Join-Path $kernel 'portable\MSVC-MingW')"
)
$sources = @(
    (Join-Path $src 'main.c'), (Join-Path $src 'bms_core.c'),
    (Join-Path $src 'sim_afe.c'), (Join-Path $src 'protocol.c'),
    (Join-Path $src 'line_ring.c'),
    (Join-Path $kernel 'list.c'), (Join-Path $kernel 'queue.c'),
    (Join-Path $kernel 'tasks.c'),
    (Join-Path $kernel 'portable\MSVC-MingW\port.c'),
    (Join-Path $kernel 'portable\MemMang\heap_4.c')
)
$exe = Join-Path $build 'bms_sim.exe'
& $zig cc @flags @sources '-lwinmm' '-lm' '-o' $exe
if ($LASTEXITCODE -ne 0) { throw '仿真程序构建失败。' }
Write-Host "已构建 $exe"

if ($Tests) {
    $testExe = Join-Path $build 'test_core.exe'
    & $zig cc '-target' 'x86_64-windows-gnu' '-std=c11' '-O2' "-I$src" `
        (Join-Path $src 'bms_core.c') (Join-Path $src 'sim_afe.c') `
        (Join-Path $root 'tests\test_core.c') '-lm' '-o' $testExe
    if ($LASTEXITCODE -ne 0) { throw '核心测试构建失败。' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw '核心测试失败。' }
    & py -3.13 -m unittest discover -s (Join-Path $root 'tests') -p 'test_protocol.py' -v
    if ($LASTEXITCODE -ne 0) { throw '协议集成测试失败。' }
    $boardCodec = Join-Path $root 'board\stm32f103\User\Bms\Core'
    & $zig cc '-target' 'x86_64-windows-gnu' '-std=c11' '-O2' ('-I' + $boardCodec) `
        (Join-Path $boardCodec 'telemetry_frame.c') (Join-Path $root 'tests\board_frame_vector.c') `
        '-o' (Join-Path $build 'board_frame_vector.exe')
    if ($LASTEXITCODE -ne 0) { throw '板端串口编码器测试程序构建失败。' }
    & py -3.13 -m unittest discover -s (Join-Path $root 'tests') -p 'test_serial.py' -v
    if ($LASTEXITCODE -ne 0) { throw '串口协议与界面测试失败。' }
    & py -3.13 (Join-Path $root 'tests\test_gui.py')
    if ($LASTEXITCODE -ne 0) { throw '界面联调测试失败。' }
}
