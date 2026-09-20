# Collective Traffic Fabric - closure validation.
# Copyright 2026 Summon Software Labs.
#
# Runs the complete release proof chain from a repository checkout:
#   Release build -> Debug build -> every suite -> install to a clean prefix ->
#   independent find_package consumer -> optional fresh clone of a tag.
#
# Usage:
#   pwsh -File validate/run-closure-validation.ps1
#   pwsh -File validate/run-closure-validation.ps1 -SkipFreshClone
#   pwsh -File validate/run-closure-validation.ps1 -Tag v1.0.0 -Remote <url>
#
# No test timeout is set anywhere in this script.  A suite that hangs is a defect
# to diagnose, and this script must not hide it.
param(
  [switch]$SkipFreshClone,
  [string]$Tag = "",
  [string]$Remote = "",
  [string]$WorkRoot = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($WorkRoot)) { $WorkRoot = Join-Path $repo "build-closure" }
$binRoot = Join-Path $WorkRoot "bin"
$prefix = Join-Path $WorkRoot "install"
$summary = Join-Path $WorkRoot "closure-summary.txt"
$results = New-Object System.Collections.Generic.List[string]
$failures = 0

function Step([string]$name, [scriptblock]$body) {
  Write-Host ""
  Write-Host "=== $name" -ForegroundColor Cyan
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    & $body
    $sw.Stop()
    $line = "PASS  {0}  ({1:N1}s)" -f $name, $sw.Elapsed.TotalSeconds
    $script:results.Add($line)
    Write-Host $line -ForegroundColor Green
  } catch {
    $sw.Stop()
    $script:failures++
    $line = "FAIL  {0}  ({1:N1}s)  {2}" -f $name, $sw.Elapsed.TotalSeconds, $_.Exception.Message
    $script:results.Add($line)
    Write-Host $line -ForegroundColor Red
    throw
  }
}

function Invoke-Checked([string]$program, [string[]]$arguments) {
  & $program @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$program exited with $LASTEXITCODE"
  }
}

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
New-Item -ItemType Directory -Force -Path $binRoot | Out-Null

$suites = @(
  "ctf_unit_core", "ctf_unit_persistence", "ctf_property_engine",
  "ctf_adversarial_decoder", "ctf_adversarial_persistence",
  "ctf_concurrency_service", "ctf_concurrency_authority",
  "ctf_protocol_framing", "ctf_integration_loopback", "ctf_integration_lifecycle",
  "ctf_persistence_restart", "ctf_multiprocess_fabric", "ctf_scale_fabric"
)

function Build-And-Test([string]$config, [string]$buildDir) {
  Step "configure $config" {
    Invoke-Checked "cmake" @("-S", $repo, "-B", $buildDir, "-G", "Visual Studio 17 2022", "-A", "x64",
                              "-DCTF_BUILD_TESTS=ON", "-DCTF_BUILD_TOOLS=ON", "-DCTF_BUILD_EXAMPLES=ON")
  }
  Step "build $config" {
    # Two passes on purpose: on a freshly generated MSBuild tree a parallel build
    # can reach a test link before the library archive exists.  The second pass is
    # a no-op once the archive is there.
    Invoke-Checked "cmake" @("--build", $buildDir, "--config", $config, "--parallel")
    Invoke-Checked "cmake" @("--build", $buildDir, "--config", $config, "--parallel")
  }
  foreach ($suite in $suites) {
    Step "suite $config $suite" {
      $exe = Join-Path $buildDir "tests\bin\$config\$suite.exe"
      if (-not (Test-Path $exe)) { throw "missing test executable $exe" }
      Invoke-Checked $exe @()
    }
  }
}

Step "repository hygiene check" {
  $porcelain = & git -C $repo status --porcelain
  $untracked = $porcelain | Where-Object { $_ -match "^\?\?" }
  Write-Host "working tree entries: $($porcelain.Count) (untracked: $($untracked.Count))"
}

Build-And-Test "Release" (Join-Path $WorkRoot "release")
Build-And-Test "Debug" (Join-Path $WorkRoot "debug")

Step "install to a clean prefix" {
  if (Test-Path $prefix) { Remove-Item -Recurse -Force $prefix }
  Invoke-Checked "cmake" @("--install", (Join-Path $WorkRoot "release"), "--config", "Release", "--prefix", $prefix)
  $header = Join-Path $prefix "include\ctf\decision.hpp"
  if (-not (Test-Path $header)) { throw "installed headers are missing" }
  $library = Get-ChildItem -Path (Join-Path $prefix "lib") -Filter "collective_traffic_fabric*" -ErrorAction SilentlyContinue
  if (-not $library) { throw "installed library is missing" }
  $config = Join-Path $prefix "lib\cmake\CollectiveTrafficFabric\CollectiveTrafficFabricConfig.cmake"
  if (-not (Test-Path $config)) { throw "installed package configuration is missing" }
}

Step "independent downstream consumer via find_package" {
  $consumerBuild = Join-Path $WorkRoot "consumer-build"
  Invoke-Checked "cmake" @("-S", (Join-Path $repo "validate\downstream"), "-B", $consumerBuild,
                            "-G", "Visual Studio 17 2022", "-A", "x64",
                            "-DCMAKE_PREFIX_PATH=$prefix")
  Invoke-Checked "cmake" @("--build", $consumerBuild, "--config", "Release", "--parallel")
  $consumerExe = Join-Path $consumerBuild "Release\ctf_consumer.exe"
  if (-not (Test-Path $consumerExe)) { throw "the consumer executable was not produced" }
  Invoke-Checked $consumerExe @()
}

Step "examples and tools against the build tree" {
  $example = Join-Path $WorkRoot "release\bin\Release\ctf_example_decision_types.exe"
  if (-not (Test-Path $example)) { throw "the example executable was not produced" }
  Invoke-Checked $example @()
  $ctfctl = Join-Path $WorkRoot "release\bin\Release\ctfctl.exe"
  if (-not (Test-Path $ctfctl)) { throw "the ctfctl tool was not produced" }
  Invoke-Checked $ctfctl @("selftest")
}

if (-not $SkipFreshClone) {
  if ([string]::IsNullOrEmpty($Tag)) { throw "-Tag is required unless -SkipFreshClone is given" }
  if ([string]::IsNullOrEmpty($Remote)) { throw "-Remote is required unless -SkipFreshClone is given" }
  $cloneRoot = Join-Path $WorkRoot "fresh-clone"
  Step "fresh clone of $Tag" {
    if (Test-Path $cloneRoot) { Remove-Item -Recurse -Force $cloneRoot }
    Invoke-Checked "git" @("clone", "--branch", $Tag, "--depth", "1", $Remote, $cloneRoot)
  }
  Step "fresh clone: build, test, install, consume" {
    $cloneBuild = Join-Path $cloneRoot "build"
    Invoke-Checked "cmake" @("-S", $cloneRoot, "-B", $cloneBuild, "-G", "Visual Studio 17 2022", "-A", "x64",
                              "-DCTF_BUILD_TESTS=ON")
    Invoke-Checked "cmake" @("--build", $cloneBuild, "--config", "Release", "--parallel")
    Invoke-Checked "cmake" @("--build", $cloneBuild, "--config", "Release", "--parallel")
    foreach ($suite in $suites) {
      $exe = Join-Path $cloneBuild "tests\bin\Release\$suite.exe"
      if (-not (Test-Path $exe)) { throw "fresh clone is missing $suite" }
      Invoke-Checked $exe @()
    }
    $clonePrefix = Join-Path $cloneRoot "install"
    Invoke-Checked "cmake" @("--install", $cloneBuild, "--config", "Release", "--prefix", $clonePrefix)
    $cloneConsumer = Join-Path $cloneRoot "consumer"
    Invoke-Checked "cmake" @("-S", (Join-Path $cloneRoot "validate\downstream"), "-B", $cloneConsumer,
                              "-G", "Visual Studio 17 2022", "-A", "x64",
                              "-DCMAKE_PREFIX_PATH=$clonePrefix")
    Invoke-Checked "cmake" @("--build", $cloneConsumer, "--config", "Release", "--parallel")
    Invoke-Checked (Join-Path $cloneConsumer "Release\ctf_consumer.exe") @()
  }
}

$results | Set-Content -Path $summary
Write-Host ""
Write-Host "closure validation complete: $($results.Count) steps, $failures failures" -ForegroundColor Green
Write-Host "summary: $summary"
exit 0