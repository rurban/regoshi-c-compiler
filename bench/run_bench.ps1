$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir   = Split-Path -Parent $ScriptDir
Set-Location $ScriptDir

$SRC     = Join-Path $ScriptDir "bench.c"
$RCC     = Join-Path $RootDir "rcc.exe"
$GCC     = "gcc"

# Discover TCC: prefer installed copy, else submodule with include paths
$TCC_INSTALL = "C:/Program Files/tcc/tcc.exe"
$TCC_SUBMODULE = Join-Path $RootDir "tinycc\tcc.exe"
$TCC = $null
$TCC_ARGS = ""
if (Test-Path $TCC_INSTALL) {
    $TCC = $TCC_INSTALL
} elseif (Test-Path $TCC_SUBMODULE) {
    $TCC = $TCC_SUBMODULE
    $win32inc = Join-Path $RootDir "tinycc\win32\include"
    $tccinc = Join-Path $RootDir "tinycc\include"
    $TCC_ARGS = "-I `"$win32inc`" -I `"$tccinc`""
}

$RCC_EXE = Join-Path $ScriptDir "bench_rcc.exe"
$TCC_EXE = Join-Path $ScriptDir "bench_tcc.exe"
$GCC_EXE = Join-Path $ScriptDir "bench_gcc.exe"
$GCC_O2  = Join-Path $ScriptDir "bench_gcc_o2.exe"

Write-Host ""
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "  RCC vs TCC vs GCC  --  Benchmark Battle"     -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host ""

# --- Helper ---
function Run-Bench {
    param($Label, $Compiler, [string]$ArgStr, $ExePath, [string]$Color = "White")

    Write-Host "--- $Label ---" -ForegroundColor $Color

    # Compile
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $Compiler -ArgumentList $ArgStr -PassThru -NoNewWindow -Wait -ErrorAction SilentlyContinue
    $sw.Stop()
    $compileMs = $sw.ElapsedMilliseconds

    if ($proc.ExitCode -ne 0 -or -not (Test-Path $ExePath)) {
        Write-Host "  COMPILE FAILED (exit=$($proc.ExitCode))" -ForegroundColor Red
        return $null
    }
    Write-Host ("  Compile : {0,6} ms" -f $compileMs) -ForegroundColor DarkGray

    # Execute (measure 3 runs, take best)
    $best = [long]::MaxValue
    $output = ""
    for ($i = 0; $i -lt 3; $i++) {
        $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
        $output = & $ExePath 2>&1 | Out-String
        $sw2.Stop()
        if ($sw2.ElapsedMilliseconds -lt $best) {
            $best = $sw2.ElapsedMilliseconds
        }
    }
    Write-Host ("  Execute : {0,6} ms  (best of 3)" -f $best) -ForegroundColor $Color
    Write-Host ("  Total   : {0,6} ms" -f ($compileMs + $best)) -ForegroundColor $Color

    # Cleanup
    if (Test-Path $ExePath) { Remove-Item $ExePath -Force -ErrorAction SilentlyContinue }

    return [PSCustomObject]@{
        Label     = $Label
        Compile   = $compileMs
        Execute   = $best
        Total     = $compileMs + $best
        Output    = $output.Trim()
    }
}

# --- Compile & Run ---
$results = @()

$r = Run-Bench "RCC (your compiler)" $RCC "$SRC -o $RCC_EXE" $RCC_EXE "Yellow"
if ($r) { $results += $r }
Write-Host ""

if ($TCC) {
    $r = Run-Bench "TCC (Tiny C Compiler)" $TCC "$SRC $TCC_ARGS -o $TCC_EXE" $TCC_EXE "Green"
    if ($r) { $results += $r }
    Write-Host ""
} else {
    Write-Host "TCC not found, skipping TCC benchmark" -ForegroundColor DarkGray
    Write-Host ""
    $results += [PSCustomObject]@{
        Label     = "TCC (Tiny C Compiler)"
        Compile   = "SKIP"
        Execute   = "SKIP"
        Total     = "SKIP"
        Output    = ""
    }
}

$r = Run-Bench "GCC -O0 (no opt)" $GCC "-O0 $SRC -o $GCC_EXE -lm" $GCC_EXE "Magenta"
if ($r) { $results += $r }
Write-Host ""

$r = Run-Bench "GCC -O2 (optimized)" $GCC "-O2 $SRC -o $GCC_O2 -lm" $GCC_O2 "Magenta"
if ($r) { $results += $r }
Write-Host ""

# --- Scoreboard ---
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "               SCOREBOARD"                     -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host ("{0,-25} {1,10} {2,10} {3,10}" -f "Compiler", "Compile", "Execute", "Total")
Write-Host ("{0,-25} {1,10} {2,10} {3,10}" -f "--------", "-------", "-------", "-----")
foreach ($r in $results) {
    Write-Host ("{0,-25} {1,8} ms {2,8} ms {3,8} ms" -f $r.Label, $r.Compile, $r.Execute, $r.Total)
}

# --- Head-to-head ---
Write-Host ""
$rcc_r = $results | Where-Object { $_.Label -like "RCC*" }
$tcc_r = $results | Where-Object { $_.Label -like "TCC*" }
if ($rcc_r -and $tcc_r) {
    Write-Host "--- RCC vs TCC Head-to-Head ---" -ForegroundColor Cyan
    $compile_ratio = if ($rcc_r.Compile -gt 0 -and $tcc_r.Compile -gt 0) { [math]::Round($rcc_r.Compile / $tcc_r.Compile, 2) } else { "N/A" }
    $exec_ratio    = if ($tcc_r.Execute  -gt 0) { [math]::Round($rcc_r.Execute  / $tcc_r.Execute, 2)  } else { "N/A" }
    $total_ratio   = if ($tcc_r.Total    -gt 0) { [math]::Round($rcc_r.Total    / $tcc_r.Total, 2)    } else { "N/A" }

    Write-Host "  Compile speed : RCC/TCC = ${compile_ratio}x"
    Write-Host "  Execute speed : RCC/TCC = ${exec_ratio}x"
    Write-Host "  Total         : RCC/TCC = ${total_ratio}x"
    Write-Host ""
    if ($exec_ratio -ne "N/A" -and $exec_ratio -lt 1.0) {
        Write-Host "  >>> RCC generates FASTER code than TCC! <<<" -ForegroundColor Yellow
    } elseif ($exec_ratio -ne "N/A" -and $exec_ratio -gt 1.0) {
        Write-Host "  >>> TCC generates faster code this time. <<<" -ForegroundColor Green
    } else {
        Write-Host "  >>> Dead heat! <<<" -ForegroundColor White
    }
}

# --- Correctness check ---
Write-Host ""
Write-Host "--- Output Correctness ---" -ForegroundColor Cyan
$ref = ($results | Where-Object { $_.Label -like "GCC -O2*" })
if ($ref) {
    foreach ($r in $results) {
        if ($r.Output -eq $ref.Output) {
            Write-Host "  $($r.Label): OK" -ForegroundColor Green
        } else {
            Write-Host "  $($r.Label): OUTPUT DIFFERS" -ForegroundColor Red
        }
    }
}
Write-Host ""

# --- Report ---
$ReportFile = Join-Path $ScriptDir "bench_report_mingw.md"
$reportLines = @()
$reportLines += "# Windows RCC Benchmark Results"
$reportLines += ""
$reportLines += "_Generated: $(Get-Date)_"
$reportLines += ""
$reportLines += "| Compiler | Compile (ms) | Execute (ms) | Total (ms) |"
$reportLines += "| :------- | -----------: | -----------: | ---------: |"
foreach ($r in $results) {
    $reportLines += "| $($r.Label) | $($r.Compile) | $($r.Execute) | $($r.Total) |"
}
$reportLines += ""
if ($rcc_r -and $tcc_r) {
    $reportLines += "## Windows RCC vs TCC Head-to-Head"
    $reportLines += ""
    $reportLines += "- Compile speed : RCC/TCC = ${compile_ratio}x"
    $reportLines += "- Execute speed : RCC/TCC = ${exec_ratio}x"
    $reportLines += "- Total         : RCC/TCC = ${total_ratio}x"
    $reportLines += ""
}
$reportLines += "## Output Correctness"
$reportLines += ""
if ($ref) {
    foreach ($r in $results) {
        $status = if ($r.Output -eq $ref.Output) { "OK" } else { "OUTPUT DIFFERS" }
        $reportLines += "- $($r.Label): $status"
    }
}
$reportLines += ""
$report = ($reportLines | Out-String) -replace "`r`n", "`n" -replace "`r", "`n"
if (-not $report.EndsWith("`n")) { $report += "`n" }
[System.IO.File]::WriteAllText($ReportFile, $report, [System.Text.UTF8Encoding]::new($false))
Write-Host "Report saved to $ReportFile" -ForegroundColor Cyan
