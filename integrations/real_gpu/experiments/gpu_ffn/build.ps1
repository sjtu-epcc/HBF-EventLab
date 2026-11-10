param(
 [Parameter(Mandatory=$true)][string]$CudaRoot,
 [string]$BuildDir=(Join-Path $PSScriptRoot '../../build-ffn'),
 [string]$BaseBuild=(Join-Path $PSScriptRoot '../../build-model'),
 [string]$CuptiRoot="",
 [string]$Python="python",
 [int]$Jobs=6
)
$ErrorActionPreference='Stop'
if($Jobs -lt 1){throw 'Jobs must be positive'}
$taskVswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs=& $taskVswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$taskVs){throw 'Visual Studio 2022 C++ build tools not found'}
$env:VSLANG='1033'
& (Join-Path $taskVs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$taskBase=(Resolve-Path -LiteralPath $BaseBuild).Path
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskPython=$Python
& $taskPython (Join-Path $taskRoot 'tools/integrated_build_identity.py') verify --build-dir $taskBase
if($LASTEXITCODE -ne 0){throw 'Dependency identity check failed'}
$taskCuda=(Resolve-Path -LiteralPath $CudaRoot).Path
$env:CUDA_PATH=$taskCuda
$env:CUDACXX=Join-Path $taskCuda 'bin/nvcc.exe'
$env:PATH=(Join-Path $taskCuda 'bin')+';'+(Join-Path $taskCuda 'bin/x64')+';'+$env:PATH
$taskCmake=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskNinja=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$taskExtra=@()
if($CuptiRoot){$taskExtra += "-DHBFLAB_CUPTI_ROOT=$((Resolve-Path -LiteralPath $CuptiRoot).Path)"}
$taskPyPath=(& $taskPython -c 'import sys; print(sys.executable)')
& $taskCmake -S $PSScriptRoot -B $BuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DBASE_BUILD=$taskBase" "-DCUDAToolkit_ROOT=$taskCuda" "-DCMAKE_MAKE_PROGRAM=$taskNinja" "-DPython3_EXECUTABLE=$taskPyPath" @taskExtra
if($LASTEXITCODE -ne 0){throw 'Configure failed'}
& $taskCmake --build $BuildDir --target gpu_ffn ffn_audit_tests ffn_bridge_smoke --parallel $Jobs
if($LASTEXITCODE -ne 0){throw 'Build failed'}
& (Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe') --test-dir $BuildDir --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Online audit tests failed'}

Copy-Item -Path (Join-Path $taskBase 'bin/*.dll') -Destination (Join-Path $BuildDir 'bin')

& $taskPython (Join-Path $PSScriptRoot "ffn_identity.py") seal --build $BuildDir
if($LASTEXITCODE -ne 0){throw "Experiment seal failed"}
