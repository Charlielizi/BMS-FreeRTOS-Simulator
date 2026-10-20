param([string]$ToolchainBin = $env:BMS_ARM_GCC_BIN)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$board = Join-Path $root 'board\stm32f103'
$kernel = Join-Path $root 'third_party\FreeRTOS-Kernel'
$out = Join-Path $root 'build\board'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not $ToolchainBin) {
    $found = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($found) { $ToolchainBin = Split-Path -Parent $found.Source }
}
if (-not $ToolchainBin -and $env:USERPROFILE) {
    $userToolchain = Join-Path $env:USERPROFILE '.codex\toolchains\arm-gnu-toolchain-14.3.rel1\bin'
    if (Test-Path -LiteralPath (Join-Path $userToolchain 'arm-none-eabi-gcc.exe')) {
        $ToolchainBin = $userToolchain
    }
}
if (-not $ToolchainBin) {
    $local = Get-ChildItem -Path (Join-Path $root 'build\tools') -Filter arm-none-eabi-gcc.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($local) { $ToolchainBin = $local.DirectoryName }
}
if (-not $ToolchainBin -or -not (Test-Path -LiteralPath (Join-Path $ToolchainBin 'arm-none-eabi-gcc.exe'))) {
    throw '需要 Arm GNU Toolchain；将 bin 目录加入 PATH，或设置 BMS_ARM_GCC_BIN。'
}
$gcc = Join-Path $ToolchainBin 'arm-none-eabi-gcc.exe'
$objcopy = Join-Path $ToolchainBin 'arm-none-eabi-objcopy.exe'
$size = Join-Path $ToolchainBin 'arm-none-eabi-size.exe'

$include = @(
    'Core\Inc', 'Drivers\STM32F1xx_HAL_Driver\Inc',
    'Drivers\STM32F1xx_HAL_Driver\Inc\Legacy',
    'Drivers\CMSIS\Device\ST\STM32F1xx\Include', 'Drivers\CMSIS\Include',
    'User', 'User\Bms', 'User\Bms\Core', 'User\Bms\Hal', 'User\Drivers',
    'CMSIS_RTOS_V2'
) | ForEach-Object { '-I' + (Join-Path $board $_) }
$include += '-I' + (Join-Path $kernel 'include')
$include += '-I' + (Join-Path $kernel 'portable\GCC\ARM_CM3')
$flags = @('-mcpu=cortex-m3','-mthumb','-std=gnu11','-Os','-g0','-ffunction-sections',
           '-fdata-sections','-fno-common','-DUSE_HAL_DRIVER','-DSTM32F103xB') + $include

$boardFiles = @(
    'Core/Src/main.c','Core/Src/gpio.c','Core/Src/freertos.c','Core/Src/can.c',
    'Core/Src/usart.c','Core/Src/stm32f1xx_it.c','Core/Src/stm32f1xx_hal_msp.c',
    'Core/Src/stm32f1xx_hal_timebase_tim.c','Core/Src/system_stm32f1xx.c',
    'User/Bms/bms_app.c',
    'User/Bms/Core/bms_analysis.c','User/Bms/Core/bms_energy.c',
    'User/Bms/Core/bms_global.c','User/Bms/Core/bms_monitor.c',
    'User/Bms/Core/bms_protect.c','User/Bms/Core/bms_utils.c',
    'User/Bms/Core/telemetry.c','User/Bms/Core/telemetry_frame.c',
    'User/Bms/Hal/bms_hal_comm.c',
    'User/Bms/Hal/bms_hal_config.c','User/Bms/Hal/bms_hal_control.c',
    'User/Bms/Hal/bms_hal_monitor.c','User/Drivers/drv_soft_i2c.c',
    'User/Drivers/drv_softi2c_bq769x0.c','CMSIS_RTOS_V2/cmsis_os2.c'
)
$halFiles = @('stm32f1xx_hal_gpio_ex.c','stm32f1xx_hal_can.c','stm32f1xx_hal.c',
    'stm32f1xx_hal_rcc.c','stm32f1xx_hal_rcc_ex.c','stm32f1xx_hal_gpio.c',
    'stm32f1xx_hal_dma.c','stm32f1xx_hal_cortex.c','stm32f1xx_hal_pwr.c',
    'stm32f1xx_hal_flash.c','stm32f1xx_hal_flash_ex.c','stm32f1xx_hal_exti.c',
    'stm32f1xx_hal_tim.c','stm32f1xx_hal_tim_ex.c','stm32f1xx_hal_uart.c') |
    ForEach-Object { Join-Path $board ('Drivers\STM32F1xx_HAL_Driver\Src\' + $_) }
$kernelFiles = @('list.c','queue.c','tasks.c','timers.c','event_groups.c','stream_buffer.c') |
    ForEach-Object { Join-Path $kernel $_ }
$kernelFiles += Join-Path $kernel 'portable\GCC\ARM_CM3\port.c'
$kernelFiles += Join-Path $kernel 'portable\MemMang\heap_4.c'
$sources = @($boardFiles | ForEach-Object { Join-Path $board $_ }) + $halFiles + $kernelFiles
$objects = @()
$index = 0
foreach ($source in $sources) {
    $object = Join-Path $out ('obj_' + $index + '.o')
    & $gcc @flags '-c' $source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "编译失败：$source" }
    $objects += $object
    $index++
}
$startup = Join-Path $out 'startup.o'
& $gcc '-mcpu=cortex-m3' '-mthumb' '-x' 'assembler-with-cpp' '-c' (Join-Path $board 'startup_stm32f103xb.s') '-o' $startup
if ($LASTEXITCODE -ne 0) { throw '启动文件编译失败' }
$objects += $startup
$elf = Join-Path $out 'bms_board.elf'
$map = Join-Path $out 'bms_board.map'
& $gcc '-mcpu=cortex-m3' '-mthumb' '-Os' @objects `
    ('-T' + (Join-Path $board 'STM32F103C8Tx_FLASH.ld')) `
    '-Wl,--gc-sections' ('-Wl,-Map=' + $map) '-specs=nano.specs' '-specs=nosys.specs' `
    '-Wl,--print-memory-usage' '-Wl,--start-group' '-lc' '-lm' '-lgcc' '-Wl,--end-group' '-o' $elf
if ($LASTEXITCODE -ne 0) { throw '板端链接失败' }
$bin = Join-Path $out 'bms_board.bin'
& $objcopy '-O' 'binary' $elf $bin
if ($LASTEXITCODE -ne 0) { throw 'BIN 导出失败' }
$sizeReport = & $size '-B' $elf
if ($LASTEXITCODE -ne 0) { throw 'size 检查失败' }
$sizeReport | Set-Content -LiteralPath (Join-Path $out 'size.txt') -Encoding UTF8
$sizeReport | Write-Host
Write-Host "板端固件：$elf"
Write-Host "烧录镜像：$bin"

