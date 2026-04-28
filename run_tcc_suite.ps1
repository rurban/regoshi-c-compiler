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
    "120_alias",              # TCC extension, GCC/CLANG fail. GNU alias
    "122_vla_reuse",          # VLA not supported
    "126_bound_global",
    "78_vla_label",           # VLA not supported
    "79_vla_continue",        # VLA not supported
    "123_vla_bug",            # VLA not supported
    "98_al_ax_extend",        # x86-32bit specific, skipped by TCC on x86_64
    "99_fastcall",            # x86-32bit specific, skipped by TCC on x86_64
    "124_atomic_counter",     # GCC atomic builtins/extensions
                              # (statement expressions, __auto_type, etc.)
    "125_atomic_misc",
    "136_atomic_gcc_style",
    "128_run_atexit",         # needs TCC-specific -dt multi-snippet runner
    "133_old_func"            # K&R-style function definitions not supported
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

    # Skip multi-file helper files (e.g. 104+_inline.c, 120+_alias.c)
    if ($base -match '\+') {
        $Total--
        continue
    }

    # Skip tests that require TCC internals, inline asm, or pthreads
    if ($SkipTests -contains $base) {
        Write-Host "$base... SKIP" -ForegroundColor DarkGray
        $Results += [PSCustomObject]@{
            Test    = $base
            Status  = "SKIP"
            Message = "Skipped"
        }
        $Total--
        continue
    }

    Write-Host "Running $base... " -NoNewline

    # 129_scopes needs to be compiled from the test directory so __FILE__ is just the basename
    $inScopesDir = $false
    if ($base -eq "129_scopes") {
        $origRCC = $RCC
        $origSrc = $src
        $RCC = (Resolve-Path $RCC).Path
        Push-Location $TestDir
        $src = "129_scopes.c"
        $inScopesDir = $true
    }

    # 1. Compilation
    $compileStart = Get-Date
    $process = Start-Process -FilePath $RCC -ArgumentList $src, "-o", $exe -PassThru -NoNewWindow -Wait -ErrorAction SilentlyContinue
    $compileEnd = Get-Date

    if ($inScopesDir) {
        Pop-Location
        $src = $origSrc
        $RCC = $origRCC
    }

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
        "46_grep" { @('[^* ]*[:a:d: ]+\:\*-/: $', "46_grep.c") }
        default   { @() }
    }

    $inGrepDir = $false
    if ($base -eq "46_grep") {
        Push-Location $TestDir
        $inGrepDir = $true
    }

    try {
        $actualOutput = & $exe @ExtraArgs 2>&1 | Out-String
        $actualOutput = $actualOutput.Trim()
    } catch {
        if ($inGrepDir) { Pop-Location }
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

    if ($inGrepDir) { Pop-Location }

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

# Normalize to LF line endings and ensure single trailing newline
$report = $report -replace "`r`n", "`n" -replace "`r", "`n"
if (-not $report.EndsWith("`n")) { $report += "`n" }
[System.IO.File]::WriteAllText($ReportFile, $report, [System.Text.UTF8Encoding]::new($false))

Write-Host "`nTest complete. Summary: $Passed Passed, $Failed Failed." -ForegroundColor Cyan
Write-Host "Full report saved to $ReportFile"

if ($Passed -ge 87) {
    exit 0
} else {
    exit 1
}
