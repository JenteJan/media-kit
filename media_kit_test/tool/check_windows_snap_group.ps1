# Run against the three-window Flutter example after all nine players have opened.
# Captures only video tile interiors belonging to the supplied process.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('d3d11va','no')][string]$Decoder = 'd3d11va'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;


public static class SnapTest {
    public struct Rect { public int Left, Top, Right, Bottom; }
    public struct Point { public int X, Y; }
    [DllImport("user32")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32", CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string cls, string title);
    public static IntPtr FindTestWindow(string title) { return FindWindow(null, title); }
    [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32")] static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("kernel32")] static extern uint GetCurrentThreadId();
    [DllImport("user32")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32")] static extern bool GetCursorPos(out Point point);
    [DllImport("user32")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32")] static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("user32")] static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32")] public static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
    [DllImport("user32")] public static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32")] public static extern IntPtr GetAncestor(IntPtr hwnd, uint flag);
    [DllImport("user32")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("dwmapi")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out Rect rect, int size);
    [DllImport("user32")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    public static void FocusWindow(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return;
        SetForegroundWindow(hwnd);
        System.Threading.Thread.Sleep(80);
        if (GetForegroundWindow() == hwnd) return;
        uint owner;
        uint foreground = GetWindowThreadProcessId(GetForegroundWindow(), out owner);
        uint self = GetCurrentThreadId();
        bool attached = self != foreground && AttachThreadInput(self, foreground, true);
        try {
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
        } finally {
            if (attached) AttachThreadInput(self, foreground, false);
        }
        if (GetForegroundWindow() == hwnd) return;
        // The Win32 foreground lock can reject programmatic activation from
        // the helper process. Click only a verified caption of this test HWND.
        Rect frame;
        Point previous;
        if (!GetCursorPos(out previous) || DwmGetWindowAttribute(hwnd, 9, out frame, 16) != 0) return;
        SetWindowPos(hwnd, new IntPtr(-1), 0, 0, 0, 0, 0x13);
        try {
            System.Threading.Thread.Sleep(120);
            for (int offset = 8; offset <= 30; offset += 3) {
                Point point = new Point { X = (frame.Left + frame.Right) / 2, Y = frame.Top + offset };
                IntPtr packed = new IntPtr((point.Y << 16) | (point.X & 0xffff));
                if (GetAncestor(WindowFromPoint(point), 2) != hwnd ||
                    SendMessage(hwnd, 0x84, IntPtr.Zero, packed).ToInt64() != 2) continue;
                if (!SetCursorPos(point.X, point.Y)) return;
                mouse_event(2, 0, 0, 0, UIntPtr.Zero);
                mouse_event(4, 0, 0, 0, UIntPtr.Zero);
                System.Threading.Thread.Sleep(100);
                break;
            }
        } finally {
            SetWindowPos(hwnd, new IntPtr(-2), 0, 0, 0, 0, 0x13);
            SetCursorPos(previous.X, previous.Y);
        }
    }
    public static void SnapKey(IntPtr hwnd, byte arrow) {
        if (GetForegroundWindow() != hwnd)
            throw new InvalidOperationException("Focus left the test window before snapping.");
        keybd_event(0x5B, 0, 1, UIntPtr.Zero);
        keybd_event(arrow, 0, 1, UIntPtr.Zero);
        keybd_event(arrow, 0, 3, UIntPtr.Zero);
        keybd_event(0x5B, 0, 3, UIntPtr.Zero);
    }
}
'@

$previousDpi = [SnapTest]::SetThreadDpiAwarenessContext([IntPtr](-4))
$images = @()
try {
    $windows = @(foreach ($number in 1..3) {
        $hwnd = [SnapTest]::FindTestWindow("media-kit / window $number / $Decoder")
        [uint32]$owner = 0
        [void][SnapTest]::GetWindowThreadProcessId($hwnd, [ref]$owner)
        if ($hwnd -eq [IntPtr]::Zero -or $owner -ne $ProcessId) {
            throw "Could not identify window $number of process $ProcessId"
        }
        $hwnd
    })
    $work = [System.Windows.Forms.Screen]::FromHandle($windows[0]).WorkingArea
    $halfWidth = [int][Math]::Floor($work.Width / 2)
    $halfHeight = [int][Math]::Floor($work.Height / 2)
    $midX = $work.Left + $halfWidth
    $midY = $work.Top + $halfHeight
    $expected = @(
        ,@($work.Left, $work.Top, $midX, $work.Bottom)
        ,@($midX, $work.Top, $work.Right, $midY)
        ,@($midX, $midY, $work.Right, $work.Bottom)
    )
    $bounds = @()
    for ($i = 0; $i -lt 3; $i++) {
        $hwnd = $windows[$i]
        [void][SnapTest]::ShowWindow($hwnd, 9) # SW_RESTORE
        $keys = switch ($i) { 0 { 0x25 } 1 { 0x27; 0x26 } 2 { 0x27; 0x28 } }
        foreach ($key in $keys) {
            [SnapTest]::FocusWindow($hwnd)
            Start-Sleep -Milliseconds 150
            if ([SnapTest]::GetForegroundWindow() -ne $hwnd) {
                throw "Cannot focus window $($i+1) for key $key (foreground $([SnapTest]::GetForegroundWindow()), target $hwnd)"
            }
            [SnapTest]::SnapKey($hwnd, $key)
            Start-Sleep -Milliseconds 500
        }
    }
    $regions = @()
    $clients = @()
    for ($i = 0; $i -lt 3; $i++) {
        $hwnd = $windows[$i]
        $frame = New-Object SnapTest+Rect
        if ([SnapTest]::DwmGetWindowAttribute($hwnd, 9, [ref]$frame, 16) -ne 0) {
            throw 'Could not get the visible window bounds'
        }
        $actual = @($frame.Left, $frame.Top, $frame.Right, $frame.Bottom)
        for ($edge = 0; $edge -lt 4; $edge++) {
            if ([Math]::Abs($actual[$edge] - $expected[$i][$edge]) -gt 16) {
                throw "Window $($i+1) bounds $actual, expected $($expected[$i])"
            }
        }
        $bounds += ,$actual
        $client = New-Object SnapTest+Rect
        $origin = New-Object SnapTest+Point
        if (![SnapTest]::GetClientRect($hwnd, [ref]$client) -or
            ![SnapTest]::ClientToScreen($hwnd, [ref]$origin)) { throw 'Missing client bounds' }
        $clients += [Drawing.Rectangle]::new($origin.X, $origin.Y, $client.Right, $client.Bottom)
        for ($tile = 0; $tile -lt 3; $tile++) {
            $cx = $origin.X + [int](($tile + 0.5) * $client.Right / 3)
            $cy = $origin.Y + [int]($client.Bottom * 0.54)
            # Sample across the video. The center of testsrc2 can be a single
            # stationary color even while other parts of the frame are moving.
            $w = [int]($client.Right / 3 * 0.86)
            $h = [int]($client.Bottom * 0.52)
            $regions += [pscustomobject]@{
                X=$cx-[int]($w/2); Y=$cy-[int]($h/2); Width=$w; Height=$h; Window=$hwnd
            }
        }
    }
    function CaptureTiles {
        $bitmap = [Drawing.Bitmap]::new(192,192)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            for ($i = 0; $i -lt $regions.Count; $i++) {
                $region = $regions[$i]
                foreach ($dx in 0,($region.Width-1)) { foreach ($dy in 0,($region.Height-1)) {
                    $point = New-Object SnapTest+Point
                    $point.X = $region.X + $dx
                    $point.Y = $region.Y + $dy
                    $root = [SnapTest]::GetAncestor([SnapTest]::WindowFromPoint($point), 2)
                    if ($root -ne $region.Window) { throw "Video tile $($i+1) is obscured" }
                }}
                $tileImage = [Drawing.Bitmap]::new($region.Width,$region.Height)
                $tileGraphics = [Drawing.Graphics]::FromImage($tileImage)
                try {
                    $tileGraphics.CopyFromScreen($region.X, $region.Y, 0, 0, $tileImage.Size)
                    $dest = [Drawing.Rectangle]::new(($i%3)*64, [int][Math]::Floor($i/3)*64, 64, 64)
                    $graphics.DrawImage($tileImage, $dest)
                } finally { $tileGraphics.Dispose(); $tileImage.Dispose() }
            }
            $bitmap
        } catch {
            $bitmap.Dispose()
            throw
        } finally { $graphics.Dispose() }
    }
    for ($n = 0; $n -lt 4; $n++) {
        $images += CaptureTiles
        Start-Sleep -Milliseconds 650
    }
    $motion = @(for ($tile = 0; $tile -lt 9; $tile++) {
        $changes = 0
        $left = ($tile % 3)*64
        $top = [int][Math]::Floor($tile / 3)*64
        for ($y = $top; $y -lt $top+64; $y += 2) {
            for ($x = $left; $x -lt $left+64; $x += 2) {
                $a = $images[0].GetPixel($x,$y)
                foreach ($sample in $images[1..3]) {
                    $b = $sample.GetPixel($x,$y)
                    if (([Math]::Abs($a.R-$b.R) + [Math]::Abs($a.G-$b.G) +
                         [Math]::Abs($a.B-$b.B)) -gt 30) { $changes++; break }
                }
            }
        }
        $changes
    })
    $out = New-Item -ItemType Directory -Force -Path $OutputDirectory
    for ($n = 0; $n -lt $images.Count; $n++) {
        $images[$n].Save((Join-Path $out "tiles-$n.png"), [Drawing.Imaging.ImageFormat]::Png)
    }
    [ordered]@{ processId=$ProcessId; windowBounds=$bounds; changedPixelsOf1024=$motion } |
        ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'snap-group.json')
    # Save only the test windows, arranged at their verified screen coordinates.
    $group = [Drawing.Bitmap]::new($work.Width, $work.Height)
    $canvas = [Drawing.Graphics]::FromImage($group)
    try {
        for ($i = 0; $i -lt 3; $i++) {
            $client = $clients[$i]
            $shot = [Drawing.Bitmap]::new($client.Width, $client.Height)
            $drawing = [Drawing.Graphics]::FromImage($shot)
            try {
                $dc = $drawing.GetHdc()
                try {
                    if (![SnapTest]::PrintWindow($windows[$i], $dc, 3)) { throw "PrintWindow failed" }
                } finally { $drawing.ReleaseHdc($dc) }
                $canvas.DrawImageUnscaled($shot, $client.X-$work.Left, $client.Y-$work.Top)
            } finally { $drawing.Dispose(); $shot.Dispose() }
        }
        $group.Save((Join-Path $out 'group.png'), [Drawing.Imaging.ImageFormat]::Png)
    } finally { $canvas.Dispose(); $group.Dispose() }
    if (($motion | Measure-Object -Minimum).Minimum -lt 8) { throw "Insufficient visible motion: $motion" }
    Write-Output "Three snapped windows, moving video in 9/9 tiles: $motion"
} finally {
    foreach ($image in $images) { $image.Dispose() }
    [void][SnapTest]::SetThreadDpiAwarenessContext($previousDpi)
}
