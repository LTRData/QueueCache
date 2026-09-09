using System.Buffers.Binary;

namespace QueueCache.Management;

/// <summary>Lifetime request counters; deferred flushes are not durable flush completions.</summary>
public sealed record CacheDiagnostics(ulong ApplicationFlushes, ulong DeferredFlushes, ulong WriteThroughWrites,
    ulong DeferredWriteThroughWrites, ulong ControlBarriers, ulong OtherBarriers, ulong ShutdownBarriers,
    ulong PowerBarriers, ulong LastBarrierCode)
{
    public const int WireSize = 80;
    public static CacheDiagnostics Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != WireSize || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != 1 ||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[4..]) != WireSize)
            throw new InvalidDataException("Unsupported diagnostics version/size.");
        var values = new ulong[9];
        for (var i = 0; i < values.Length; i++) values[i] = BinaryPrimitives.ReadUInt64LittleEndian(bytes[(8 + i * 8)..]);
        if (values[1] > values[0] || values[3] > values[2]) throw new InvalidDataException("Inconsistent deferred request counters.");
        return new(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
    }
}
