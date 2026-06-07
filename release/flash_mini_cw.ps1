param(
    [string]$Port = "COM11",
    [int]$Baud = 460800,
    [string]$BinPath = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BinPath)) {
    $BinPath = Join-Path $PSScriptRoot "MiniCW_V1_1.bin"
}

if (-not (Test-Path -LiteralPath $BinPath -PathType Leaf)) {
    throw "Merged binary not found: $BinPath"
}
$ResolvedBin = Resolve-Path $BinPath

$Python = $null
if ($env:IDF_PYTHON_ENV_PATH) {
    $Candidate = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
        $Python = $Candidate
    }
}

if (-not $Python) {
    $Candidate = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
    if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
        $Python = $Candidate
    }
}

if (-not $Python) {
    $PythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCommand) {
        $Python = $PythonCommand.Source
    }
}

if (-not $Python) {
    throw "Python not found. Run ESP-IDF export.ps1 first or install Python with esptool."
}

$EsptoolArgs = @(
    "-m", "esptool",
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", "$Baud",
    "--before", "default_reset",
    "--after", "hard_reset",
    "write_flash",
    "--flash_mode", "dio",
    "--flash_size", "8MB",
    "--flash_freq", "80m",
    "0x0", $ResolvedBin.Path
)

Write-Host "Flashing Mini-CW V1.1 merged image"
Write-Host "Port: $Port"
Write-Host "Baud: $Baud"
Write-Host "Image: $($ResolvedBin.Path)"
Write-Host "Python: $Python"

if ($DryRun) {
    Write-Host "Dry run command:"
    Write-Host "& `"$Python`" $($EsptoolArgs -join ' ')"
    exit 0
}

& $Python @EsptoolArgs
exit $LASTEXITCODE
