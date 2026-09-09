using System.Buffers.Binary;
using QueueCache.Management;
using QueueCache.Operations;

// Dependency-free protocol regression checks. No driver or disk writes required.
var data = new byte[184];
BinaryPrimitives.WriteUInt32LittleEndian(data, 184);
data[4] = 1;
BinaryPrimitives.WriteInt32LittleEndian(data.AsSpan(8), unchecked((int)0xC000009A));
BinaryPrimitives.WriteInt64LittleEndian(data.AsSpan(16), 200L << 30);
BinaryPrimitives.WriteInt64LittleEndian(data.AsSpan(104), 9L << 30);
BinaryPrimitives.WriteInt64LittleEndian(data.AsSpan(136), 3L << 30);
BinaryPrimitives.WriteInt64LittleEndian(data.AsSpan(176), 4L << 30);
var s = CacheStatistics.Decode(data);
new CacheConfiguration().Validate(true);
new CacheConfiguration(64, CachePreset.Strict).Validate(false);
Reject(() => new CacheConfiguration().Validate(false), "fast preset requires risk acceptance");
Reject(() => new CacheConfiguration(0).Validate(true), "zero configuration budget");
Reject(() => new CacheConfiguration(4097).Validate(true), "oversized configuration budget");
Reject(() => new CacheConfiguration(64, (CachePreset)99).Validate(true), "unknown preset");
Check(s.Enabled && s.LastError == unchecked((int)0xC000009A), "flags and signed NTSTATUS");
Check(s.DeviceBytes == 200L << 30 && s.WrittenBytes == 9L << 30, "64-bit byte counters");
Check(s.QueueMemoryBytes == 3L << 30 && s.MaxQueueBytes == 4L << 30, "cache budgets above 2 GiB");
Reject(() => CacheStatistics.Decode(data.AsSpan(0, 183)), "short response");
data[0] = 0;
Reject(() => CacheStatistics.Decode(data), "incompatible version");
Check(DevicePath.Normalize("D:") == @"\\.\D:", "volume normalization");
Check(DevicePath.Normalize(@"\\.\PhysicalDrive1") == @"\\.\PhysicalDrive1", "disk normalization");
foreach (var invalid in new[] { @"D:\file.bin", @"\\server\share", "PhysicalDrive-1", "D:\\", "PhysicalDrive1\n" })
    Reject(() => DevicePath.Normalize(invalid), "reject non-device path " + invalid);
Console.WriteLine("All protocol/path regression checks passed.");
var cacheData = new byte[WriteCacheState.WireSize];
BinaryPrimitives.WriteUInt32LittleEndian(cacheData, 1);
BinaryPrimitives.WriteUInt32LittleEndian(cacheData.AsSpan(4), 128);
BinaryPrimitives.WriteUInt32LittleEndian(cacheData.AsSpan(8), 3);
BinaryPrimitives.WriteInt32LittleEndian(cacheData.AsSpan(12), unchecked((int)0xC0000185));
ulong[] values = [200UL << 30, 4UL << 30, 3UL << 30, 1UL << 30, 4096, 2UL << 30, 123, 9UL << 30, 8UL << 30, 19, 123456, 2, 7, 1UL << 30];
for (var i = 0; i < values.Length; i++) BinaryPrimitives.WriteUInt64LittleEndian(cacheData.AsSpan(16 + i * 8), values[i]);
var writeState = WriteCacheState.Decode(cacheData);
Check(writeState.Enabled && writeState.Faulted && writeState.LastError == unchecked((int)0xC0000185), "write-cache flags and error ABI");
Check(writeState.BudgetBytes == 4UL << 30 && writeState.AcceptedBytes == 9UL << 30 && writeState.Flushes == 7 && writeState.ThrottleWaits == 19, "write-cache 64-bit offsets");
Reject(() => WriteCacheState.Decode(cacheData.AsSpan(0, 127)), "short write-cache snapshot");
cacheData[0] = 2; Reject(() => WriteCacheState.Decode(cacheData), "write-cache version"); cacheData[0] = 1;
BinaryPrimitives.WriteUInt64LittleEndian(cacheData.AsSpan(32), 5UL << 30);
Reject(() => WriteCacheState.Decode(cacheData), "cache reserved exceeds budget");
Console.WriteLine("Write-cache ABI and bounds regression checks passed.");
var nextState = writeState with { AcceptedBytes = writeState.AcceptedBytes + (4UL << 20), DrainedBytes = writeState.DrainedBytes + (2UL << 20), ThrottleWaits = writeState.ThrottleWaits + 3 };
var rates = CacheTelemetry.Between(writeState, nextState, TimeSpan.FromSeconds(2));
Check(rates.AcceptedMiBPerSecond == 2 && rates.DrainedMiBPerSecond == 1 && rates.NewThrottleWaits == 3 && rates.FillFraction == 0.5, "telemetry rates and fill");
var resetRates = CacheTelemetry.Between(nextState, writeState, TimeSpan.FromSeconds(1));
Check(resetRates.CountersReset && resetRates.AcceptedMiBPerSecond == 0 && resetRates.DrainedMiBPerSecond == 0, "counter reset avoids unsigned underflow");
Reject(() => CacheTelemetry.Between(writeState, nextState, TimeSpan.Zero), "zero sample interval");
Console.WriteLine("Telemetry regression checks passed.");
Check(writeState.CoalescedBytes == 0, "legacy accepted/drained/dirty conservation");
Check((writeState with { AcceptedBytes = writeState.AcceptedBytes + (2UL << 30) }).CoalescedBytes == 2UL << 30, "coalesced bytes are not drained bytes");
Check((writeState with { AcceptedBytes = 0 }).CoalescedBytes == 0, "coalesced counter reset avoids underflow");
Check((writeState with { AcceptedBytes = writeState.AcceptedBytes + (2UL << 30), DiscardedBytes = 1UL << 30 }).CoalescedBytes == 1UL << 30, "discarded bytes are not coalesced bytes");
var extendedData = new byte[WriteCacheState.ExtendedWireSize];
data = new byte[WriteCacheState.WireSize];
BinaryPrimitives.WriteUInt32LittleEndian(extendedData, 2);
BinaryPrimitives.WriteUInt32LittleEndian(extendedData.AsSpan(4), 160);
BinaryPrimitives.WriteUInt64LittleEndian(extendedData.AsSpan(128), 4096);
var extendedState = WriteCacheState.DecodeExtended(extendedData);
Check(extendedState.ExtendedCountersAvailable && extendedState.DiscardedBytes == 4096, "extended snapshot counters");
Reject(() => WriteCacheState.DecodeExtended(extendedData.AsSpan(0, 159)), "short extended snapshot");
Check(!(writeState with { Flags = 0 }).UnsafeDefer && (writeState with { Flags = 32 }).FlushPolicy == "UNSAFE-DEFER", "flush policy flag");
var diagnosticsBytes = new byte[CacheDiagnostics.WireSize];
BinaryPrimitives.WriteUInt32LittleEndian(diagnosticsBytes, 1);
BinaryPrimitives.WriteUInt32LittleEndian(diagnosticsBytes.AsSpan(4), CacheDiagnostics.WireSize);
ulong[] diagnosticValues = [9, 4, 7, 3, 2, 8, 1, 6, 0x2d4804];
for (var i = 0; i < diagnosticValues.Length; i++) BinaryPrimitives.WriteUInt64LittleEndian(diagnosticsBytes.AsSpan(8 + i * 8), diagnosticValues[i]);
var diagnostics = CacheDiagnostics.Decode(diagnosticsBytes);
Check(diagnostics.DeferredFlushes == 4 && diagnostics.DeferredWriteThroughWrites == 3 && diagnostics.LastBarrierCode == 0x2d4804, "diagnostics ABI offsets");
Reject(() => CacheDiagnostics.Decode(diagnosticsBytes.AsSpan(0, 79)), "short diagnostics");
diagnosticsBytes[0] = 2; Reject(() => CacheDiagnostics.Decode(diagnosticsBytes), "diagnostics version"); diagnosticsBytes[0] = 1;
BinaryPrimitives.WriteUInt64LittleEndian(diagnosticsBytes.AsSpan(16), 10);
Reject(() => CacheDiagnostics.Decode(diagnosticsBytes), "deferred count exceeds requests");
Console.WriteLine("Flush-policy/diagnostics protocol regression checks passed.");
if (OperatingSystem.IsWindows())
{
    Check(DeviceFilters.Plan(["one", "two"], true).SequenceEqual(["one", "two", "qcachelab"]), "append lab filter");
    Check(DeviceFilters.Plan(["one", "QCACHELAB", "two"], true).SequenceEqual(["one", "QCACHELAB", "two"]), "idempotent registration");
    Check(DeviceFilters.Plan(["one", "QCACHELAB", "two"], false).SequenceEqual(["one", "two"]), "remove only lab filter");
    Check(DeviceFilters.Plan([], false).Length == 0, "empty removal");
    Console.WriteLine("Filter-list regression checks passed (no device changes).");
}

static void Check(bool value, string label) { if (!value) throw new Exception("FAIL: " + label); }
static void Reject(Action action, string label)
{
    try { action(); } catch (Exception e) when (e is InvalidDataException or ArgumentException) { return; }
    throw new Exception("FAIL: " + label);
}
