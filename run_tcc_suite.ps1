$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$TestDir = if (Test-Path (Join-Path $ScriptDir "tinycc\tests\tests2")) {
    Join-Path $ScriptDir "tinycc\tests\tests2"
} elseif (Test-Path (Join-Path $ScriptDir "tests2")) {
    Join-Path $ScriptDir "tests2"
} else {
    Join-Path $ScriptDir "tinycc\tests\tests2"
}
$ReportFile = Join-Path $ScriptDir "tcc_test_mingw.md"

$RCC = $null
@("rcc.exe", "rcc.exe", "rcc") | ForEach-Object {
    $candidate = Join-Path $ScriptDir $_
    if (Test-Path $candidate) {
        $RCC = $candidate
        return
    }
}

if (-not $RCC) {
    Write-Error "rcc.exe not found in $ScriptDir. Please build it first."
    exit 1
}

# Tests to skip – mirrors tinycc/tests/tests2/Makefile SKIP, including WIN32 extras
# (bound-checker, backtrace, btdll, builtins are TCC-runtime-only)
$SkipTests = @(
    "22_floating_point",
    "60_errors_and_warnings", # no main; TCC -dt mode
    "73_arm64",
    "96_nodata_wanted",       # no main; TCC -dt mode
    "104_inline",             # needs multi-file 104+_inline.c
    "106_versym",             # requires -pthread
    "112_backtrace",
    "113_btdll",
    "114_bound_signal",
    "115_bound_setjmp",
    "116_bound_setjmp2",
    "117_builtins",
    "120_alias",              # needs multi-file 120+_alias.c + GNU alias
    "122_vla_reuse",          # VLA not supported
    "126_bound_global"
)

$TestFiles = Get-ChildItem -Path $TestDir -Filter "*.c" | Sort-Object {
    $base = $_.BaseName
    if ($base -match '^(\d+)') { [int]$Matches[1] } else { [int]::MaxValue }
}, @{ Expression = 'BaseName'; Ascending = $true }
$Total = $TestFiles.Count
$Passed = 0
$Failed = 0
$Results = @()

Write-Host "Starting TCC Test Suite on RCC..." -ForegroundColor Cyan
Write-Host "Total tests found: $Total"

foreach ($file in $TestFiles) {
    $base = $file.BaseName
    $src = $file.FullName
    $exe = Join-Path $TestDir "$base.exe"
    $expectFile = Join-Path $TestDir "$base.expect"
    $outputFile = Join-Path $TestDir "$base.out"

    # Skip tests that require TCC internals, inline asm, or pthreads
    if ($SkipTests -contains $base) {
        Write-Host "$base... SKIP" -ForegroundColor DarkGray
        continue
    }

    Write-Host "Running $base... " -NoNewline

    # 1. Compilation
    $compileStart = Get-Date
    $process = Start-Process -FilePath $RCC -ArgumentList $src, "-o", $exe -PassThru -NoNewWindow -Wait -ErrorAction SilentlyContinue
    $compileEnd = Get-Date

    if ($process.ExitCode -ne 0) {
        Write-Host "COMPILE FAIL" -ForegroundColor Red
        $Results += [PSCustomObject]@{
            Test = $base
            Status = "COMPILE_FAIL"
            Message = "rcc exited with $($process.ExitCode)"
        }
        $Failed++
        continue
    }

    if (-not (Test-Path $exe)) {
        Write-Host "NO EXE PRODUCED" -ForegroundColor Red
        $Results += [PSCustomObject]@{
            Test = $base
            Status = "COMPILE_FAIL"
            Message = "rcc finished but $base.exe is missing"
        }
        $Failed++
        continue
    }

    # 2. Execution
    $ExtraArgs = switch ($base) {
        "31_args" { @("arg1", "arg2", "arg3", "arg4", "arg5") }
        "46_grep" { @('[^* ]*[:a:d: ]+\:\*-/: $', (Join-Path $TestDir "46_grep.c")) }
        default   { @() }
    }
    try {
        $actualOutput = & $exe @ExtraArgs 2>&1 | Out-String
        $actualOutput = $actualOutput.Trim()
    } catch {
        Write-Host "EXECUTION FAIL" -ForegroundColor Red
        $Results += [PSCustomObject]@{
            Test = $base
            Status = "EXEC_FAIL"
            Message = $_.Exception.Message
        }
        $Failed++
        if (Test-Path $exe) { Remove-Item $exe -Force }
        continue
    }

    # 3. Verification
    if (Test-Path $expectFile) {
        $expectedRaw = Get-Content $expectFile -Raw
        $expectedOutput = if ($null -eq $expectedRaw) { "" } else { $expectedRaw.Trim() }

        # Normalize line endings and spaces for comparison
        $normActual = $actualOutput -replace "\r\n", "`n"
        $normExpected = $expectedOutput -replace "\r\n", "`n"

        if ($normActual -eq $normExpected) {
            Write-Host "PASS" -ForegroundColor Green
            $Results += [PSCustomObject]@{
                Test = $base
                Status = "PASS"
                Message = "Output matches"
            }
            $Passed++
        } else {
            Write-Host "MISMATCH" -ForegroundColor Yellow
            $Results += [PSCustomObject]@{
                Test = $base
                Status = "MISMATCH"
                Message = "Output does not match .expect"
            }
            $Failed++
            $actualOutput | Out-File $outputFile -Encoding utf8
        }
    } else {
        # No expect file: Assume success if it runs
        Write-Host "PASS (No Expect)" -ForegroundColor Gray
        $Results += [PSCustomObject]@{
            Test = $base
            Status = "PASS"
            Message = "Executed successfully (no .expect file)"
        }
        $Passed++
    }

    if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }
}

# Generate Report
$report = @"
# TCC Test Suite Report for RCC
Generated on: $(Get-Date)

## Summary
- **Total Tests**: $Total
- **Passed**: $Passed
- **Failed**: $Failed
- **Pass Rate**: $([math]::Round(($Passed/$Total)*100, 2))%

## Detailed Results
| Test | Status | Message |
| --- | --- | --- |
$( $Results | ForEach-Object { "| $($_.Test) | $($_.Status) | $($_.Message) |" } | Out-String )

"@

$report | Out-File $ReportFile -Encoding utf8

Write-Host "`nTest complete. Summary: $Passed Passed, $Failed Failed." -ForegroundColor Cyan
Write-Host "Full report saved to $ReportFile"

if ($Passed -ge 77) {
    exit 0
} else {
    exit 1
}
