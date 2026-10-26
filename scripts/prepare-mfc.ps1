#requires -Version 5.1
<#
.SYNOPSIS
Extract the official VS 2022 v143 MFC/ATL Win32 build files without installing them.
.DESCRIPTION
Uses the installed Visual Studio catalog for Microsoft's payload URLs and SHA-256
hashes. Downloads are archives only; no installer or downloaded script is run.
The existing Visual Studio installation is never modified. The selected payloads
provide Unicode Release static MFC (uafxcw.lib) and ATL for an x86 /MT build.
Microsoft's component licenses still apply; do not commit these files to Git.
.PARAMETER CatalogPath
Optional path to an installed VS 2022 catalog.json containing the pinned 14.44
payloads. When omitted, the script locates the installed Build Tools catalog.
.PARAMETER Destination
Extraction directory. Defaults to the repository's .deps/atlmfc directory.
#>
[CmdletBinding()]
param(
    [string]$CatalogPath,
    [string]$Destination = (Join-Path $PSScriptRoot '..\.deps\atlmfc')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Find-InstalledCatalog {
    $cacheRoots = @()
    foreach ($key in @('HKLM:\SOFTWARE\Microsoft\VisualStudio\Setup', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\Setup')) {
        $settings = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($settings -and $settings.CachePath) { $cacheRoots += $settings.CachePath }
    }
    $cacheRoots += Join-Path $env:ProgramData 'Microsoft\VisualStudio\Packages'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $instanceId = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property instanceId
        if ($instanceId) {
            foreach ($cacheRoot in ($cacheRoots | Select-Object -Unique)) {
                $candidate = Join-Path $cacheRoot (Join-Path '_Instances' (Join-Path ($instanceId.Trim()) 'catalog.json'))
                if (Test-Path -LiteralPath $candidate) { return $candidate }
            }
        }
    }
    throw 'Cannot find an installed Visual Studio catalog. Pass -CatalogPath <catalog.json>, or use Visual Studio Installer to add the v143 MFC and ATL components.'
}

if (-not $CatalogPath) { $CatalogPath = Find-InstalledCatalog }
if (-not (Test-Path -LiteralPath $CatalogPath -PathType Leaf)) {
    throw "Catalog not found: $CatalogPath. Pass -CatalogPath, or add MFC/ATL using Visual Studio Installer."
}

# These are real build payloads, not the small dependency-only wrapper packages.
$packageIds = @(
    'Microsoft.VC.14.44.17.14.ATL.Headers.base',
    'Microsoft.VC.14.44.17.14.ATL.X86.base',
    'Microsoft.VC.14.44.17.14.MFC.Headers.base',
    'Microsoft.VC.14.44.17.14.MFC.X86.base'
)
$archivePrefix = 'Contents/VC/Tools/MSVC/14.44.35207/atlmfc/'
$catalog = Get-Content -LiteralPath $CatalogPath -Raw -Encoding UTF8 | ConvertFrom-Json
$packages = foreach ($id in $packageIds) {
    $matches = @($catalog.packages | Where-Object { $_.id -eq $id })
    if ($matches.Count -ne 1 -or @($matches[0].payloads).Count -ne 1) {
        throw "Catalog does not contain the expected payload for $id. Supply a matching VS 2022 14.44 catalog, or install MFC/ATL through Visual Studio Installer."
    }
    $matches[0]
}

$destinationRoot = [IO.Path]::GetFullPath($Destination)
$destinationPrefix = $destinationRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$downloadRoot = Join-Path ([IO.Directory]::GetParent($destinationRoot).FullName) 'mfc-downloads'
[IO.Directory]::CreateDirectory($destinationRoot) | Out-Null
[IO.Directory]::CreateDirectory($downloadRoot) | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$manifestPackages = @()

foreach ($package in $packages) {
    $payload = $package.payloads[0]
    $uri = [Uri]$payload.url
    if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'download.visualstudio.microsoft.com') {
        throw "Unexpected payload origin for $($package.id): $uri"
    }
    if ($payload.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or [IO.Path]::GetFileName($payload.fileName) -ne $payload.fileName) {
        throw "Invalid payload metadata for $($package.id)."
    }

    $archivePath = Join-Path $downloadRoot $payload.fileName
    $validCache = (Test-Path -LiteralPath $archivePath) -and ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -eq $payload.sha256)
    if (-not $validCache) {
        Write-Host "Downloading $($payload.fileName) from Microsoft..."
        $partialPath = $archivePath + '.download'
        Invoke-WebRequest -Uri $uri.AbsoluteUri -UseBasicParsing -OutFile $partialPath -TimeoutSec 180
        if ((Get-FileHash -LiteralPath $partialPath -Algorithm SHA256).Hash -ne $payload.sha256) {
            throw "SHA-256 verification failed for $($payload.fileName); the archive will not be extracted."
        }
        Move-Item -LiteralPath $partialPath -Destination $archivePath -Force
    }
    # Some signed VSIX sizes differ from catalog size; SHA-256 is the integrity check.
    Write-Host "Verified $($payload.fileName); extracting MFC/ATL files..."
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    $extractedCount = 0
    try {
        foreach ($entry in $archive.Entries) {
            $entryName = [Uri]::UnescapeDataString($entry.FullName.Replace('\', '/'))
            if (-not $entryName.StartsWith($archivePrefix, [StringComparison]::OrdinalIgnoreCase) -or $entryName.EndsWith('/')) { continue }
            $relativePath = $entryName.Substring($archivePrefix.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
            $targetPath = [IO.Path]::GetFullPath((Join-Path $destinationRoot $relativePath))
            if (-not $targetPath.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe archive entry: $entryName"
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($targetPath)) | Out-Null
            $inputStream = $entry.Open()
            try {
                $outputStream = [IO.File]::Create($targetPath)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
            $extractedCount++
        }
    } finally { $archive.Dispose() }
    if ($extractedCount -eq 0) { throw "No expected MFC/ATL files were found in $($payload.fileName)." }
    $manifestPackages += [pscustomobject]@{
        id = $package.id
        version = $package.version
        url = $payload.url
        sha256 = $payload.sha256
        extractedFiles = $extractedCount
    }
}

foreach ($requiredFile in @('include\afxwin.h', 'include\afxres.h', 'include\atlbase.h', 'lib\x86\uafxcw.lib', 'lib\x86\atls.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $destinationRoot $requiredFile) -PathType Leaf)) {
        throw "The extracted dependency set is incomplete: $requiredFile"
    }
}
[pscustomobject]@{
    toolset = 'v143 / 14.44.35207'
    platform = 'Win32'
    configuration = 'Unicode Release; static MFC; /MT'
    catalog = [IO.Path]::GetFullPath($CatalogPath)
    packages = $manifestPackages
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destinationRoot 'payload-manifest.json') -Encoding UTF8
Write-Host "MFC/ATL is ready: $destinationRoot"
Write-Host 'Set RCS_ATLMFC_ROOT to this directory when configuring CMake. No VS installation was changed.'
