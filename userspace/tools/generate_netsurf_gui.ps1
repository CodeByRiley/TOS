param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$source = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $InputPath).Path)

function Replace-Once {
    param(
        [string]$Text,
        [string]$Marker,
        [string]$Replacement
    )

    $index = $Text.IndexOf($Marker, [System.StringComparison]::Ordinal)
    if ($index -lt 0 -or
        $Text.IndexOf($Marker, $index + $Marker.Length,
            [System.StringComparison]::Ordinal) -ge 0) {
        throw "Expected one GUI marker in $InputPath"
    }
    return $Text.Replace($Marker, $Replacement)
}

$source = Replace-Once $source `
    '#define NSFB_TOOLBAR_DEFAULT_LAYOUT "blfsrutc"' `
    '#define NSFB_TOOLBAR_DEFAULT_LAYOUT "blfsrut"'

$createMarker = @'
		NSLOG(netsurf, INFO, "toolbar adding %c", *itmtype);
'@
$createReplacement = @'
		if (*itmtype == 'c') {
			itmtype += xdir;
			continue;
		}

		NSLOG(netsurf, INFO, "toolbar adding %c", *itmtype);
'@
$source = Replace-Once $source $createMarker.TrimEnd("`r", "`n") `
    $createReplacement.TrimEnd("`r", "`n")

$resizeMarker = @'
	while (itmtype >= toolbar_layout && xdir != 0) {

		switch (*itmtype) {
'@
$resizeReplacement = @'
	while (itmtype >= toolbar_layout && xdir != 0) {

		if (*itmtype == 'c') {
			itmtype += xdir;
			continue;
		}

		switch (*itmtype) {
'@
$source = Replace-Once $source $resizeMarker.TrimEnd("`r", "`n") `
    $resizeReplacement.TrimEnd("`r", "`n")

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($OutputPath, $source, $encoding)
