[CmdletBinding()]
param(
    [string]$PortName = "COM4"
)

$ErrorActionPreference = "Stop"

$Port = [System.IO.Ports.SerialPort]::new(
    $PortName,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$Port.DtrEnable = $true
$Port.RtsEnable = $true
$Port.ReadTimeout = 50
$Port.WriteTimeout = 500

try {
    $Port.Open()
    Write-Host "SmartKnob monitor on $PortName"
    Write-Host "Keys: E arm, T run test, X stop, S status, H help, Q quit"

    while ($true) {
        if ($Port.BytesToRead -gt 0) {
            Write-Host -NoNewline $Port.ReadExisting()
        }

        if ([Console]::KeyAvailable) {
            $Key = [Console]::ReadKey($true)
            $Character = [char]::ToLowerInvariant($Key.KeyChar)
            if ($Character -eq "q") {
                break
            }

            if ("etxsh".Contains($Character)) {
                $Port.Write([string]$Character)
            }
        }

        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($Port.IsOpen) {
        $Port.Close()
    }
    $Port.Dispose()
}
