param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$MfcRoot = '',
    [string]$BuildDirectory = 'build',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $projectRoot $BuildDirectory }
$configureArgs = @('-S', $projectRoot, '-B', $buildRoot, '-G', 'Visual Studio 17 2022', '-A', 'Win32')
if ($MfcRoot) { $configureArgs += "-DRCS_ATLMFC_ROOT=$MfcRoot" }
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (-not $SkipTests) {
    & ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
Write-Host "Binaries: $buildRoot/bin/$Configuration"
