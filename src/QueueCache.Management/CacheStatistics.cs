using System.Buffers.Binary;

namespace QueueCache.Management;

/// <summary>Snapshot of the legacy driver's x64 ABI. Counters are approximate, not atomic.</summary>
public sealed record CacheStatistics(
    bool Enabled, int LastError, long DeviceBytes, long ReadBytes, long WrittenBytes,
    long QueueItems, long QueueMemoryBytes, long PeakQueueMemoryBytes,
    int PagingPathCount, long LowMemoryQueued, long MaxQueueItems, long MaxQueueBytes)
{
    public const int WireSize = 184;

    public static CacheStatistics Decode(ReadOnlySpan<byte> data)
    {
        if (data.Length != WireSize)
            throw new InvalidDataException($"Statistics response must be {WireSize} bytes; got {data.Length}.");
        if (BinaryPrimitives.ReadUInt32LittleEndian(data) != WireSize)
            throw new InvalidDataException("Driver statistics ABI does not match this controller.");
        return new(data[4] != 0, I32(data, 8), I64(data, 16), I64(data, 32), I64(data, 104),
            I64(data, 120), I64(data, 136), I64(data, 144), I32(data, 152),
            I64(data, 160), I64(data, 168), I64(data, 176));
    }

    private static long I64(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadInt64LittleEndian(data[offset..]);
    private static int I32(ReadOnlySpan<byte> data, int offset) => BinaryPrimitives.ReadInt32LittleEndian(data[offset..]);
}
