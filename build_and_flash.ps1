param([string]$Port = "")
$ErrorActionPreference = "Stop"
$idfRoot = "C:\esp-idf-v6.0.2"
if (-not (Test-Path "$idfRoot\tools\idf.py")) { throw "ESP-IDF v6.0.2 wurde unter $idfRoot nicht gefunden." }
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& "$idfRoot\export.ps1"
$python = (Get-Command python).Source
$version = (& $python "$idfRoot\tools\idf.py" --version) -join " "
if ($version -notmatch "v6\.0\.2") { throw "ESP-IDF v6.0.2 erwartet. Gefunden: $version" }
Set-Location $PSScriptRoot
Remove-Item .\sdkconfig -ErrorAction SilentlyContinue
& $python "$idfRoot\tools\idf.py" fullclean
& $python "$idfRoot\tools\idf.py" set-target esp32s3
& $python "$idfRoot\tools\idf.py" build
if ($Port) {
    & $python "$idfRoot\tools\idf.py" -p $Port flash monitor
} else {
    & $python "$idfRoot\tools\idf.py" flash monitor
}
