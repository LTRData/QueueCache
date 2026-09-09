using System.Buffers.Binary;

namespace QueueCache.Management;

/// <summary>Versioned, coherent write-cache snapshot. Dirty includes in-flight payload, not unique disk blocks.</summary>
public sealed record WriteCacheState(uint Flags, int LastError, ulong DeviceBytes, ulong BudgetBytes,
    ulong ReservedBytes, ulong DirtyBytes, ulong InFlightBytes, ulong PayloadCapacity, ulong OccupiedSlots,
    ulong AcceptedBytes, ulong DrainedBytes, ulong ThrottleWaits, ulong CacheReadBytes, ulong Errors,
    ulong Flushes, ulong PeakDirtyBytes)
{
    public const int WireSize = 128;
    public const int ExtendedWireSize = 160;
    public ulong DiscardedBytes { get; init; }
    public ulong LowerWrites { get; init; }
    public ulong BatchedWrites { get; init; }
    public ulong TrimRequests { get; init; }
    public bool ExtendedCountersAvailable { get; init; }
    public bool Enabled => (Flags & 1) != 0;
    public bool Faulted => (Flags & 2) != 0;
    public bool Suspended => (Flags & 4) != 0;
    public bool Draining => (Flags & 8) != 0;
    public bool Removed => (Flags & 16) != 0;
    public bool UnsafeDefer => (Flags & 32) != 0;
    public string FlushPolicy => UnsafeDefer ? "UNSAFE-DEFER" : "STRICT";
    /// <summary>Accepted bytes superseded in RAM, not bytes written to the lower disk.</summary>
    public ulong CoalescedBytes => AcceptedBytes >= DrainedBytes && AcceptedBytes - DrainedBytes >= DirtyBytes &&
        AcceptedBytes - DrainedBytes - DirtyBytes >= DiscardedBytes
        ? AcceptedBytes - DrainedBytes - DirtyBytes - DiscardedBytes : 0;
    public static WriteCacheState DecodeExtended(ReadOnlySpan<byte> data)
    {
        if (data.Length != ExtendedWireSize || BinaryPrimitives.ReadUInt32LittleEndian(data) != 2 ||
            BinaryPrimitives.ReadUInt32LittleEndian(data[4..]) != ExtendedWireSize)
            throw new InvalidDataException("Unsupported extended state version/size.");
        Span<byte> legacy = stackalloc byte[WireSize]; data[..WireSize].CopyTo(legacy);
        BinaryPrimitives.WriteUInt32LittleEndian(legacy, 1); BinaryPrimitives.WriteUInt32LittleEndian(legacy[4..], WireSize);
        return Decode(legacy) with
        {
            DiscardedBytes = BinaryPrimitives.ReadUInt64LittleEndian(data[128..]),
            LowerWrites = BinaryPrimitives.ReadUInt64LittleEndian(data[136..]),
            BatchedWrites = BinaryPrimitives.ReadUInt64LittleEndian(data[144..]),
            TrimRequests = BinaryPrimitives.ReadUInt64LittleEndian(data[152..]), ExtendedCountersAvailable = true
        };
    }
    public static WriteCacheState Decode(ReadOnlySpan<byte> data)
    {
        if (data.Length != WireSize || BinaryPrimitives.ReadUInt32LittleEndian(data) != 1 ||
            BinaryPrimitives.ReadUInt32LittleEndian(data[4..]) != WireSize)
            throw new InvalidDataException("Unsupported write-cache snapshot version/size.");
        var values = new ulong[14];
        for (var i = 0; i < values.Length; i++) values[i] = BinaryPrimitives.ReadUInt64LittleEndian(data[(16 + i * 8)..]);
        var result = new WriteCacheState(BinaryPrimitives.ReadUInt32LittleEndian(data[8..]), BinaryPrimitives.ReadInt32LittleEndian(data[12..]),
            values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9], values[10], values[11], values[12], values[13]);
        if (result.ReservedBytes > result.BudgetBytes || result.DirtyBytes > result.PayloadCapacity || result.InFlightBytes > result.DirtyBytes)
            throw new InvalidDataException("Driver reported inconsistent write-cache bounds.");
        return result;
    }
}
public enum WriteCacheAction : uint { Configure = 1, Enable, Flush, Disable, Retry, LabDelay, LabFault, FlushPolicy }
