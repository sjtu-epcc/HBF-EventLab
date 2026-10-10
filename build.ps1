param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 C++ tools not found.' }
# Match the generator below; -latest without a version range can select newer BuildTools.
$vsInstall = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio 2022 C++ tools not found.' }
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakeExe = $cmakeCommand.Source }
else {
    $cmakeExe = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$buildDir = Join-Path $PSScriptRoot 'build'
& $cmakeExe -S $PSScriptRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsInstall"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmakeExe --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (!$SkipTests) {
    $ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
    & $ctestExe --test-dir $buildDir -C $Configuration --output-on-failure --timeout 30
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& (Join-Path $buildDir "$Configuration\hbf_eventlab_demo.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $buildDir "$Configuration\hbf_eventlab_device_demo.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $buildDir "$Configuration\hbf_eventlab_cli.exe") (Join-Path $PSScriptRoot "examples\workloads\normal.txt")
exit $LASTEXITCODE
