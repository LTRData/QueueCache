using System.ComponentModel;
using System.Buffers.Binary;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace QueueCache.Operations;

/// <summary>Owns one unbuffered file handle and one page-aligned transfer buffer. Never targets a device.</summary>
internal sealed class AlignedFile : IDisposable
{
    private readonly SafeFileHandle handle;
    private readonly IntPtr memory;
    private readonly int capacity;
    public AlignedFile(string path, int capacity, bool create)
    {
        if (capacity <= 0 || capacity % 4096 != 0) throw new ArgumentException("Transfer buffer must be 4 KiB aligned.");
        this.capacity = capacity;
        handle = CreateFileW(path, 0xC0000000, 0, IntPtr.Zero, create ? 1u : 3u, 0x20000000, IntPtr.Zero);
        if (handle.IsInvalid) { var error = Marshal.GetLastWin32Error(); handle.Dispose(); throw new Win32Exception(error); }
        memory = VirtualAlloc(IntPtr.Zero, (nuint)capacity, 0x3000, 4);
        if (memory == IntPtr.Zero) { var error = Marshal.GetLastWin32Error(); handle.Dispose(); throw new Win32Exception(error); }
    }
    public void Write(long offset, byte[] data)
    {
        Seek(offset, data.Length);
        Marshal.Copy(data, 0, memory, data.Length);
        if (!WriteFile(handle, memory, (uint)data.Length, out var count, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (count != data.Length) throw new IOException("Short unbuffered write.");
    }
    public void Read(long offset, byte[] data)
    {
        Seek(offset, data.Length);
        if (!ReadFile(handle, memory, (uint)data.Length, out var count, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (count != data.Length) throw new IOException("Short unbuffered read.");
        Marshal.Copy(memory, data, 0, data.Length);
    }
    private void Seek(long offset, int length)
    {
        if (offset < 0 || offset % 4096 != 0 || length <= 0 || length % 4096 != 0 || length > capacity)
            throw new ArgumentException("Unaligned transfer.");
        if (!SetFilePointerEx(handle, offset, out _, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public void Flush() { if (!FlushFileBuffers(handle)) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    public void Trim(long offset, long length)
    {
        if (offset < 0 || length <= 0 || offset % 4096 != 0 || length % 4096 != 0) throw new ArgumentException("Trim must be aligned.");
        var input = new byte[24];
        BinaryPrimitives.WriteUInt32LittleEndian(input.AsSpan(4), 1);
        BinaryPrimitives.WriteInt64LittleEndian(input.AsSpan(8), offset);
        BinaryPrimitives.WriteInt64LittleEndian(input.AsSpan(16), length);
        var output = new byte[4];
        // FSCTL_FILE_LEVEL_TRIM: file-relative ranges, NEVER a raw disk IOCTL.
        if (!DeviceIoControl(handle, 0x98208, input, 24, output, 4, out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (returned != 4 || BinaryPrimitives.ReadUInt32LittleEndian(output) != 1) throw new IOException("Incomplete file trim.");
    }
    public void Dispose() { handle.Dispose(); VirtualFree(memory, 0, 0x8000); }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFilePointerEx(SafeFileHandle handle, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WriteFile(SafeFileHandle handle, IntPtr data, uint length, out uint count, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReadFile(SafeFileHandle handle, IntPtr data, uint length, out uint count, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FlushFileBuffers(SafeFileHandle handle);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, byte[] input, uint inputBytes,
        [Out] byte[] output, uint outputBytes, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAlloc(IntPtr address, nuint bytes, uint allocation, uint protection);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool VirtualFree(IntPtr address, nuint size, uint type);
}
