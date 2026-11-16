param(
    [ValidateSet('Version', 'SetTarget', 'Build', 'Flash', 'Monitor')]
    [string]$Action = 'Build',
    [string]$Port = 'COM6'
)
$ErrorActionPreference = 'Stop'
# Local SDK/tools discovered during bring-up. No SDK installation is modified.
$sdkRoot = 'D:\esp\v6.1\esp-idf'
$pythonEnv = 'D:\Espressif\tools\python\v6.1\venv'
$installedTools = 'D:\Espressif\tools'
$taskRoot = Join-Path $env:USERPROFILE '.cache\codex-s3-module-lab'
$toolRoot = Join-Path $taskRoot 'idf-env'
$buildRoot = Join-Path $taskRoot 'build'
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $installedTools 'espidf.constraints.v6.1.txt') -Destination $toolRoot
$toolLink = Join-Path $toolRoot 'tools'
if (!(Test-Path -LiteralPath $toolLink)) {
    New-Item -ItemType Junction -Path $toolLink -Target $installedTools | Out-Null
}
$env:PYTHONUTF8 = '1'
$env:IDF_TOOLS_PATH = $toolRoot
$env:IDF_PYTHON_ENV_PATH = $pythonEnv
$env:Path = 'D:\Espressif\tools\python;' + $env:Path
& (Join-Path $sdkRoot 'export.ps1')
if (!$?) { throw 'ESP-IDF activation failed' }
# idf-exe --version reports its wrapper version. Invoke the actual SDK idf.py.
$pythonExe = Join-Path $pythonEnv 'Scripts\python.exe'
$idfScript = Join-Path $sdkRoot 'tools\idf.py'
$idfArgs = @('-C', $PSScriptRoot, '-B', $buildRoot, '--no-ccache')
switch ($Action) {
    Version   { $idfArgs += '--version' }
    SetTarget { $idfArgs += @('set-target', 'esp32s3') }
    Build     { $idfArgs += 'build' }
    Flash     { $idfArgs += @('-p', $Port, 'flash') }
    Monitor   { $idfArgs += @('-p', $Port, 'monitor') }
}
& $pythonExe $idfScript @idfArgs
if ($LASTEXITCODE -ne 0) { throw "idf.py failed: exit $LASTEXITCODE" }
