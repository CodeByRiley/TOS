<#
.SYNOPSIS
    Print the source tree, for getting oriented from a Windows shell.

.DESCRIPTION
    Directories only by default, each annotated with how many files sit
    directly in it. The vendored and generated trees are skipped unless -All
    is given: between them musl, NetSurf and build/ are around eighteen
    thousand files, and none of it is the code you went looking for.

    Drawn with ASCII rather than box-drawing characters so it survives a
    console that is not UTF-8.

.PARAMETER Path
    What to walk. Defaults to kernel/ and userspace/, relative to the
    repository root, so the script works from any directory.

.PARAMETER Depth
    How far down to go. Default 3.

.PARAMETER Files
    List file names too, not just the per-directory count.

.PARAMETER All
    Include the vendored and generated directories.

.EXAMPLE
    tools/tree.ps1
.EXAMPLE
    tools/tree.ps1 kernel -Depth 2 -Files
.EXAMPLE
    tools/tree.ps1 userspace/lib -Files
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string[]]$Path,
    [int]$Depth = 3,
    [switch]$Files,
    [switch]$All
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot

# Vendored source, build output, and the git database. Matched by directory
# name at any level, which is enough because none of these names recur
# somewhere they would be wanted.
$Skip = @(
    '.git',
    'build',
    'dist',
    'musl-1.2.6',
    'build-musl-tos',
    'netsurf',
    'doomgeneric'
)

function Show-Branch {
    param(
        [string]$Dir,
        [string]$Prefix,
        [int]$Level
    )

    if ($Level -ge $Depth) { return }

    $dirs = @(Get-ChildItem -LiteralPath $Dir -Directory -ErrorAction SilentlyContinue |
              Where-Object { $All -or ($Skip -notcontains $_.Name) } |
              Sort-Object Name)

    $leaves = @()
    if ($Files) {
        $leaves = @(Get-ChildItem -LiteralPath $Dir -File -ErrorAction SilentlyContinue |
                    Sort-Object Name)
    }

    $entries = @($dirs) + @($leaves)
    for ($i = 0; $i -lt $entries.Count; $i++) {
        $entry = $entries[$i]
        $isLast = ($i -eq $entries.Count - 1)
        $branch = if ($isLast) { '`-- ' } else { '|-- ' }

        if ($entry.PSIsContainer) {
            $count = @(Get-ChildItem -LiteralPath $entry.FullName -File `
                                     -ErrorAction SilentlyContinue).Count
            $note = if ($count -gt 0 -and -not $Files) {
                "  ($count file$(if ($count -ne 1) { 's' }))"
            } else { '' }
            Write-Output "$Prefix$branch$($entry.Name)/$note"

            # Two spaces where the branch ended, a bar where it continues.
            $childPrefix = $Prefix + $(if ($isLast) { '    ' } else { '|   ' })
            Show-Branch -Dir $entry.FullName -Prefix $childPrefix -Level ($Level + 1)
        }
        else {
            Write-Output "$Prefix$branch$($entry.Name)"
        }
    }
}

if (-not $Path) {
    $Path = @('kernel', 'userspace')
}

foreach ($p in $Path) {
    # Accept both a path relative to the repository root and an absolute one,
    # so this reads the same whether it is run from the root or from tools/.
    $full = if ([System.IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $Root $p }

    if (-not (Test-Path -LiteralPath $full)) {
        Write-Warning "no such path: $p"
        continue
    }

    # Forward slashes, and relative to the root where it sits under it, so the
    # heading matches how paths are written everywhere else in the tree.
    $label = (Resolve-Path -LiteralPath $full).Path.Replace("$Root\", '').Replace('\', '/')

    # The heading carries its own file count for the same reason the branches
    # do , without it a directory holding only files (userspace/lib, say)
    # prints a bare heading and nothing else, which reads as a failure.
    $own = @(Get-ChildItem -LiteralPath $full -File -ErrorAction SilentlyContinue).Count
    if ($own -gt 0 -and -not $Files) {
        Write-Output "$label  ($own file$(if ($own -ne 1) { 's' }))"
    } else {
        Write-Output $label
    }
    Show-Branch -Dir $full -Prefix '' -Level 0
    Write-Output ''
}
