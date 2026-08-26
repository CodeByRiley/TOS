param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$source = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $InputPath).Path)
$marker = "`tcase NSFB_KEY_BACKSPACE:"
$replacement = @'
	case NSFB_KEY_TAB:
		break;

	case NSFB_KEY_DELETE:
		if (widget->u.text.idx >= widget->u.text.len)
			break;
		memmove(widget->u.text.text + widget->u.text.idx,
				widget->u.text.text + widget->u.text.idx + 1,
				widget->u.text.len - widget->u.text.idx - 1);
		widget->u.text.len--;
		widget->u.text.text[widget->u.text.len] = 0;

		fb_font_width(&font_style, widget->u.text.text,
				widget->u.text.len, &widget->u.text.width);

		caret_moved = true;
		break;

	case NSFB_KEY_BACKSPACE:
'@

$markerIndex = $source.IndexOf($marker, [System.StringComparison]::Ordinal)
if ($markerIndex -lt 0 -or
    $source.IndexOf($marker, $markerIndex + $marker.Length,
        [System.StringComparison]::Ordinal) -ge 0) {
    throw "Expected one text-input marker in $InputPath"
}

$source = $source.Replace($marker, $replacement.TrimEnd("`r", "`n"))
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($OutputPath, $source, $encoding)
