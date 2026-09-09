using System.ComponentModel;
using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Microsoft.Win32.SafeHandles;

namespace QueueCache.Management;

/// <summary>Management surface. Mutations require an explicitly read/write handle; lifetime remains in the driver.</summary>
[SupportedOSPlatform("windows")]
public sealed class CacheDevice : IDisposable
{
    public const uint StatisticsIoctl = (0x8844u << 16) | (0xD01u << 2);
    public const uint WriteStateIoctl = (0x8844u << 16) | (0xD10u << 2);
    public const uint WriteControlIoctl = (0x8844u << 16) | (3u << 14) | (0xD11u << 2);
    public const uint DiagnosticsIoctl = (0x8844u << 16) | (0xD12u << 2);
    public const uint ExtendedStateIoctl = (0x8844u << 16) | (0xD13u << 2);
    private readonly SafeFileHandle handle;

    public CacheDevice(string device, bool writable = false)
    {
        Path = DevicePath.Normalize(device);
        handle = Native.CreateFileW(Path, writable ? 0xC0000000u : 0, 1 | 2 | 4, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new Win32Exception(error, $"Cannot open {Path}: {new Win32Exception(error).Message}");
        }
    }

    public string Path { get; }

    public CacheDiagnostics GetDiagnostics()
    {
        var data = new byte[CacheDiagnostics.WireSize];
        if (!Native.DeviceIoControl(handle, DiagnosticsIoctl, IntPtr.Zero, 0, data, (uint)data.Length, out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cache diagnostics unavailable (requires matching new driver).");
        if (returned != data.Length) throw new InvalidDataException("Invalid diagnostics length.");
        return CacheDiagnostics.Decode(data);
    }

    public WriteCacheState GetWriteCacheState()
    {
        var data = new byte[WriteCacheState.WireSize];
        if (!Native.DeviceIoControl(handle, WriteStateIoctl, IntPtr.Zero, 0, data, (uint)data.Length, out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Write-cache state unavailable.");
        if (returned != data.Length) throw new InvalidDataException("Invalid write-cache snapshot length.");
        var legacy = WriteCacheState.Decode(data);
        // Never probe an unknown IOCTL on an old driver: unknown controls may be
        // real drain barriers. New drivers advertise extended state in flag 64.
        if ((legacy.Flags & 64) != 0)
        {
            var extended = new byte[WriteCacheState.ExtendedWireSize];
            if (Native.DeviceIoControl(handle, ExtendedStateIoctl, IntPtr.Zero, 0, extended, (uint)extended.Length, out var count, IntPtr.Zero))
            {
                if (count != extended.Length) throw new InvalidDataException("Invalid extended snapshot length.");
                return WriteCacheState.DecodeExtended(extended);
            }
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, "Advertised extended cache state unavailable.");
        }
        return legacy;
    }

    public void Control(WriteCacheAction action, ulong budgetBytes = 0, ulong value = 0)
    {
        if (!Enum.IsDefined(action)) throw new ArgumentOutOfRangeException(nameof(action));
        var command = new byte[32];
        BinaryPrimitives.WriteUInt32LittleEndian(command, 1);
        BinaryPrimitives.WriteUInt32LittleEndian(command.AsSpan(4), 32);
        BinaryPrimitives.WriteUInt32LittleEndian(command.AsSpan(8), (uint)action);
        BinaryPrimitives.WriteUInt64LittleEndian(command.AsSpan(16), budgetBytes);
        BinaryPrimitives.WriteUInt64LittleEndian(command.AsSpan(24), value);
        if (!Native.DeviceIoControlCommand(handle, WriteControlIoctl, command, (uint)command.Length, IntPtr.Zero, 0, out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Cache {action} failed. Inspect cache-status; dirty buffers may be retained.");
        if (returned != 0) throw new InvalidDataException("Unexpected cache control response.");
    }

    public CacheStatistics GetStatistics()
    {
        var data = new byte[CacheStatistics.WireSize];
        if (!Native.DeviceIoControl(handle, StatisticsIoctl, IntPtr.Zero, 0, data,
                (uint)data.Length, out var returned, IntPtr.Zero))
        {
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, $"QueueCache statistics unavailable for {Path}: {new Win32Exception(error).Message}");
        }
        if (returned > data.Length) throw new InvalidDataException("Driver returned an invalid response length.");
        return CacheStatistics.Decode(data.AsSpan(0, (int)returned));
    }

    public static IReadOnlyList<string> EnumerateDevices()
    {
        for (var length = 32768; length <= 1048576; length *= 2)
        {
            var buffer = new char[length];
            var count = Native.QueryDosDeviceW(null, buffer, buffer.Length);
            if (count != 0)
                return new string(buffer, 0, (int)count).Split('\0', StringSplitOptions.RemoveEmptyEntries)
                    .Where(n => (n.Length == 2 && char.IsAsciiLetter(n[0]) && n[1] == ':') ||
                        (n.StartsWith("PhysicalDrive", StringComparison.OrdinalIgnoreCase) &&
                         int.TryParse(n.AsSpan(13), out _)))
                    .Order(StringComparer.OrdinalIgnoreCase).ToArray();
            var error = Marshal.GetLastWin32Error();
            if (error != 122) throw new Win32Exception(error);
        }
        throw new IOException("Device-name buffer exceeded 1 MiB.");
    }

    public void Dispose() => handle.Dispose();

    private static class Native
    {
        [DllImport("kernel32.dll", EntryPoint = "DeviceIoControl", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DeviceIoControlCommand(SafeFileHandle device, uint code, byte[] input,
            uint inputLength, IntPtr output, uint outputLength, out uint returned, IntPtr overlapped);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        internal static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr input,
            uint inputLength, [Out] byte[] output, uint outputLength, out uint returned, IntPtr overlapped);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        internal static extern uint QueryDosDeviceW(string? name, [Out] char[] target, int capacity);
    }
}
