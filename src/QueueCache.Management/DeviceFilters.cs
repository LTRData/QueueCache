using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace QueueCache.Management;

/// <summary>Exact devnode properties only. Never reads/writes disk-class filters.</summary>
[SupportedOSPlatform("windows")]
public static class DeviceFilters
{
    public const string LabService = "qcachelab";
    private static readonly Guid DiskClass = new("4d36e967-e325-11ce-bfc1-08002be10318");
    public sealed record Snapshot(string InstanceId, string DriverKey, string[] UpperFilters);

    public static Snapshot Inspect(string instanceId) => Access(instanceId, null, null);
    public static Snapshot Change(string instanceId, string expectedDriverKey, bool add) =>
        Access(instanceId, expectedDriverKey, add);

    private static Snapshot Access(string instanceId, string? expectedKey, bool? add)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(instanceId);
        var set = Native.SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try
        {
            var device = new DeviceInfo { Size = (uint)Marshal.SizeOf<DeviceInfo>() };
            if (!Native.SetupDiOpenDeviceInfoW(set, instanceId, IntPtr.Zero, 0, ref device)) throw new Win32Exception();
            if (device.ClassGuid != DiskClass) throw new ArgumentException("Target must be a disk devnode.");
            var keyBytes = Read(set, ref device, 9, 1) ?? throw new IOException("Disk has no driver key.");
            var key = Encoding.Unicode.GetString(keyBytes).TrimEnd('\0');
            var filters = DecodeMultiString(Read(set, ref device, 17, 7));
            if (add is not null)
            {
                if (!string.Equals(key, expectedKey, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("Disk driver key changed; refusing attachment change.");
                var changed = Plan(filters, add.Value);
                var bytes = changed.Length == 0 ? null : Encoding.Unicode.GetBytes(string.Join('\0', changed) + "\0\0");
                if (!Native.SetupDiSetDeviceRegistryPropertyW(set, ref device, 17, bytes, (uint)(bytes?.Length ?? 0)))
                    throw new Win32Exception();
                filters = DecodeMultiString(Read(set, ref device, 17, 7));
                if (!filters.SequenceEqual(changed, StringComparer.OrdinalIgnoreCase))
                    throw new IOException("UpperFilters read-back verification failed.");
            }
            return new(instanceId, key, filters);
        }
        finally { Native.SetupDiDestroyDeviceInfoList(set); }
    }

    // Pure list operation used by tests. Preserve other drivers and their ordering.
    public static string[] Plan(IEnumerable<string> filters, bool add)
    {
        var items = filters.ToList();
        if (add)
        {
            if (!items.Contains(LabService, StringComparer.OrdinalIgnoreCase)) items.Add(LabService);
        }
        else items.RemoveAll(s => s.Equals(LabService, StringComparison.OrdinalIgnoreCase));
        return items.ToArray();
    }

    private static string[] DecodeMultiString(byte[]? bytes)
    {
        if (bytes is null) return [];
        if (bytes.Length < 4 || bytes.Length % 2 != 0 ||
            bytes[^1] != 0 || bytes[^2] != 0 || bytes[^3] != 0 || bytes[^4] != 0)
            throw new IOException("Malformed UpperFilters property.");
        return Encoding.Unicode.GetString(bytes).Split('\0', StringSplitOptions.RemoveEmptyEntries);
    }

    private static byte[]? Read(IntPtr set, ref DeviceInfo info, uint property, uint expectedType)
    {
        var bytes = new byte[65536];
        if (!Native.SetupDiGetDeviceRegistryPropertyW(set, ref info, property, out var type,
                bytes, (uint)bytes.Length, out var size))
        {
            var error = Marshal.GetLastWin32Error();
            if (error == 13) return null; // ERROR_INVALID_DATA: property absent.
            throw new Win32Exception(error);
        }
        if (type != expectedType || size > bytes.Length || size % 2 != 0)
            throw new IOException("Unexpected device property type/length.");
        return bytes[..(int)size];
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInfo { public uint Size; public Guid ClassGuid; public uint DevInst; public UIntPtr Reserved; }
    private static class Native
    {
        [DllImport("setupapi.dll", SetLastError = true)]
        internal static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classGuid, IntPtr parent);
        [DllImport("setupapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string id, IntPtr parent, uint flags, ref DeviceInfo info);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo info, uint property,
            out uint type, [Out] byte[] bytes, uint capacity, out uint required);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo info,
            uint property, byte[]? bytes, uint size);
    }
}
