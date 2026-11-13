# Sends keyboard and mouse input to a running Fragile Allegiance window, for testing the native input path.
# Steps are separated by spaces. Each is "key:<scan code in hex>", "click:<x>,<y>" in game pixels of a 640x480
# picture, or "wait:<seconds>".
param([string]$Steps)
$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NaxInput {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$window = [NaxInput]::FindWindow("NaxFragileAllegiance", [NullString]::Value)
if ($window -eq [IntPtr]::Zero) { throw "The game window is not open." }
foreach ($step in ($Steps -split " " | Where-Object { $_ })) {
    $kind, $value = $step.Split(":", 2)
    switch ($kind) {
        "wait" { Start-Sleep -Milliseconds ([int]([double]$value * 1000)) }
        "key" {
            $scan = [Convert]::ToInt32($value, 16)
            [NaxInput]::PostMessage($window, 0x0100, [IntPtr]0, [IntPtr](1 -bor ($scan -shl 16))) | Out-Null
            Start-Sleep -Milliseconds 80
            [NaxInput]::PostMessage($window, 0x0101, [IntPtr]0, [IntPtr](1 -bor ($scan -shl 16) -bor (3 -shl 30))) | Out-Null
            Start-Sleep -Milliseconds 80
        }
        "click" {
            $x, $y = $value.Split(",") | ForEach-Object { [int]$_ }
            $r = New-Object NaxInput+RECT
            [NaxInput]::GetClientRect($window, [ref]$r) | Out-Null
            $w = $r.Right; $h = $r.Bottom
            $pw = $w; $ph = [int]($w * 3 / 4)
            if ($ph -gt $h) { $ph = $h; $pw = [int]($h * 4 / 3) }
            $cx = [int](($w - $pw) / 2 + ($x + 0.5) * $pw / 640)
            $cy = [int](($h - $ph) / 2 + ($y + 0.5) * $ph / 480)
            $pos = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
            [NaxInput]::PostMessage($window, 0x0200, [IntPtr]0, $pos) | Out-Null
            Start-Sleep -Milliseconds 150
            [NaxInput]::PostMessage($window, 0x0201, [IntPtr]1, $pos) | Out-Null
            Start-Sleep -Milliseconds 120
            [NaxInput]::PostMessage($window, 0x0202, [IntPtr]0, $pos) | Out-Null
            Start-Sleep -Milliseconds 150
        }
    }
}
