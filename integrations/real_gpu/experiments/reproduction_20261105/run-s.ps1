param([ValidateSet('S5-zero','S5-fixed','S5-audit','S6','S6-report','S6-qualify','S7','S8','S8-negative','S8-decomposition','S9','S9-audit')][string]$Start='S5-zero', [Parameter(Mandatory=$true)][string]$BatchDir, [Parameter(Mandatory=$true)][string]$BaseBuild, [Parameter(Mandatory=$true)][string]$BuildDir)
$ErrorActionPreference='Stop'

$batch = (Resolve-Path -LiteralPath $BatchDir).Path
$env:GPU_S_BATCHES = Join-Path $batch 'batches.json'
$env:GPU_S_BASE_BUILD = (Resolve-Path -LiteralPath $BaseBuild).Path
$env:GPU_S_BUILD = (Resolve-Path -LiteralPath $BuildDir).Path
$cfg = Get-Content $env:GPU_S_BATCHES -Raw | ConvertFrom-Json -AsHashtable
foreach($key in @($cfg.Keys)) { $cfg[$key] = [IO.Path]::GetFullPath((Join-Path $batch $cfg[$key])) }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $repo
python tools/integrated_build_identity.py verify --build-dir $env:GPU_S_BASE_BUILD
if($LASTEXITCODE){throw 'Dependency build identity failed'}
python experiments/gpu_s/build_identity.py verify --build $env:GPU_S_BUILD
if($LASTEXITCODE){throw 'Experiment build identity failed'}
$script:started=$false
function Stage($name, [string[]]$params) {
    if($name -eq $Start){$script:started=$true}
    if(!$script:started){return}
    Write-Output "Starting $name"
    & python @params *> "$batch/$name.log"
    if($LASTEXITCODE){throw "$name failed. See $batch/$name.log"}
    Write-Output "$name command complete"
}
Stage 'S5-zero' @('experiments/gpu_s/s5_zero.py','--s4',$cfg.s4,'--s2',$cfg.s2,'--study',$cfg['s5-zero'])
Stage 'S5-fixed' @('experiments/gpu_s/s5_fixed.py','--study',$cfg.s5)
Stage 'S5-audit' @('experiments/gpu_s/s5_validate.py','--study',$cfg.s5)
Stage 'S6' @('experiments/gpu_s/s6.py')
Stage 'S6-report' @('experiments/gpu_s/s6_report.py')
Stage 'S6-qualify' @('experiments/gpu_s/s6_qualify_minimum.py')
Stage 'S7' @('experiments/gpu_s/s7.py','--study',$cfg.s7)
Stage 'S8' @('experiments/gpu_s/s8.py','--study',$cfg.s8)
Stage 'S8-negative' @('experiments/gpu_s/s8_validate.py','--study',$cfg.s8)
Stage 'S8-decomposition' @('experiments/gpu_s/s8_decomposition.py','--study',$cfg.s8)
Stage 'S9' @('experiments/gpu_s/s9.py','--study',(Join-Path $batch 's9'))
Stage 'S9-audit' @('experiments/gpu_s/s9_validate.py','--study',(Join-Path $batch 's9'))
