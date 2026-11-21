# Generate six clips for the playback and window group tests.
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/test-media/six'))
$ErrorActionPreference = 'Stop'
$root = (New-Item -ItemType Directory -Path $OutputDirectory -Force).FullName
$font = (Join-Path $env:WINDIR 'Fonts/consola.ttf').Replace('\','/').Replace(':','\:')
$clips = @(
    @('01-landscape-h264', '1280x720', 30, 'libx264', 'yuv420p'),
    @('02-padded-hevc',     '720x576', 25, 'libx265', 'yuv420p'),
    @('03-portrait-h264',  '720x1280', 30, 'libx264', 'yuv420p'),
    @('04-main10-hevc',    '720x1280', 25, 'libx265', 'yuv420p10le'),
    @('05-wide-h264',      '1920x800', 24, 'libx264', 'yuv420p'),
    @('06-landscape-hevc',  '960x540', 30, 'libx265', 'yuv420p')
)
foreach ($clip in $clips) {
    $name, $size, $rate, $codec, $pixelFormat = $clip
    $target = Join-Path $root "$name.mp4"
    $temp = Join-Path $root "$name-$([Guid]::NewGuid()).mp4"
    try {
        $path = $target
        if (!(Test-Path -LiteralPath $target)) {
            $path = $temp
            $hue = [int]($name.Substring(0,2)) * 40
            $filter = "testsrc2=size=${size}:rate=${rate},hue=h='$hue+12*t'," +
                "drawtext=fontfile='$font':text='$name  %{pts\:hms}':x=12:y=12:" +
                'fontsize=w/34:fontcolor=white:box=1:boxcolor=black@0.8'
            & ffmpeg -nostdin -hide_banner -loglevel error -n -f lavfi -i $filter -t 90 -an -c:v $codec -threads 4 -preset ultrafast -pix_fmt $pixelFormat -movflags +faststart $path
            if ($LASTEXITCODE -ne 0) { throw "Failed to encode $name" }
        }
        $probe = & ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,duration,pix_fmt -of json $path | ConvertFrom-Json
        $stream = $probe.streams[0]
        $expected = if ($codec -eq 'libx264') { 'h264' } else { 'hevc' }
        if ($LASTEXITCODE -ne 0 -or $stream.codec_name -ne $expected -or
            $stream.pix_fmt -ne $pixelFormat -or [double]$stream.duration -lt 90) {
            throw "Expected a 90-second $expected/$pixelFormat stream at $path"
        }
        if ($path -eq $temp) { Move-Item -LiteralPath $temp -Destination $target }
        Write-Output "$target ($($stream.duration) s)"
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
    }
}
