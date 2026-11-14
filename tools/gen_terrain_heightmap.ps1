# Procedural heightmap tiles for terrain lab (quadtree LOD).
# Writes content/terrain/heightmap.bmp, albedo.bmp, tiles/tile_x_z.bmp
# Prefer app first-run C++ generator; this script is a fallback for CI/manual use.

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$terrainDir = Join-Path $root "content\terrain"
$tileDir = Join-Path $terrainDir "tiles"
New-Item -ItemType Directory -Force -Path $tileDir | Out-Null

# int32-safe hash (matches spirit of C++ Hash01, not bit-identical)
function Hash01([int]$x, [int]$y, [int]$seed) {
    [int64]$n = [int64]$x * 374761393L + [int64]$y * 668265263L + [int64]$seed * 1274126177L
    $n = $n -band 0x7FFFFFFF
    $n = $n -bxor ($n -shr 13)
    $n = ($n * 1274126177L) -band 0x7FFFFFFF
    $n = $n -bxor ($n -shr 16)
    return [double]($n -band 0xFFFF) / 65535.0
}

function ValueNoise([double]$x, [double]$y, [int]$seed) {
    $x0 = [math]::Floor($x)
    $y0 = [math]::Floor($y)
    $fx = $x - $x0
    $fy = $y - $y0
    $fx = $fx * $fx * (3.0 - 2.0 * $fx)
    $fy = $fy * $fy * (3.0 - 2.0 * $fy)
    $a = (Hash01 ([int]$x0) ([int]$y0) $seed) * (1.0 - $fx) + (Hash01 ([int]($x0 + 1)) ([int]$y0) $seed) * $fx
    $b = (Hash01 ([int]$x0) ([int]($y0 + 1)) $seed) * (1.0 - $fx) + (Hash01 ([int]($x0 + 1)) ([int]($y0 + 1)) $seed) * $fx
    return $a * (1.0 - $fy) + $b * $fy
}

function Write-BmpBgr([string]$path, [byte[]]$bgr, [int]$w, [int]$h) {
    $stride = [int]([math]::Floor(($w * 3 + 3) / 4) * 4)
    $pix = $stride * $h
    $fs = 54 + $pix
    $hdr = New-Object byte[] 54
    $hdr[0] = [byte][char]'B'
    $hdr[1] = [byte][char]'M'
    [BitConverter]::GetBytes([int]$fs).CopyTo($hdr, 2)
    $hdr[10] = 54
    $hdr[14] = 40
    [BitConverter]::GetBytes([int]$w).CopyTo($hdr, 18)
    [BitConverter]::GetBytes([int]$h).CopyTo($hdr, 22)
    $hdr[26] = 1
    $hdr[28] = 24
    [BitConverter]::GetBytes([int]$pix).CopyTo($hdr, 34)
    $stream = [IO.File]::Create($path)
    if (-not $stream) { throw "Cannot create $path" }
    $stream.Write($hdr, 0, 54)
    $row = New-Object byte[] $stride
    for ($y = $h - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w; $x++) {
            $si = ($y * $w + $x) * 3
            $di = $x * 3
            $row[$di] = $bgr[$si]
            $row[$di + 1] = $bgr[$si + 1]
            $row[$di + 2] = $bgr[$si + 2]
        }
        $stream.Write($row, 0, $stride)
    }
    $stream.Flush()
    $stream.Close()
}

$size = 512
$tile = 256
Write-Host "Generating ${size}x${size} heightmap..."

$gray = New-Object byte[] ($size * $size)
$rgb = New-Object byte[] ($size * $size * 3)

for ($y = 0; $y -lt $size; $y++) {
    for ($x = 0; $x -lt $size; $x++) {
        $nx = $x / [double]$size
        $ny = $y / [double]$size
        $h = 0.0
        $amp = 1.0
        $freq = 2.0
        $norm = 0.0
        for ($o = 0; $o -lt 5; $o++) {
            $h += $amp * (ValueNoise ($nx * $freq * 3.0) ($ny * $freq * 3.0) (42 + $o))
            $norm += $amp
            $amp *= 0.5
            $freq *= 2.0
        }
        $h /= $norm
        $ridge = 1.0 - [math]::Abs(2.0 * (ValueNoise ($nx * 2.5) ($ny * 2.5) 99) - 1.0)
        $h = [math]::Pow(($h * 0.7 + $ridge * 0.3), 1.2)
        $v = [byte]([math]::Max(0, [math]::Min(255, $h * 255.0)))
        $i = $y * $size + $x
        $gray[$i] = $v
        $pi = $i * 3
        $rgb[$pi] = [byte](22 + $h * 40)
        $rgb[$pi + 1] = [byte](48 + $h * 110)
        $rgb[$pi + 2] = [byte](28 + $h * 70)
    }
}

$grayBgr = New-Object byte[] ($size * $size * 3)
for ($i = 0; $i -lt ($size * $size); $i++) {
    $grayBgr[$i * 3] = $gray[$i]
    $grayBgr[$i * 3 + 1] = $gray[$i]
    $grayBgr[$i * 3 + 2] = $gray[$i]
}

Write-BmpBgr (Join-Path $terrainDir "heightmap.bmp") $grayBgr $size $size
Write-BmpBgr (Join-Path $terrainDir "albedo.bmp") $rgb $size $size

$n = [int]($size / $tile)
for ($tz = 0; $tz -lt $n; $tz++) {
    for ($tx = 0; $tx -lt $n; $tx++) {
        $tg = New-Object byte[] ($tile * $tile * 3)
        for ($y = 0; $y -lt $tile; $y++) {
            for ($x = 0; $x -lt $tile; $x++) {
                $si = (($tz * $tile + $y) * $size + ($tx * $tile + $x))
                $di = ($y * $tile + $x) * 3
                $v = $gray[$si]
                $tg[$di] = $v
                $tg[$di + 1] = $v
                $tg[$di + 2] = $v
            }
        }
        $name = "tile_{0}_{1}.bmp" -f $tx, $tz
        Write-BmpBgr (Join-Path $tileDir $name) $tg $tile $tile
    }
}

Write-Host "Done:"
Get-ChildItem $terrainDir -Recurse | Select-Object Name, Length
