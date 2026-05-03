# SPDX-License-Identifier: LGPL-2.1-or-later
param(
    [switch] $O1
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# Ensure UTF-8 source files (e.g. 83_utf8_in_identifiers) are decoded correctly
# when capturing child-process output.  Windows defaults to the OEM code page
# (CP437/CP850/CP1252) which misinterprets raw UTF-8 bytes.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding          = [System.Text.Encoding]::UTF8

Write-Host "OutputEncoding : $([Console]::OutputEncoding.EncodingName)  (CodePage $([Console]::OutputEncoding.CodePage))" -ForegroundColor DarkCyan
Write-Host "chcp           : $(& chcp)" -ForegroundColor DarkCyan

$RCCFLAGS = ""
if ($O1) { $RCCFLAGS = "-O1" }

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
    "125_atomic_misc",        # Requires -dt
    "126_bound_global",
    "98_al_ax_extend",        # x86-32bit specific, skipped by TCC on x86_64
    "99_fastcall",            # x86-32bit specific, skipped by TCC on x86_64
    "128_run_atexit"          # needs TCC-specific -dt multi-snippet runner
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

    # Apply local fixups for tinycc tests2 expect files.
    # If test/tinycc-<base>.expect exists and differs from the upstream
    # expect, overwrite the upstream copy.
    $fixupFile = Join-Path (Join-Path $ScriptDir "test") "tinycc-$base.expect"
    $upstreamExpect = Join-Path $TestDir "$base.expect"
    $fixedUp = $false
    if (Test-Path $fixupFile) {
        $fixupContent = Get-Content $fixupFile -Raw -Encoding UTF8
        $upstreamContent = Get-Content $upstreamExpect -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        if ($fixupContent -ne $upstreamContent) {
            Copy-Item $upstreamExpect "$upstreamExpect.orig" -Force
            Copy-Item $fixupFile $upstreamExpect -Force
            $fixedUp = $true
        }
    }

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

    $extraFlags = ""
    if ($base -eq "129_scopes") {
        $extraFlags = "-D__TINYC__"
    }

    # 1. Compilation - capture stderr (warnings) to mirror Linux runner behaviour
    $compileStart = Get-Date
    $tmpErr = [System.IO.Path]::GetTempFileName()
    $argList = @()
    if ($RCCFLAGS) { $argList += $RCCFLAGS }
    if ($extraFlags) { $argList += $extraFlags }
    $argList += $src
    $argList += "-o"
    $argList += $exe
    $process = Start-Process -FilePath $RCC -ArgumentList $argList -PassThru -Wait -RedirectStandardError $tmpErr -ErrorAction SilentlyContinue
    $compileStdErr = if (Test-Path $tmpErr) { Get-Content $tmpErr -Raw -ErrorAction SilentlyContinue } else { $null }
    Remove-Item $tmpErr -Force -ErrorAction SilentlyContinue
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

    $execFailed = $false
    $execFailMsg = ""
    try {
        $actualOutput = & $exe @ExtraArgs 2>&1 | Out-String
        # Non-zero exit code means the test program itself crashed/failed.
        # & $exe never throws for non-zero exit; check $LASTEXITCODE explicitly.
        if ($LASTEXITCODE -ne 0) {
            $execFailed = $true
            $execFailMsg = "exited with code $LASTEXITCODE"
        }
        # Prepend compile-time warnings (stderr) so output matches Linux runner format
        if ($compileStdErr) { $actualOutput = $compileStdErr + $actualOutput }
        $actualOutput = $actualOutput.Trim()
    } catch {
        $execFailed = $true
        $execFailMsg = $_.Exception.Message
    }

    if ($inGrepDir) { Pop-Location }

    if ($execFailed) {
        Write-Host "EXECUTION FAIL" -ForegroundColor Red
        # Emit assembly so we can see what the compiler generated
        $asmFile = Join-Path $TestDir "$base.s"
        $asmArgs = @(); if ($RCCFLAGS) { $asmArgs += $RCCFLAGS }
        $asmArgs += "-S"; $asmArgs += $src; $asmArgs += "-o"; $asmArgs += $asmFile
        $null = Start-Process -FilePath $RCC -ArgumentList $asmArgs -PassThru -Wait -ErrorAction SilentlyContinue
        if (Test-Path $asmFile) {
            Write-Host "  --- assembly ($base.s) ---" -ForegroundColor DarkCyan
            Get-Content $asmFile | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkCyan }
            Remove-Item $asmFile -Force -ErrorAction SilentlyContinue
        }
        $Results += [PSCustomObject]@{
            Test    = $base
            Status  = "EXEC_FAIL"
            Message = $execFailMsg
        }
        $Failed++
        if (Test-Path $exe) { Remove-Item $exe -Force }
        continue
    }

    # 3. Verification
    $expectFile = if (Test-Path $fixupFile) { $fixupFile } else { $upstreamExpect }
    if (Test-Path $expectFile) {
        $expectedRaw = Get-Content $expectFile -Raw -Encoding UTF8
        $expectedOutput = if ($null -eq $expectedRaw) { "" } else { $expectedRaw.Trim() }

        # Normalize: CRLF→LF, strip trailing whitespace per line
        $normActual   = (($actualOutput   -replace "\r\n", "`n") -split "`n" | ForEach-Object { $_.TrimEnd() }) -join "`n"
        $normExpected = (($expectedOutput -replace "\r\n", "`n") -split "`n" | ForEach-Object { $_.TrimEnd() }) -join "`n"

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
            # Print diff for CI visibility
            $expectedLines = $normExpected -split "`n"
            $actualLines   = $normActual   -split "`n"
            $diff = Compare-Object -ReferenceObject $expectedLines -DifferenceObject $actualLines
            foreach ($line in $diff) {
                $prefix = if ($line.SideIndicator -eq '<=') { '-' } else { '+' }
                Write-Host "$prefix $($line.InputObject)"
            }
            $Results += [PSCustomObject]@{
                Test = $base
                Status = "MISMATCH"
                Message = "Output does not match .expect"
            }
            $Failed++
            $actualOutput | Out-File $outputFile -Encoding utf8

            # Diagnostics: hex dump of actual vs expected bytes (catches encoding bugs)
            $actBytes = [System.Text.Encoding]::UTF8.GetBytes($normActual)
            $expBytes = [System.Text.Encoding]::UTF8.GetBytes($normExpected)
            Write-Host "  actual  hex: $(($actBytes | Select-Object -First 48 | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')" -ForegroundColor DarkYellow
            Write-Host "  expect  hex: $(($expBytes | Select-Object -First 48 | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')" -ForegroundColor DarkYellow

            # Diagnostics: emit the assembly for string literals so we can see
            # whether the compiler encoded the bytes correctly.
            $asmFile = Join-Path $TestDir "$base.s"
            $asmArgs = @()
            if ($RCCFLAGS) { $asmArgs += $RCCFLAGS }
            $asmArgs += "-S"; $asmArgs += $src; $asmArgs += "-o"; $asmArgs += $asmFile
            $null = Start-Process -FilePath $RCC -ArgumentList $asmArgs -PassThru -Wait -ErrorAction SilentlyContinue
            if (Test-Path $asmFile) {
                $asmLines = Get-Content $asmFile | Where-Object { $_ -match '^\s*\.(byte|2byte|4byte|ascii|string|LC\d)' }
                if ($asmLines) {
                    Write-Host "  --- string data from $base.s ---" -ForegroundColor DarkCyan
                    $asmLines | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkCyan }
                }
                Remove-Item $asmFile -Force -ErrorAction SilentlyContinue
            }
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

    # Restore upstream expect if it was overwritten by a fixup
    if ($fixedUp) {
        $origExpect = "$upstreamExpect.orig"
        if (Test-Path $origExpect) {
            Copy-Item $origExpect $upstreamExpect -Force
            Remove-Item $origExpect -Force
        }
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

if ($Passed -ge 108) {
    exit 0
} else {
    exit 1
}
