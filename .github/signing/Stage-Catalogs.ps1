param([string]$SignedCatalogDirectory = 'signed-catalogs')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Verify catalog membership against the exact checked-out INF bytes.
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
$signtool = Get-ChildItem -Path "$kits/*/x64/signtool.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (!$signtool) { throw 'Windows SDK signtool.exe is required' }

foreach ($name in @('usb_device', 'eyetoy_device', 'ps4cam_device')) {
    $catalog = Join-Path $SignedCatalogDirectory "$name.cat"
    $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    if ($signature.Status -ne 'Valid' -or !$signature.TimeStamperCertificate) {
        throw "Catalog must have a trusted, timestamped signature: $catalog"
    }
    $cert = $signature.SignerCertificate
    if ($cert.Subject -eq $cert.Issuer) { throw "Self-signed catalog rejected: $catalog" }
    & $signtool.FullName verify /pa /v /c $catalog "driver/$name.inf"
    if ($LASTEXITCODE -ne 0) { throw "Catalog does not authenticate driver/$name.inf" }
    Copy-Item -LiteralPath $catalog -Destination "driver/$name.cat" -Force
    [IO.File]::WriteAllBytes((Join-Path $PWD "driver/$name.cer"),
        $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
}
