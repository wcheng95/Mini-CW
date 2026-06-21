param(
    [string]$IdfPath = "",
    [string]$IdfToolsPath = "",
    [string]$IdfPythonEnvPath = "",
    [string]$EspIdfVersion = "5.5.1",
    [string]$EspRomElfDir = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($IdfPath)) {
    $IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { "C:\esp-idf" }
}
if ([string]::IsNullOrWhiteSpace($IdfToolsPath)) {
    $IdfToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "C:\Espressif" }
}
if ([string]::IsNullOrWhiteSpace($IdfPythonEnvPath)) {
    $IdfPythonEnvPath = if ($env:IDF_PYTHON_ENV_PATH) {
        $env:IDF_PYTHON_ENV_PATH
    } else {
        Join-Path $IdfToolsPath "python_env\idf5.5_py3.11_env"
    }
}
if ([string]::IsNullOrWhiteSpace($EspRomElfDir)) {
    $RomRoot = Join-Path $IdfToolsPath "tools\esp-rom-elfs"
    if (Test-Path -LiteralPath $RomRoot -PathType Container) {
        $LatestRomDir = Get-ChildItem -LiteralPath $RomRoot -Directory |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($LatestRomDir) {
            $EspRomElfDir = $LatestRomDir.FullName
        }
    }
}
if ($IdfArgs.Count -eq 0) {
    $IdfArgs = @("build")
}

$Python = Join-Path $IdfPythonEnvPath "Scripts\python.exe"
$IdfPy = Join-Path $IdfPath "tools\idf.py"
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw "ESP-IDF Python not found: $Python"
}
if (-not (Test-Path -LiteralPath $IdfPy -PathType Leaf)) {
    throw "idf.py not found: $IdfPy"
}
if ([string]::IsNullOrWhiteSpace($EspRomElfDir) -or
    -not (Test-Path -LiteralPath $EspRomElfDir -PathType Container)) {
    throw "ESP ROM ELF directory not found. Pass -EspRomElfDir or install esp-rom-elfs."
}

function Add-PathIfPresent {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $env:PATH = "$Path;$env:PATH"
    }
}

Add-PathIfPresent (Join-Path $IdfToolsPath "tools\ninja\1.12.1")
Add-PathIfPresent (Join-Path $IdfToolsPath "tools\cmake\3.30.2\bin")
Add-PathIfPresent (Join-Path $IdfToolsPath "tools\idf-git\2.44.0\cmd")

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $IdfToolsPath
$env:IDF_PYTHON_ENV_PATH = $IdfPythonEnvPath
$env:ESP_IDF_VERSION = $EspIdfVersion
$env:ESP_ROM_ELF_DIR = $EspRomElfDir

Write-Host "ESP-IDF: $IdfPath"
Write-Host "ESP-IDF Python env: $IdfPythonEnvPath"
Write-Host "ESP-IDF version: $EspIdfVersion"
Write-Host "ESP ROM ELF dir: $EspRomElfDir"
Write-Host "Project: $ProjectRoot"
Write-Host "idf.py args: $($IdfArgs -join ' ')"

Push-Location $ProjectRoot
try {
    & $Python $IdfPy @IdfArgs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
