<#
.SYNOPSIS
    First-time setup for TTT2 Native Online: verifies YOUR copy of Tekken Tag Tournament 2
    (Xbox 360), copies its data into this folder and builds the native game on this PC.

.DESCRIPTION
    Nothing from the game is distributed with this project. Setup:
      1. finds your disc image (.iso) or extracted disc folder
      2. verifies every game file against config/game_versions.json while copying it to native\game
      3. downloads a portable compiler toolchain into native\toolchain (hash-checked)
      4. builds the recompiler tools, translates Default.xex to C++ and compiles the game
    Everything is written inside this folder. Nothing is installed on the system.

.PARAMETER Image
    Disc image (.iso) or folder containing Default.xex. Optional: setup looks in .\iso first
    and asks otherwise.
.PARAMETER Rebuild
    Keep the verified game data and only rebuild the code (after an update).
.PARAMETER VerifyOnly
    Check the image and stop.
#>
[CmdletBinding()]
param(
    [string]$Image,
    [switch]$Rebuild,
    [switch]$VerifyOnly,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'Continue'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Native = Join-Path $Root 'native'
$GameDir = Join-Path $Native 'game'
$BuildDir = Join-Path $Native 'build'
$ToolDir = Join-Path $Native 'toolchain'
$DownloadDir = Join-Path $Native 'downloads'
$BinDir = Join-Path $Native 'bin'
$LogFile = Join-Path $Native 'build.log'

New-Item -ItemType Directory -Force -Path $Native | Out-Null

# ------------------------------------------------------------------------------ helpers

function Write-Log([string]$Message, [string]$Color = 'Gray') {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line -ForegroundColor $Color
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

function Write-Step([string]$Message) { Write-Log '' ; Write-Log "== $Message" 'Cyan' }

$script:Reported = $false
function Fail([string]$Message) {
    Write-Log "ERROR: $Message" 'Red'
    Write-Log "Details are in $LogFile" 'Red'
    $script:Reported = $true
    throw $Message
}

# Runs a native program, mirrors its output to the log, fails on a non-zero exit code.
# (Windows PowerShell turns native stderr into error records, so the preference is relaxed here.)
function Invoke-Native([string]$Exe, [string[]]$Arguments, [string]$What) {
    Write-Log "> $Exe $($Arguments -join ' ')" 'DarkGray'
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            # stderr lines arrive as ErrorRecords; empty ones print as their type name
            $text = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
            if ($text -eq 'System.Management.Automation.RemoteException') { $text = '' }
            Add-Content -Path $LogFile -Value $text -Encoding UTF8
            if ($text -match 'error|warning: unresolved|FAILED') { Write-Host $text -ForegroundColor Yellow }
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    if ($code -ne 0) { Fail "$What failed (exit code $code)." }
}

# KEY=VALUE lines from .env; real environment variables win.
function Import-DotEnv([string]$Path) {
    if (-not (Test-Path $Path)) { return }
    foreach ($raw in Get-Content $Path) {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
        $key, $value = $line.Split('=', 2)
        $key = $key.Trim(); $value = $value.Trim().Trim('"')
        if (-not [Environment]::GetEnvironmentVariable($key)) {
            [Environment]::SetEnvironmentVariable($key, $value)
        }
    }
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return '{0:N1} GB' -f ($Bytes / 1GB) }
    return '{0:N0} MB' -f ($Bytes / 1MB)
}

# ------------------------------------------------------------------------------ start

Add-Content -Path $LogFile -Value "`r`n===== setup $(Get-Date -Format s) =====" -Encoding UTF8
Import-DotEnv (Join-Path $Root '.env')

if (-not [Environment]::Is64BitOperatingSystem) { Fail 'A 64-bit Windows is required.' }

$versions = (Get-Content (Join-Path $Root 'config\game_versions.json') -Raw | ConvertFrom-Json).versions
$toolchain = Get-Content (Join-Path $Root 'config\toolchain.json') -Raw | ConvertFrom-Json

Add-Type -Path (Join-Path $PSScriptRoot 'Xdvdfs.cs')
Add-Type -AssemblyName System.IO.Compression.FileSystem

Write-Host ''
Write-Host '  TTT2 Native Online - first-time setup' -ForegroundColor White
Write-Host '  Your own copy of the game is verified and compiled on this PC.' -ForegroundColor Gray
Write-Host ''

# ------------------------------------------------------------------------------ 1. find the image

function Find-Image {
    if ($Image) { return $Image }
    if ($env:TT2_IMAGE) { return $env:TT2_IMAGE }

    $isoDir = Join-Path $Root 'iso'
    if (Test-Path $isoDir) {
        $candidates = @(Get-ChildItem $isoDir -Filter *.iso -File -ErrorAction SilentlyContinue)
        $candidates += @(Get-ChildItem $isoDir -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName 'Default.xex') })
        if ($candidates.Count -eq 1) { return $candidates[0].FullName }
        if ($candidates.Count -gt 1) { Write-Log 'More than one image in .\iso - pick one.' 'Yellow' }
    }

    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your Tekken Tag Tournament 2 (Xbox 360) disc image, or Default.xex of an extracted disc'
    $dialog.Filter = 'Xbox 360 disc image (*.iso)|*.iso|Extracted disc (Default.xex)|Default.xex'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No disc image selected.' }
    if ([IO.Path]::GetFileName($dialog.FileName) -ieq 'Default.xex') { return [IO.Path]::GetDirectoryName($dialog.FileName) }
    return $dialog.FileName
}

# ------------------------------------------------------------------------------ 2. verify + copy

# Returns the matching version entry, copying (and hashing) every listed file into native\game.
function Copy-VerifiedGame([string]$Source) {
    $isFolder = Test-Path $Source -PathType Container
    $sources = @{}
    if ($isFolder) {
        foreach ($f in Get-ChildItem $Source -Recurse -File) {
            $rel = $f.FullName.Substring($Source.TrimEnd('\').Length + 1).Replace('\', '/')
            $sources[$rel.ToLowerInvariant()] = @{ Path = $f.FullName; Offset = 0; Size = $f.Length; Name = $rel }
        }
        Write-Log "Extracted disc folder: $Source"
    } else {
        $entries = [TT2Setup.Xdvdfs]::List($Source)
        foreach ($e in $entries) { $sources[$e.Path.ToLowerInvariant()] = @{ Path = $Source; Offset = $e.Offset; Size = $e.Size; Name = $e.Path } }
        $fs = [IO.File]::OpenRead($Source)
        try { $layout = [TT2Setup.Xdvdfs]::LayoutName([TT2Setup.Xdvdfs]::FindPartition($fs)) } finally { $fs.Close() }
        Write-Log "Disc image: $Source ($layout, $(Format-Size (Get-Item $Source).Length))"
    }

    # Pick the version whose file list matches by name and size before reading any data.
    $match = $null
    foreach ($v in $versions) {
        $ok = $true
        foreach ($p in $v.files.PSObject.Properties) {
            $s = $sources[$p.Name.ToLowerInvariant()]
            if (-not $s -or $s.Size -ne $p.Value.size) { $ok = $false; break }
        }
        if ($ok) { $match = $v; break }
    }
    if (-not $match) {
        Fail ('This does not look like a supported copy of the game. Supported: ' + (($versions | ForEach-Object { $_.name }) -join ', ') +
              '. Use an unmodified 1:1 dump of your own disc.')
    }
    Write-Log "Found: $($match.name)" 'Green'

    $files = @($match.files.PSObject.Properties)
    $total = ($files | ForEach-Object { [double]$_.Value.size } | Measure-Object -Sum).Sum

    if (-not $VerifyOnly) {
        $drive = (Get-Item $Native).PSDrive
        $needed = $total + 4GB
        if ($drive.Free -lt $needed) { Fail "Not enough free space on $($drive.Name): ($(Format-Size $drive.Free) free, $(Format-Size $needed) needed)." }
        if (Test-Path $GameDir) { Remove-Item $GameDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $GameDir | Out-Null
    }

    $script:done = 0.0
    $progress = [Action[long]] {
        param($n)
        $script:done += $n
        Write-Progress -Activity ($(if ($VerifyOnly) { 'Verifying game files' } else { 'Verifying and copying game files' })) `
            -Status ('{0} of {1}' -f (Format-Size $script:done), (Format-Size $total)) -PercentComplete ([int](100 * $script:done / $total))
    }

    foreach ($p in $files) {
        $s = $sources[$p.Name.ToLowerInvariant()]
        if ($VerifyOnly) {
            if ($isFolder) { $sha = [TT2Setup.Xdvdfs]::HashFile($s.Path, $progress) }
            else {
                $tmp = Join-Path $env:TEMP ('tt2-verify-' + [Guid]::NewGuid())
                try { $sha = [TT2Setup.Xdvdfs]::CopyHashed($s.Path, $s.Offset, $s.Size, $tmp, $progress) } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
            }
        } else {
            $dest = Join-Path $GameDir ($p.Name.Replace('/', '\'))
            $sha = [TT2Setup.Xdvdfs]::CopyHashed($s.Path, $s.Offset, $s.Size, $dest, $progress)
        }
        if ($sha -ne $p.Value.sha1) {
            Write-Progress -Activity 'Verifying' -Completed
            if (-not $VerifyOnly) { Remove-Item $GameDir -Recurse -Force -ErrorAction SilentlyContinue }
            Fail "$($p.Name) does not match the known good copy (damaged or modified dump). Re-dump your disc."
        }
    }
    Write-Progress -Activity 'Verifying' -Completed
    Write-Log "All $($files.Count) game files verified ($(Format-Size $total))." 'Green'

    if (-not $VerifyOnly) {
        @{ version = $match.id; default_xex_sha1 = $match.default_xex_sha1; verified = (Get-Date -Format s) } |
            ConvertTo-Json | Set-Content (Join-Path $GameDir '.verified.json') -Encoding UTF8
    }
    return $match
}

# ------------------------------------------------------------------------------ 3. toolchain

function Get-Tool($Tool) {
    $dest = Join-Path $ToolDir $Tool.name
    $marker = Join-Path $dest '.installed'
    if ((Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq $Tool.sha256)) { return $dest }

    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
    $zip = Join-Path $DownloadDir ([IO.Path]::GetFileName($Tool.url))
    if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $Tool.sha256.ToUpper()) {
        Write-Log "Downloading $($Tool.name) ($($Tool.url))"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is 10x slower with its progress bar
        try { Invoke-WebRequest -Uri $Tool.url -OutFile $zip -UseBasicParsing } finally { $ProgressPreference = $old }
    }
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($hash -ne $Tool.sha256.ToUpper()) {
        Remove-Item $zip -Force
        Fail "Download of $($Tool.name) is corrupt (SHA-256 $hash). Try again."
    }

    Write-Log "Unpacking $($Tool.name)"
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    $staging = "$dest.unpack"
    if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $staging)
    # Archives that wrap everything in one top folder are flattened.
    $inner = @(Get-ChildItem $staging)
    if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) { Move-Item $inner[0].FullName $dest } else { Move-Item $staging $dest }
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
    Set-Content $marker $Tool.sha256 -Encoding ASCII
    return $dest
}

function Initialize-Toolchain {
    Write-Step 'Compiler toolchain (portable, inside this folder)'
    if ($env:TT2_USE_SYSTEM_TOOLS -eq '1') {
        Write-Log 'TT2_USE_SYSTEM_TOOLS=1: using clang, cmake, ninja and git from PATH.' 'Yellow'
        return
    }
    $paths = @()
    foreach ($t in $toolchain.tools) {
        $dir = Get-Tool $t
        $paths += (Join-Path $dir $t.bin)
    }
    $env:PATH = ($paths -join ';') + ';' + $env:PATH
    foreach ($exe in 'clang++', 'cmake', 'ninja', 'git') {
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { Fail "$exe is missing from the toolchain." }
    }
    Write-Log ('clang: ' + ((& clang++ --version) | Select-Object -First 1))
}

# ------------------------------------------------------------------------------ 4. build

function Invoke-Build {
    if ($Jobs -le 0) { $Jobs = if ($env:TT2_JOBS) { [int]$env:TT2_JOBS } else { [Environment]::ProcessorCount } }
    $common = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++')

    Write-Step 'Building the recompiler tools'
    $toolsBuild = Join-Path $BuildDir 'tools'
    Invoke-Native 'cmake' (@('-S', (Join-Path $Root 'tools'), '-B', $toolsBuild) + $common) 'Configuring the tools'
    Invoke-Native 'cmake' @('--build', $toolsBuild, '--parallel', "$Jobs") 'Building the tools'

    Write-Step 'Translating the game code (Default.xex -> C++)'
    $recomp = Join-Path $BuildDir 'recomp'
    $ppc = Join-Path $recomp 'ppc'
    if (Test-Path $recomp) { Remove-Item $recomp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $ppc | Out-Null
    Copy-Item (Join-Path $GameDir 'Default.xex') (Join-Path $recomp 'default.xex')

    Invoke-Native (Join-Path $toolsBuild 'tt2-analyze.exe') @((Join-Path $recomp 'default.xex'), $recomp) 'Analysing Default.xex'
    # Hand-maintained hooks (frame boundary, input, ...) go after the generated part.
    Get-Content (Join-Path $Root 'config\recomp\tt2_hooks.toml') | Add-Content (Join-Path $recomp 'tt2_recomp.toml')

    $context = Get-Content (Join-Path $toolsBuild 'ppc_context_path.txt') -Raw
    Push-Location $recomp
    try { Invoke-Native (Join-Path $toolsBuild 'XenonRecomp.exe') @('tt2_recomp.toml', $context.Trim()) 'Recompiling' }
    finally { Pop-Location }
    $count = @(Get-ChildItem $ppc -Filter 'ppc_recomp.*.cpp').Count
    Write-Log "$count C++ files generated."

    Write-Step "Compiling the game ($Jobs jobs, this takes a few minutes)"
    $gameBuild = Join-Path $BuildDir 'game'
    $simde = Join-Path (Split-Path (Split-Path $context.Trim() -Parent) -Parent) 'thirdparty\simde'
    Invoke-Native 'cmake' (@('-S', $Root, '-B', $gameBuild, '-DTT2_BUILD_GAME=ON', '-DTT2_BUILD_LAUNCHER=OFF', '-DTT2_BUILD_TESTS=OFF',
        "-DTT2_RECOMP_DIR=$recomp", "-DTT2_SIMDE_DIR=$simde", "-DTT2_OUTPUT_DIR=$BinDir") + $common) 'Configuring the game build'
    Invoke-Native 'cmake' @('--build', $gameBuild, '--target', 'tt2', '--parallel', "$Jobs") 'Compiling the game'
}

# ------------------------------------------------------------------------------ run

try {
    $started = Get-Date
    $verified = Join-Path $GameDir '.verified.json'

    if ($Rebuild -and (Test-Path $verified)) {
        Write-Log 'Rebuild: keeping the verified game data in native\game.'
        $info = Get-Content $verified -Raw | ConvertFrom-Json
        $version = $versions | Where-Object { $_.id -eq $info.version } | Select-Object -First 1
        if (-not $version) { Fail 'The verified game data belongs to a version this release no longer supports. Run setup without -Rebuild.' }
    } else {
        Write-Step 'Checking your copy of the game'
        $source = Find-Image
        if (-not (Test-Path $source)) { Fail "Not found: $source" }
        $version = Copy-VerifiedGame (Resolve-Path $source).Path
        if ($VerifyOnly) { Write-Log 'Verification passed.' 'Green'; return }
    }

    Initialize-Toolchain
    Invoke-Build

    # Netplay only connects players whose game data AND build are identical.
    $buildId = (Get-FileHash -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes(
        $version.default_xex_sha1 + (Get-Content (Join-Path $Root 'VERSION') -Raw).Trim() +
        ((Get-ChildItem (Join-Path $Root 'tools\recomp-patches') -Filter *.patch | Sort-Object Name | Get-FileHash).Hash -join '')))) -Algorithm SHA256).Hash.Substring(0, 16)
    @{ version = $version.id; build_id = $buildId; built = (Get-Date -Format s) } | ConvertTo-Json |
        Set-Content (Join-Path $BinDir 'build-info.json') -Encoding UTF8

    $minutes = [int]((Get-Date) - $started).TotalMinutes
    Write-Log ''
    Write-Log "Done in $minutes min. The launcher will now start the native game." 'Green'
}
catch {
    if (-not $script:Reported) { Write-Log "ERROR: $($_.Exception.Message)" 'Red' }
    exit 1
}
