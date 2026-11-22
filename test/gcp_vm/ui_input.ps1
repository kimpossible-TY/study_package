param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("move", "click", "key", "hotkey", "text")]
    [string]$Action,

    [Parameter(Position = 1)]
    [string]$Arg1 = "",

    [Parameter(Position = 2)]
    [string]$Arg2 = ""
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class InputHelper {
    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, UIntPtr dwExtraInfo);

    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    public const uint MOUSEEVENTF_MOVE = 0x0001;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    public const uint KEYEVENTF_KEYUP = 0x0002;

    public static void MoveMouse(int x, int y) {
        SetProcessDPIAware();
        int sw = Math.Max(1, GetSystemMetrics(0) - 1);
        int sh = Math.Max(1, GetSystemMetrics(1) - 1);
        SetCursorPos(x, y);
        int ax = (int)Math.Round(x * 65535.0 / sw);
        int ay = (int)Math.Round(y * 65535.0 / sh);
        mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, ax, ay, 0, UIntPtr.Zero);
    }

    public static void ClickMouse(int x, int y) {
        MoveMouse(x, y);
        Thread.Sleep(30);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        Thread.Sleep(30);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }

    public static void TapVk(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        Thread.Sleep(25);
        keybd_event(vk, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static void Chord(byte[] vks) {
        for (int i = 0; i < vks.Length; i++) {
            keybd_event(vks[i], 0, 0, UIntPtr.Zero);
            Thread.Sleep(20);
        }
        Thread.Sleep(30);
        for (int i = vks.Length - 1; i >= 0; i--) {
            keybd_event(vks[i], 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
            Thread.Sleep(20);
        }
    }
}
"@

function Resolve-Vk([string]$name) {
    switch ($name.ToLowerInvariant()) {
        "ctrl"   { return [byte]0x11 }
        "alt"    { return [byte]0x12 }
        "shift"  { return [byte]0x10 }
        "space"  { return [byte]0x20 }
        "enter"  { return [byte]0x0D }
        "tab"    { return [byte]0x09 }
        "esc"    { return [byte]0x1B }
        "escape" { return [byte]0x1B }
        "f2"     { return [byte]0x71 }
        "equal"  { return [byte]0xBB }
        "plus"   { return [byte]0xBB }
        "="      { return [byte]0xBB }
        "minus"  { return [byte]0xBD }
        "-"      { return [byte]0xBD }
        "comma"  { return [byte]0xBC }
        ","      { return [byte]0xBC }
        default {
            if ($name.Length -eq 1) {
                $ch = [char]$name.ToUpperInvariant()
                return [byte][int]$ch
            }
            throw "Unknown key name: $name"
        }
    }
}

[void][InputHelper]::SetProcessDPIAware()

switch ($Action) {
    "move" {
        [InputHelper]::MoveMouse([int]$Arg1, [int]$Arg2)
        Write-Output "Moved cursor to ($Arg1, $Arg2)"
    }
    "click" {
        [InputHelper]::ClickMouse([int]$Arg1, [int]$Arg2)
        Write-Output "Clicked ($Arg1, $Arg2)"
    }
    "key" {
        $vk = Resolve-Vk $Arg1
        [InputHelper]::TapVk($vk)
        Write-Output "Tapped key $Arg1"
    }
    "hotkey" {
        $parts = $Arg1 -split "\+"
        $vks = [byte[]]@($parts | ForEach-Object { Resolve-Vk $_ })
        [InputHelper]::Chord($vks)
        Write-Output "Sent hotkey $Arg1"
    }
    "text" {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.SendKeys]::SendWait($Arg1)
        Write-Output "Sent text ($($Arg1.Length) chars)"
    }
}
