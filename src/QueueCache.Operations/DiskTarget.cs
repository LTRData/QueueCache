using System.Buffers.Binary;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace QueueCache.Operations;

/// <summary>Validated single-volume secondary-disk target. No guessed disk numbers or raw test writes.</summary>
[SupportedOSPlatform("windows")]
public sealed record DiskTarget(char Letter, int Number, long Bytes, string Instance)
{
    public string Root => $"{Letter}:\\";
    public string Device => $"PhysicalDrive{Number}";

    public static async Task<DiskTarget> InspectAsync(string volume, CancellationToken cancellationToken = default)
    {
        if (volume.Length != 2 || !char.IsAsciiLetter(volume[0]) || volume[1] != ':')
            throw new ArgumentException("Select an explicit volume such as Q:, not a directory or raw disk.");
        var letter = char.ToUpperInvariant(volume[0]);
        var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        info.ArgumentList.Add("-NoProfile"); info.ArgumentList.Add("-Command");
        // Only an already validated ASCII letter enters this script. Native volume extents are checked as well.
        info.ArgumentList.Add($"$ErrorActionPreference='Stop'; $p=@(Get-Partition -DriveLetter {letter}); if($p.Count -ne 1){{throw 'Ambiguous volume'}}; $d=Get-Disk -Number $p[0].DiskNumber; if($d.IsBoot -or $d.IsSystem -or $d.Number -eq 0){{throw 'OS/disk zero excluded'}}; if(@(Get-CimInstance Win32_PageFileUsage | Where-Object {{$_.Name -like '{letter}:*'}}).Count){{throw 'Paging disk excluded'}}; $c=Get-CimInstance Win32_DiskDrive -Filter ('Index='+$d.Number); [pscustomobject]@{{Letter='{letter}';Number=[int]$d.Number;Bytes=[long]$d.Size;Instance=$c.PNPDeviceID}} | ConvertTo-Json -Compress");
        using var process = Process.Start(info) ?? throw new IOException("Cannot inspect disk.");
        var stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderr = process.StandardError.ReadToEndAsync(cancellationToken);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { if (!process.HasExited) process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); throw; }
        if (process.ExitCode != 0) throw new IOException(await stderr);
        var target = JsonSerializer.Deserialize<DiskTarget>(await stdout) ?? throw new IOException("Missing disk identity.");
        if (target.Number <= 0 || target.Bytes <= 0 || string.IsNullOrWhiteSpace(target.Instance) || target.Letter != letter)
            throw new IOException("Invalid disk identity.");
        if (!string.Equals(new DriveInfo(target.Root).DriveFormat, "NTFS", StringComparison.OrdinalIgnoreCase))
            throw new IOException("Only NTFS secondary volumes are currently supported.");
        target.CheckExtents();
        return target;
    }

    public void CheckExtents()
    {
        using var handle = CreateFileW($"\\\\.\\{Letter}:", 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        var buffer = new byte[32];
        if (!DeviceIoControl(handle, 0x560000, IntPtr.Zero, 0, buffer, 32, out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        var start = BinaryPrimitives.ReadInt64LittleEndian(buffer.AsSpan(16));
        var length = BinaryPrimitives.ReadInt64LittleEndian(buffer.AsSpan(24));
        if (returned != 32 || BinaryPrimitives.ReadUInt32LittleEndian(buffer) != 1 ||
            BinaryPrimitives.ReadInt32LittleEndian(buffer.AsSpan(8)) != Number || start < 0 || length <= 0 || start > Bytes - length)
            throw new IOException("Volume extents do not match the selected disk.");
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputBytes,
        [Out] byte[] output, uint outputBytes, out uint returned, IntPtr overlapped);
}
