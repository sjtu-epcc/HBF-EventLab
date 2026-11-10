param([ValidateSet('F1','F2','F3')][string]$Start='F1', [Parameter(Mandatory=$true)][string]$BatchDir, [Parameter(Mandatory=$true)][string]$BaseBuild, [Parameter(Mandatory=$true)][string]$BuildDir, [Parameter(Mandatory=$true)][string]$Model, [Parameter(Mandatory=$true)][string]$CudaRoot)
$ErrorActionPreference = 'Stop'
$BaseBuild=(Resolve-Path -LiteralPath $BaseBuild).Path
$BuildDir=(Resolve-Path -LiteralPath $BuildDir).Path
$BatchDir=(Resolve-Path -LiteralPath $BatchDir).Path
$Model=(Resolve-Path -LiteralPath $Model).Path
$CudaRoot=(Resolve-Path -LiteralPath $CudaRoot).Path
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $repo

$batch = (Resolve-Path -LiteralPath $BatchDir).Path
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$reference = Join-Path $batch 'F1'
$stages = @('F1','F2','F3')
$passed = @()
function Save-Status($state, $active, $detail) {
    [ordered]@{ status=$state; stage=$active; passed=$passed; detail=$detail; updated=(Get-Date).ToString('o') } |
        ConvertTo-Json -Depth 4 | Set-Content (Join-Path $batch 'status.json')
}
function Invoke-Python($log, [string[]]$arguments) {
    & python @arguments *> (Join-Path $batch $log)
    if ($LASTEXITCODE) { throw "Python exited $LASTEXITCODE. See $log" }
}
$current = $Start
try {
    Save-Status 'preflight' $current ''
    Invoke-Python 'preflight.log' @('tools/integrated_build_identity.py','verify','--build-dir',$BaseBuild)
    $begin = [Array]::IndexOf($stages,$Start)
    for ($index = 0; $index -lt $begin; $index++) {
        $prior = $stages[$index]
        $result = Get-Content (Join-Path $batch "$prior/result.json") -Raw | ConvertFrom-Json
        $audit = Get-Content (Join-Path $batch "$prior/negative-checks.json") -Raw | ConvertFrom-Json
        if ($result.status -ne 'pass' -or $audit.status -ne 'pass') { throw "Missing passed audit for $prior" }
        $passed += $prior
    }
    for ($index = $begin; $index -lt $stages.Count; $index++) {
        $current = $stages[$index]
        $destination = Join-Path $batch $current
        Save-Status 'running' $current 'GPU inference and HBF simulation'
        $argv = @('experiments/gpu_ffn/run.py','--stage',$current,'--tokens','8','--model',$Model,
                  '--build-dir',$build,'--cuda-root',$CudaRoot,'--output',$destination,'--timeout','21600')
        if ($current -ne 'F1') { $argv += @('--reference',$reference) }
        Invoke-Python "$current-run.log" $argv
        Save-Status 'auditing' $current 'Offline audit and corrupt-record rejection'
        Invoke-Python "$current-audit.log" @('experiments/gpu_ffn/validate.py','--output',$destination,'--negative-checks')
        $result = Get-Content (Join-Path $destination 'result.json') -Raw | ConvertFrom-Json
        $audit = Get-Content (Join-Path $destination 'negative-checks.json') -Raw | ConvertFrom-Json
        if ($result.status -ne 'pass' -or $audit.status -ne 'pass' -or $result.G3.actual_outputs -ne 8) {
            throw "Formal 8-token acceptance failed for $current"
        }
        $passed += $current
    }
    $rows = foreach ($current in $stages) {
        $directory = Join-Path $batch $current
        $result = Get-Content (Join-Path $directory 'result.json') -Raw | ConvertFrom-Json
        $cost = $result.software_costs.$current
        [ordered]@{ stage=$current; model=$result.coverage; compute_ps=$result.compute_ps; wait_ps=$result.total_wait_ps;
            G2=$result.G2; G3=$result.G3; events=$result.events_processed; online_audit=$result.online_audit;
            process_wall_seconds=$cost.process_wall_seconds; peak_working_set_bytes=$cost.peak_working_set_bytes;
            table_sha256=(Get-FileHash (Join-Path $directory 'compute-table.json') -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    if (@($rows.table_sha256 | Select-Object -Unique).Count -ne 1) { throw 'Compute table hashes differ' }
    [ordered]@{ status='pass'; stages=@($rows) } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $batch 'summary.json')
    Save-Status 'pass' $null 'F1, F2 and F3 passed'
} catch {
    Save-Status 'failed' $current $_.Exception.Message
    throw
}
