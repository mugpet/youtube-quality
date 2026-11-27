param(
    [string] $OutputPath,
    [switch] $TestBuild
)
$ErrorActionPreference = 'Stop'
if (!$OutputPath) {
    $filename = if ($TestBuild) { 'out\YouTubeQuality-test.exe' } else { 'YouTube Quality.exe' }
    $OutputPath = Join-Path $PSScriptRoot $filename
}
$output = [IO.Path]::GetFullPath($OutputPath)
$outputDir = [IO.Path]::GetDirectoryName($output)
$target = [IO.Path]::GetFileNameWithoutExtension($output)
$mode = if ($TestBuild) { 'test' } else { 'release' }
$intermediate = Join-Path $PSScriptRoot "out\obj\$mode"
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Visual Studio with the C++ desktop workload is required.' }
$properties = @('/p:Configuration=Release', '/p:Platform=x64', "/p:OutDir=$outputDir\",
    "/p:IntDir=$intermediate\", "/p:TargetName=$target", "/p:QualityTestBuild=$($TestBuild.IsPresent.ToString().ToLowerInvariant())")
& $msbuild (Join-Path $PSScriptRoot 'YouTubeQuality.vcxproj') /nologo /verbosity:minimal @properties
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $output)) { throw 'The native build failed.' }
