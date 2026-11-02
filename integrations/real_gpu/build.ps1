param(
 [ValidateSet('cpu','cuda','model')][string]$Target='cpu',
 [string]$Config,[string]$CudaRoot,[string]$CuptiRoot,[string]$Python,[int]$Jobs,
 [string]$CudaArchitectures,[string]$PtxArchitecture,[string]$BuildDir,
 [string]$BackendSource,[ValidateSet('ON','OFF')][string]$EnableInjection,
 [string]$VsRoot,[string]$CMake,[string]$Ninja,[switch]$PrintConfiguration
)
$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
# Defaults are frontend-relative; config paths are config-relative; CLI paths are cwd-relative.
$taskSettings=@{cuda_root=$env:CUDA_PATH;cupti_root='';python='python';jobs=6;cuda_architectures='native';ptx_architecture='compute_75';build_dir=(Join-Path $taskRoot "build-$Target");backend_source=[IO.Path]::GetFullPath((Join-Path $taskRoot '../..'));enable_injection='';vs_root='';cmake='';ninja=''}
if($Config){
 $taskConfigPath=(Resolve-Path -LiteralPath $Config).Path
 $taskConfig=Get-Content -LiteralPath $taskConfigPath -Raw | ConvertFrom-Json
 foreach($taskProperty in $taskConfig.PSObject.Properties){
  if(!$taskSettings.ContainsKey($taskProperty.Name)){throw "Unknown machine setting: $($taskProperty.Name)"}
  if($null -eq $taskProperty.Value){throw "Null machine setting: $($taskProperty.Name)"}
  $taskSettings[$taskProperty.Name]=$taskProperty.Value
 }
 foreach($taskKey in @('cuda_root','cupti_root','build_dir','backend_source','vs_root','cmake','ninja','python')){
  $taskValue=[string]$taskSettings[$taskKey]
  if($taskValue -and ($taskKey -notin @('cmake','ninja','python') -or $taskValue -match '[/\\]') -and ![IO.Path]::IsPathRooted($taskValue)){
   $taskSettings[$taskKey]=[IO.Path]::GetFullPath((Join-Path (Split-Path $taskConfigPath) $taskValue))
  }
 }
}
$taskOverrides=@{CudaRoot='cuda_root';CuptiRoot='cupti_root';Python='python';Jobs='jobs';CudaArchitectures='cuda_architectures';PtxArchitecture='ptx_architecture';BuildDir='build_dir';BackendSource='backend_source';EnableInjection='enable_injection';VsRoot='vs_root';CMake='cmake';Ninja='ninja'}
foreach($taskKey in $taskOverrides.Keys){if($PSBoundParameters.ContainsKey($taskKey)){$taskSettings[$taskOverrides[$taskKey]]=$PSBoundParameters[$taskKey]}}
if([string]::IsNullOrWhiteSpace([string]$taskSettings.backend_source)){throw 'backend_source must not be empty'}
$taskSettings.backend_source=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath([string]$taskSettings.backend_source)
if($taskSettings.cupti_root){$taskSettings.cupti_root=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath([string]$taskSettings.cupti_root)}
if($taskSettings.enable_injection -notin @('','ON','OFF')){throw 'enable_injection must be ON or OFF (or empty for the CMake default)'}
if([int]$taskSettings.jobs -lt 1){throw 'jobs must be positive'}
if($taskSettings.cuda_architectures -notmatch '^(native|all|all-major|[0-9]+[af]?(-(real|virtual))?(;[0-9]+[af]?(-(real|virtual))?)*)$'){throw 'Invalid cuda_architectures'}
if($taskSettings.ptx_architecture -notmatch '^compute_[0-9]+[af]?$'){throw 'Invalid ptx_architecture'}
if($PrintConfiguration){$taskSettings | ConvertTo-Json;return}
if(!$IsWindows -and $PSVersionTable.PSEdition -eq 'Core'){throw 'build.ps1 requires Windows; use CMake directly on other platforms (see docs/ENVIRONMENT.md)'}
$taskVs=$taskSettings.vs_root
if(!$taskVs){
 $taskVswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
 $taskVs=& $taskVswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if(!$taskVs){throw 'Set vs_root or install Visual Studio 2022 C++ build tools'}
$env:VSLANG='1033'
& (Join-Path $taskVs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if(!$taskSettings.cmake){$taskSettings.cmake=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'}
if(!$taskSettings.ninja){$taskSettings.ninja=Join-Path $taskVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'}
$taskBuild=[IO.Path]::GetFullPath($taskSettings.build_dir)
$taskSource=$taskRoot
$taskArgs=@('-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=ON','-DHBFLAB_BACKEND_TESTS=ON',"-DHBFLAB_EVENTLAB_SOURCE:PATH=$($taskSettings.backend_source)","-DPython3_EXECUTABLE=$($taskSettings.python)","-DCMAKE_MAKE_PROGRAM=$($taskSettings.ninja)")
if($Target -eq 'cpu'){
 $taskArgs+=@('-DHBFLAB_ENABLE_CUDA=OFF','-DHBF_EVENTLAB_EXPERIMENT_TESTS=ON')
}else{
 if($taskSettings.enable_injection){$taskArgs+="-DHBFLAB_ENABLE_INJECTION=$($taskSettings.enable_injection)"}
 if(!$taskSettings.cuda_root){throw 'Set cuda_root in the machine config, CUDA_PATH, or -CudaRoot'}
 $taskCuda=[IO.Path]::GetFullPath($taskSettings.cuda_root)
 $env:CUDA_PATH=$taskCuda;$env:CUDACXX=Join-Path $taskCuda 'bin/nvcc.exe'
 $env:PATH=(Join-Path $taskCuda 'bin')+';'+(Join-Path $taskCuda 'bin/x64')+';'+$env:PATH
 $taskArgs+=@('-DHBFLAB_ENABLE_CUDA=ON',"-DCMAKE_CUDA_ARCHITECTURES=$($taskSettings.cuda_architectures)","-DHBFLAB_PTX_ARCHITECTURE=$($taskSettings.ptx_architecture)","-DCUDAToolkit_ROOT=$taskCuda")
}
if($Target -eq 'model'){
 $taskSource=Join-Path $taskRoot 'adapters/llama_cpp/integrated'
 if($taskSettings.cupti_root){$taskArgs+="-DHBFLAB_CUPTI_ROOT:PATH=$($taskSettings.cupti_root)"}
 if(Test-Path (Join-Path $taskBuild 'release-identity.json')){throw 'Model build is sealed; choose a fresh build_dir'}
}
& $taskSettings.cmake -S $taskSource -B $taskBuild -G Ninja @taskArgs
if($LASTEXITCODE -ne 0){throw 'Configure failed'}
& $taskSettings.cmake --build $taskBuild --parallel $taskSettings.jobs
if($LASTEXITCODE -ne 0){throw 'Build failed'}
if($Target -eq 'model'){
 & $taskSettings.python (Join-Path $taskRoot 'tools/integrated_build_identity.py') seal --build-dir $taskBuild
 if($LASTEXITCODE -ne 0){throw 'Build identity failed'}
}
