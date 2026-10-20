param([switch]$Smoke)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root 'build.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& py -3.13 -c 'import PySide6, PySide6.QtCharts, PySide6.QtSerialPort'
if ($LASTEXITCODE -ne 0) {
    throw '缺少 PySide6。请先执行 py -3.13 -m pip install -r requirements.txt。'
}
$guiArgs = @((Join-Path $root 'desktop\app.py'), '--sim', (Join-Path $root 'build\bms_sim.exe'))
if ($Smoke) { $guiArgs += '--smoke' }
& py -3.13 @guiArgs
exit $LASTEXITCODE
