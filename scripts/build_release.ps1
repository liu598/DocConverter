#Requires -Version 5.1
<#
.SYNOPSIS
    One-click build / test / deploy / package for DocConverter.

.DESCRIPTION
    Locates Qt, MSVC and the Windows SDK automatically, configures and builds
    the project with CMake + Ninja, optionally runs CTest, deploys the Qt
    runtime, and optionally rebuilds the installer with Qt Installer Framework.

    No cmd.exe involved: the MSVC environment (INCLUDE / LIB / PATH) is set
    directly from PowerShell, so the script also works in restricted shells.

.EXAMPLE
    .\scripts\build_release.ps1
        Build only, then deploy into .\deploy

.EXAMPLE
    .\scripts\build_release.ps1 -Test -Package
        Build, run the test suite, deploy into the installer package directory
        and regenerate DocConverter_Setup.exe.

.EXAMPLE
    .\scripts\build_release.ps1 -Package -BundlePandoc
        Same as above, but also bundle pandoc.exe so the installer works
        without requiring users to install Pandoc.

.PARAMETER QtPath
    Qt kit directory, e.g. D:\Qt\6.11.1\msvc2022_64. Auto-detected when omitted.

.PARAMETER BundlePandoc
    Copy the locally installed pandoc.exe into tools\pandoc\ of the output
    directory. The app prefers the bundled copy, so users do not need to
    install Pandoc themselves (useful for an offline / portable build).
#>

[CmdletBinding()]
param(
    [string]  $QtPath       = "",
    [string]  $BuildDir     = "",
    [string]  $InstallerDir = "",
    [string]  $Configuration = "Release",
    [switch]  $Test,
    [switch]  $Package,
    [switch]  $BundlePandoc,
    [switch]  $Clean
)

$ErrorActionPreference = 'Stop'
$ProjectDir = Split-Path -Parent $PSScriptRoot

function Write-Step([string]$Message) {
    Write-Host "`n=== $Message ===" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------
function Find-QtKit {
    if ($QtPath) {
        if (-not (Test-Path "$QtPath\bin\windeployqt.exe")) {
            throw "QtPath '$QtPath' does not contain bin\windeployqt.exe"
        }
        return $QtPath
    }

    $kits = @()
    foreach ($root in @('D:\Qt', 'C:\Qt', 'D:\qt', 'C:\qt')) {
        if (-not (Test-Path $root)) { continue }
        $kits += Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'msvc*_64' -ErrorAction SilentlyContinue }
    }
    $kits = $kits | Where-Object { Test-Path "$($_.FullName)\bin\windeployqt.exe" } |
        Sort-Object { [version]($_.Parent.Name -replace '[^\d.]', '') } -Descending

    if (-not $kits) { throw "No Qt MSVC kit found. Pass -QtPath explicitly." }
    return $kits[0].FullName
}

function Find-VsInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = @(& $vswhere -latest -products * -property installationPath) | Select-Object -First 1
        if ($found -and (Test-Path "$found\VC\Tools\MSVC")) { return $found }
    }
    foreach ($candidate in @(
        'D:\vs',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools')) {
        if (Test-Path "$candidate\VC\Tools\MSVC") { return $candidate }
    }
    throw "No Visual Studio C++ toolchain found."
}

function Import-BuildEnvironment([string]$VsInstall) {
    $msvcDir = Get-ChildItem "$VsInstall\VC\Tools\MSVC" -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $msvc = $msvcDir.FullName

    $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
    $sdkVer = (Get-ChildItem "$sdkRoot\Include" -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
    $sdk = "$sdkRoot"

    $env:PATH = "$msvc\bin\Hostx64\x64;$sdk\bin\$sdkVer\x64;$sdk\bin\x64;$env:PATH"
    $env:INCLUDE = "$msvc\include;$sdk\Include\$sdkVer\ucrt;$sdk\Include\$sdkVer\um;" +
                   "$sdk\Include\$sdkVer\shared;$sdk\Include\$sdkVer\winrt"
    $env:LIB = "$msvc\lib\x64;$sdk\Lib\$sdkVer\ucrt\x64;$sdk\Lib\$sdkVer\um\x64"

    Write-Host "  MSVC      : $($msvcDir.Name)"
    Write-Host "  Windows SDK: $sdkVer"
}

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($vs in @('D:\vs', 'C:\Program Files\Microsoft Visual Studio\2022\Community')) {
        $p = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $p) { return $p }
    }
    throw "cmake not found in PATH or in the Visual Studio installation."
}

function Find-BinaryCreator([string]$QtKit) {
    $qtRoot = Split-Path (Split-Path $QtKit -Parent) -Parent
    $tools = Get-ChildItem "$qtRoot\Tools\QtInstallerFramework" -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($t in $tools) {
        $exe = "$($t.FullName)\bin\binarycreator.exe"
        if (Test-Path $exe) { return $exe }
    }
    throw "Qt Installer Framework (binarycreator.exe) not found under $qtRoot\Tools."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Step "Locating toolchain"
$QtKit = Find-QtKit
$Vs = Find-VsInstall
$CMake = Find-CMake
$CTest = Join-Path (Split-Path $CMake -Parent) 'ctest.exe'
Write-Host "  Qt        : $QtKit"
Write-Host "  CMake     : $CMake"

if (-not $BuildDir) { $BuildDir = Join-Path $ProjectDir 'build\windows-msvc' }

# 已有构建目录时沿用其编译器：vswhere 可能找到机器上的另一套 VS，
# 混用两套工具链会让 ninja 直接编译失败（头文件版本对不上）。
# 注意必须在 Import-BuildEnvironment 之前判断，否则 INCLUDE/LIB 会指向另一套 MSVC。
$existingCache = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $existingCache) {
    $match = Select-String -Path $existingCache -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($match -and $match.Matches.Groups[1].Value -match '^(.*?)[\\/]VC[\\/]Tools[\\/]MSVC[\\/][^\\/]+[\\/]') {
        $Vs = $Matches[1]
        Write-Host "  Reusing compiler from existing build dir: $Vs"
    }
}
Write-Host "  Visual C++: $Vs"

Import-BuildEnvironment $Vs
$env:PATH = "$QtKit\bin;$env:PATH"

# Where to put the deployable application directory
if ($Package) {
    if (-not $InstallerDir) {
        $InstallerDir = Join-Path (Split-Path $ProjectDir -Parent) 'DocConverterInstaller'
    }
    $InstallDir = Join-Path $InstallerDir 'packages\com.docconverter.app\data'
} else {
    $InstallDir = Join-Path $ProjectDir 'deploy'
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Cleaning build directory"
    Remove-Item $BuildDir -Recurse -Force
}

Write-Step "Configuring"
# 参数一律走数组 + splat：既避免 PowerShell 5.1 对裸参数 -DNAME=$var 不做展开的坑，
# 也不用反引号续行（这种续行在本文件里曾被编码问题破坏）。
$configureArgs = @(
    '-S', $ProjectDir,
    '-B', $BuildDir,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_PREFIX_PATH=$QtKit"
)
& $CMake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

Write-Step "Building"
& $CMake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

if ($Test) {
    Write-Step "Running tests"
    & $CTest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
}

Write-Step "Deploying to $InstallDir"
if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
& $CMake --install $BuildDir --config $Configuration --prefix $InstallDir
if ($LASTEXITCODE -ne 0) { throw "Deploy failed ($LASTEXITCODE)" }

$sizeMB = [math]::Round(((Get-ChildItem $InstallDir -Recurse -File |
    Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "  Deployed size: $sizeMB MB"

# ---------------------------------------------------------------------------
# 可选：把本机的 pandoc.exe 一起打进产物（tools/pandoc/）。
# 程序会优先使用这个内置副本，用户机器上不装 Pandoc 也能转换 md/txt/html/
# epub/docx 等；涉及 PDF 的转换仍然需要 LibreOffice。
# 产出体积会明显增大（pandoc.exe 约 220MB），适合做「绿色版/离线版」。
# ---------------------------------------------------------------------------
if ($BundlePandoc) {
    Write-Step "Bundling pandoc"
    $pandocSrc = $null
    $cmd = Get-Command pandoc.exe -ErrorAction SilentlyContinue
    if ($cmd) { $pandocSrc = $cmd.Source }
    if (-not $pandocSrc) {
        $candidates = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory |
            Where-Object { $_.Name -like 'JohnMacFarlane.Pandoc*' }
        foreach ($c in $candidates) {
            $found = Get-ChildItem $c.FullName -Recurse -Filter 'pandoc.exe' |
                Select-Object -First 1
            if ($found) { $pandocSrc = $found.FullName; break }
        }
    }
    if (-not $pandocSrc) { throw "BundlePandoc: 未找到本机的 pandoc.exe，请先安装 Pandoc" }

    $bundleDir = Join-Path $InstallDir 'tools\pandoc'
    New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null
    Copy-Item $pandocSrc (Join-Path $bundleDir 'pandoc.exe') -Force
    $pandocMB = [math]::Round((Get-Item (Join-Path $bundleDir 'pandoc.exe')).Length / 1MB, 1)
    Write-Host "  Bundled pandoc.exe ($pandocMB MB) -> $bundleDir"
}

if ($Package) {
    Write-Step "Building installer"
    $BinaryCreator = Find-BinaryCreator $QtKit
    $IconSource = Join-Path $ProjectDir 'resources\app_icon.ico'
    Copy-Item $IconSource (Join-Path $InstallerDir 'config\app_icon.ico') -Force

    # 版本号同步：exe 的版本资源由 CMakeLists 的 project() 生成，
    # 安装包 XML 必须跟它一致，否则「程序 v1.4.0 / 安装器 v1.0.1」会对不上。
    $exeVersion = (Get-Item (Join-Path $InstallDir 'DocConverter.exe')).VersionInfo.ProductVersion
    foreach ($xml in @('config\config.xml', 'packages\com.docconverter.app\meta\package.xml')) {
        $xmlPath = Join-Path $InstallerDir $xml
        if (Test-Path $xmlPath) {
            $content = Get-Content $xmlPath -Raw -Encoding UTF8
            $content = $content -replace '<Version>[^<]+</Version>', "<Version>$exeVersion</Version>"
            Set-Content -Path $xmlPath -Value $content -Encoding UTF8 -NoNewline
            Write-Host "  $xml -> Version $exeVersion"
        }
    }

    $SetupExe = Join-Path $InstallerDir 'DocConverter_Setup.exe'
    # 不预先删除旧包：binarycreator 会直接原地覆盖，
    # 这样也避免某些环境下的删除拦截（安全删除钩子/文件占用）。

    Push-Location $InstallerDir
    try {
        & $BinaryCreator -c config/config.xml -p packages DocConverter_Setup.exe
        if ($LASTEXITCODE -ne 0) { throw "binarycreator failed ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }

    $setupMB = [math]::Round((Get-Item $SetupExe).Length / 1MB, 1)
    Write-Host "  Installer: $SetupExe ($setupMB MB)"
}

Write-Step "Done"
Write-Host "Application directory: $InstallDir"
if ($Package) { Write-Host "Installer: $SetupExe" }
