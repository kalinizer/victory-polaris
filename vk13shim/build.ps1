# Builds the vulkan-1 shim.
#
#   1. generate the export table from vk.xml
#   2. compile the four implementation files
#   3. link vulkan-1.dll with that .def
#   4. drop a copy of the untouched Khronos loader beside it as
#      vulkan_loader_real.dll (the .def forwarders point at that name)
#
# Usage:  .\build.ps1 [-VkXml <path>] [-OutDir <dir>]

param(
    [string]$VkXml = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = 'C:\Users\vh175\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat'

if (-not $VkXml) {
    $VkXml = Join-Path $root 'third_party\Vulkan-Headers-main\registry\vk.xml'
}
if (-not (Test-Path $VkXml)) { throw "vk.xml not found: $VkXml (see third_party\)" }
if (-not $OutDir) { $OutDir = Join-Path $root 'build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

& $python (Join-Path $root 'tools\gen_exports.py') --vk-xml $VkXml --def (Join-Path $root 'vulkan13.def') --real-def (Join-Path $root 'vulkan_loader_real.def')
if ($LASTEXITCODE -ne 0) { throw 'export generation failed' }

$include = Join-Path $root 'third_party\Vulkan-Headers-main\include'
$sources = @('src\loader.cpp', 'src\renderpass.cpp', 'src\dispatch.cpp', 'src\sync.cpp', 'src\push.cpp', 'src\maint5.cpp', 'src\copy2.cpp', 'src\dynstate.cpp', 'src\spv_dump.cpp') |
    ForEach-Object { Join-Path $root $_ }

$cl = @(
    '/nologo', '/EHsc', '/std:c++20', '/permissive-', '/W3', '/O2', '/MD',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
    '/I', (Join-Path $root 'src'), '/I', $include
) -join ' '
$libCmd = "call `"$vcvars`" >nul 2>&1 && lib /nologo /def:$root\vulkan_loader_real.def /machine:x64 /out:$root\vulkan_loader_real.lib"
cmd /c $libCmd
if ($LASTEXITCODE -ne 0) { throw "import library for the real loader failed" }

$link = @(
    '/DLL', "/DEF:$root\vulkan13.def", "/OUT:$OutDir\vulkan-1.dll", "/IMPLIB:$OutDir\vulkan-1.lib"
) -join ' '

$cmd = "call `"$vcvars`" >nul 2>&1 && cl $cl $($sources -join ' ') $root\vulkan_loader_real.lib /link $link"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "link failed: $LASTEXITCODE" }

# The forwarders need the real loader next to us under a different name.
$real = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
if (Test-Path $real) {
    Copy-Item $real (Join-Path $OutDir 'vulkan_loader_real.dll') -Force
    Write-Host "copied the Khronos loader to vulkan_loader_real.dll"
} else {
    Write-Warning "no $real; copy a Vulkan loader there before running"
}

Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
