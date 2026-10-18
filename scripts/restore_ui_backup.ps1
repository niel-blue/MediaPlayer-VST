param(
    [string]$BackupDirectory = 'E:\code_works\Media Player VST3\tmp\ui-backups\before-redesign-9e5be70cb255440b97af0b6ac4fbaf59',
    [switch]$RestoreSource
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$bundle = Join-Path $BackupDirectory 'DAW Media Player.vst3'
$target = 'E:\ProgramFiles\VST\DAW Media Player.vst3'
if (!(Test-Path -LiteralPath (Join-Path $bundle 'Contents\x86_64-win\DAW Media Player.vst3'))) {
    throw 'The selected backup does not contain a DAW Media Player VST3 bundle.'
}
# Preserve the version being replaced, so rollback is itself reversible.
$saved = Join-Path $workspace ('tmp\installed-backups\before-restore-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $saved | Out-Null
Copy-Item -LiteralPath $target -Destination $saved -Recurse -Force
Get-ChildItem -LiteralPath $bundle -Force | Copy-Item -Destination $target -Recurse -Force
foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -File -Force) {
    $relative = $file.FullName.Substring($bundle.Length + 1)
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $target $relative)).Hash) {
        throw ('Restored file differs from backup: ' + $relative)
    }
}
if ($RestoreSource) {
    foreach ($entry in @('src', 'tests', 'CMakeLists.txt', 'README.md')) {
        $source = Join-Path $BackupDirectory $entry
        if (!(Test-Path -LiteralPath $source)) { throw ('Missing source backup: ' + $entry) }
    }
    $sourceSaved = Join-Path $saved 'source'
    New-Item -ItemType Directory -Path $sourceSaved | Out-Null
    foreach ($entry in @('src', 'tests', 'CMakeLists.txt', 'README.md')) {
        $current = Join-Path $workspace $entry
        Copy-Item -LiteralPath $current -Destination $sourceSaved -Recurse -Force
        $source = Join-Path $BackupDirectory $entry
        if (Test-Path -LiteralPath $source -PathType Container) {
            Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $current -Recurse -Force
        } else {
            Copy-Item -LiteralPath $source -Destination $current -Force
        }
    }
}
Write-Output ('Previous UI restored. Replacement backup: ' + $saved)
