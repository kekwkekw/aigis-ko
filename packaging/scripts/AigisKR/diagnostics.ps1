param(
    [Parameter(Mandatory = $false)]
    [string]$GameRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$CleanGameRoot = $GameRoot.Trim().Trim('"')
if ($CleanGameRoot -match '^[A-Za-z]:\\$') {
    $Root = $CleanGameRoot
} else {
    $Root = (Resolve-Path -LiteralPath $CleanGameRoot).Path.TrimEnd('\', '/')
    if ($Root -match '^[A-Za-z]:$') {
        $Root = $Root + '\'
    }
}
$Stamp = (Get-Date).ToString("yyyyMMdd_HHmm")
$OutZip = Join-Path $Root ("AigisKR_Support_{0}.zip" -f $Stamp)
$TempStage = Join-Path $env:TEMP ("aigiskr_diag_{0}_{1}" -f $Stamp, $PID)

function Get-FileSha256OrMissing {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "MISSING"
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $hashBytes = $sha.ComputeHash($fs)
        return ([System.BitConverter]::ToString($hashBytes)).Replace("-", "").ToLowerInvariant()
    } catch {
        return "ERROR"
    } finally {
        if ($fs) { $fs.Dispose() }
        if ($sha) { $sha.Dispose() }
    }
}

function Redact-Text {
    param([string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return $Text }
    $r = $Text

    $userName = $env:USERNAME
    $userProf = $env:USERPROFILE
    if (-not [string]::IsNullOrEmpty($userProf)) {
        $escapedProf = [regex]::Escape($userProf)
        $r = [regex]::Replace($r, $escapedProf, "C:\Users\<REDACTED_USER>", "IgnoreCase")
        $escapedProfJson = [regex]::Escape($userProf.Replace("\", "\\"))
        $r = [regex]::Replace($r, $escapedProfJson, "C:\\Users\\<REDACTED_USER>", "IgnoreCase")
    }
    if (-not [string]::IsNullOrEmpty($userName)) {
        $escapedUser = [regex]::Escape($userName)
        $r = [regex]::Replace($r, "(?i)([A-Za-z]:\\Users\\)" + $escapedUser + "(?=\\|$)", '$1<REDACTED_USER>')
        $r = [regex]::Replace($r, "(?i)([A-Za-z]:\\\\Users\\\\)" + $escapedUser + "(?=\\\\|$)", '$1<REDACTED_USER>')
    }

    # Generic Windows user profile path and JSON-escaped / forward-slash variants
    $r = [regex]::Replace($r, "(?i)([A-Za-z]:\\Users\\)[^\\\""'\r\n\t]+", '$1<REDACTED_USER>')
    $r = [regex]::Replace($r, "(?i)([A-Za-z]:\\\\Users\\\\)[^\\\""'\r\n\t]+", '$1<REDACTED_USER>')
    $r = [regex]::Replace($r, "(?i)([A-Za-z]:/Users/)[^/\""'\r\n\t]+", '$1<REDACTED_USER>')

    # Sensitive tokens, cookies, player/account IDs
    # Handle JSON/quoted values before bare command-line/INI values. Include
    # the DMM launch argument names, not only the generic token suffix.
    $sensitiveKey = '(?:dmm_token|login_token|access_token|pf_access_token|onetime_token|session_id|session|cookie|cookies|auth|token|secret|password|player_id|account_id|viewer_id|open_id|user_id|uid)'
    $prefix = '(?i)(["'']?\b' + $sensitiveKey + '\b["'']?\s*[:=]\s*)'
    $r = [regex]::Replace($r, $prefix + '"(?:\\.|[^"\\])*"', '$1"<REDACTED>"')
    $r = [regex]::Replace($r, $prefix + "'(?:\\.|[^'\\])*'", '$1''<REDACTED>''')
    $r = [regex]::Replace($r, $prefix + '[^&\s"''\r\n,}]+', '$1<REDACTED>')
    $r = [regex]::Replace($r, "(?i)\bID\s*[:=]\s*\d{4,}", "ID:<REDACTED>")
    return $r
}

function Copy-RedactedFileIfExists {
    param(
        [string]$SrcPath,
        [string]$DstName
    )
    if (Test-Path -LiteralPath $SrcPath -PathType Leaf) {
        $raw = Get-Content -LiteralPath $SrcPath -Raw -Encoding UTF8
        $redacted = Redact-Text -Text $raw
        $dst = Join-Path $TempStage $DstName
        [System.IO.File]::WriteAllText($dst, $redacted, (New-Object System.Text.UTF8Encoding($false)))
        return $true
    }
    return $false
}

try {
    New-Item -ItemType Directory -Path $TempStage -Force | Out-Null

    $osReg = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion" -ErrorAction SilentlyContinue
    $osCaption = if ($osReg -and $osReg.ProductName) {
        "{0} (DisplayVersion={1}, Build={2}.{3})" -f $osReg.ProductName, $osReg.DisplayVersion, $osReg.CurrentBuildNumber, $osReg.UBR
    } else {
        [System.Environment]::OSVersion.VersionString
    }

    $exePath = Join-Path $Root "Aigis_r.exe"
    $exeSize = if (Test-Path -LiteralPath $exePath -PathType Leaf) { (Get-Item -LiteralPath $exePath).Length } else { 0 }

    $hcPath = Join-Path $Root "scripts\AigisKR\healthcheck_latest.json"
    $hcJson = $null
    if (Test-Path -LiteralPath $hcPath -PathType Leaf) {
        try {
            $hcJson = Get-Content -LiteralPath $hcPath -Raw -Encoding UTF8 | ConvertFrom-Json
        } catch {
            $hcJson = $null
        }
    }
    $atNode = if ($hcJson -and $hcJson.PSObject.Properties.Match("auto_translate").Count -gt 0) { $hcJson.auto_translate } else { $null }
    $atHostMode = if ($atNode -and $atNode.host_mode) { [string]$atNode.host_mode } else { "UNKNOWN" }
    $atHostVer = if ($atNode -and $atNode.host_version) { [string]$atNode.host_version } else { "NONE" }
    $atProtoVer = if ($atNode) { [string]$atNode.host_protocol_ver } else { "0" }
    $atEngineVer = if ($atNode -and $atNode.hdtrans_engine_ver) { [string]$atNode.hdtrans_engine_ver } else { "NONE" }
    $atRootSel = if ($atNode -and $atNode.engine_root_type) { [string]$atNode.engine_root_type } else { "none" }
    $atKlVer = if ($atNode -and $atNode.knowledge_version) { [string]$atNode.knowledge_version } else { "NONE" }

    $arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
    $metaLines = @(
        "AigisKR Support Bundle Summary",
        "GeneratedUtc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
        "AigisKRVersion: 0.1.0",
        "WindowsVersion: $osCaption",
        "OSArchitecture: $arch",
        "GameRootRedacted: $(Redact-Text -Text $Root)",
        "GameExeSize: $exeSize",
        "GameExeSha256: $(Get-FileSha256OrMissing -Path $exePath)",
        "WinmmSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'winmm.dll'))",
        "AigisKRAsiSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR.asi'))",
        "HdTransHostExeSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR\bin\AigisKR.HdTransHost.exe'))",
        "BundledJ2KEngineSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR\hdtrans\J2KEngine.dll'))",
        "TextureToolkitAsiSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\TextureToolkit-x86.asi'))",
        "TranslationsDbSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR\data\translations.json'))",
        "TemplatesDbSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR\data\templates.json'))",
        "NoticesDbSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'scripts\AigisKR\data\notices.json'))",
        "TextureManifestSha256: $(Get-FileSha256OrMissing -Path (Join-Path $Root 'TT\texture_manifest_r2.json'))",
        "AutoTranslateHostMode: $atHostMode",
        "AutoTranslateHostVersion: $atHostVer",
        "AutoTranslateHostProtocolVersion: $atProtoVer",
        "AutoTranslateEngineVersion: $atEngineVer",
        "AutoTranslateSelection: $atRootSel",
        "AutoTranslateKnowledgeVersion: $atKlVer"
    )
    [System.IO.File]::WriteAllText((Join-Path $TempStage "summary.txt"), ($metaLines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))

    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "scripts\AigisKR\healthcheck_latest.json") -DstName "healthcheck_latest.json")
    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "TT\tt_profile_status.json") -DstName "tt_profile_status.json")
    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "AigisKR.log") -DstName "AigisKR.log")
    if (-not (Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "scripts\TextureToolkit.log") -DstName "TextureToolkit.log")) {
        [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "TextureToolkit.log") -DstName "TextureToolkit.log")
    }
    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "scripts\AigisKR.ini") -DstName "AigisKR.ini")
    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "scripts\TextureToolkit.ini") -DstName "TextureToolkit.ini")
    [void](Copy-RedactedFileIfExists -SrcPath (Join-Path $Root "scripts\global.ini") -DstName "global.ini")

    if (Test-Path -LiteralPath $OutZip) {
        Remove-Item -LiteralPath $OutZip -Force
    }
    Compress-Archive -Path (Join-Path $TempStage "*") -DestinationPath $OutZip -CompressionLevel Optimal -Force
    Write-Output "[AigisKR Diagnostics] Created support bundle: $OutZip"
}
finally {
    if (Test-Path -LiteralPath $TempStage) {
        Remove-Item -LiteralPath $TempStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
