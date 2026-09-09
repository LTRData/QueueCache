using System.Buffers.Binary;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using QueueCache.Management;

// Explicitly destructive lab tool. Fixed region, no partition table writes, no file-system formatting.
ulong requestedSeed = 0;
if (args.Length >= 2 && args[^2] == "--seed")
{
    if (!ulong.TryParse(args[^1], out requestedSeed) || requestedSeed == 0) return 2;
    args = args[..^2];
}
if (!OperatingSystem.IsWindows() || args.Length is not (4 or 5 or 6) ||
    !int.TryParse(args[0], out var diskNumber) || diskNumber <= 0 ||
    !long.TryParse(args[1], out var expectedSize) || expectedSize < (4L << 30) ||
    args[3] is not ("--write-disposable-region" or "--write-and-read-disposable-region" or "--verify-only" or "--verify-base-prefix" or "--write-dirty-prefix" or "--verify-dirty-prefix" or "--write-through-check" or "--write-concurrent-check" or "--write-toggle-check" or "--write-cancellation-check" or "--write-performance-check"))
{
    Console.Error.WriteLine("Usage: qcache-write-tests <disk> <expected bytes> <PnP instance> <mode> [prefix bytes] [nonzero pattern seed]. Modes: --write-disposable-region, --write-and-read-disposable-region, --verify-only, --verify-base-prefix, --write-dirty-prefix, --verify-dirty-prefix, --write-through-check, --write-concurrent-check, --write-cancellation-check, --write-performance-check. Ordinary write/verify modes also accept trailing --seed <nonzero ulong>.");
    return 2;
}
const long regionStart = 1L << 30;
const int regionLength = 64 << 20, blockSize = 65536;
int prefixBytes = 0;
ulong patternSeed = requestedSeed;
if (args[3] is "--write-dirty-prefix" or "--verify-dirty-prefix")
{
    if (requestedSeed != 0 || args.Length != 6 || !int.TryParse(args[4], out prefixBytes) || prefixBytes <= 0 || prefixBytes > regionLength || prefixBytes % blockSize != 0 ||
        !ulong.TryParse(args[5], out patternSeed) || patternSeed == 0) return 2;
}
else if (args[3] == "--verify-base-prefix")
{
    if (args.Length != 5 || !int.TryParse(args[4], out prefixBytes) || prefixBytes <= 0 || prefixBytes > regionLength || prefixBytes % blockSize != 0) return 2;
}
else if (args.Length != 4) return 2;
if (patternSeed == 0 && args[3] is "--write-through-check" or "--write-concurrent-check" or "--write-toggle-check" or "--write-cancellation-check")
    patternSeed = BinaryPrimitives.ReadUInt64LittleEndian(Guid.NewGuid().ToByteArray()) | 1UL;
bool writing = args[3].StartsWith("--write-", StringComparison.Ordinal), exerciseCache = args[3] == "--write-and-read-disposable-region";
try
{
    // Inspect the live disk inside the executable, not only in an optional wrapper.
    var probe = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
    probe.ArgumentList.Add("-NoProfile"); probe.ArgumentList.Add("-Command");
    probe.ArgumentList.Add($"$ErrorActionPreference='Stop'; $d=Get-Disk -Number {diskNumber}; $c=Get-CimInstance Win32_DiskDrive -Filter 'Index={diskNumber}'; [pscustomobject]@{{Size=[long]$d.Size; Boot=$d.IsBoot; System=$d.IsSystem; Raw=($d.PartitionStyle -eq 'RAW'); Partitions=$d.NumberOfPartitions; Logical=$d.LogicalSectorSize; Physical=$d.PhysicalSectorSize; Instance=$c.PNPDeviceID}} | ConvertTo-Json -Compress");
    using var process = Process.Start(probe) ?? throw new IOException("Cannot inspect disk.");
    var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
    try { await process.WaitForExitAsync(timeout.Token); }
    catch (OperationCanceledException) { process.Kill(); throw new IOException("Disk inspection timed out."); }
    if (process.ExitCode != 0) throw new IOException(await error);
    var identity = JsonSerializer.Deserialize<Identity>(await output) ?? throw new IOException("Missing disk identity.");
    if (identity.Size != expectedSize || identity.Boot || identity.System || !identity.Raw || identity.Partitions != 0 ||
        !string.Equals(identity.Instance, args[2], StringComparison.OrdinalIgnoreCase) ||
        identity.Logical is not (512 or 4096) || identity.Physical is not (512 or 4096))
        throw new IOException("Disk identity/empty-secondary/alignment guard refused target.");
    var properties = DeviceFilters.Inspect(identity.Instance);
    Console.WriteLine(JsonSerializer.Serialize(new { identity, properties, regionStart, regionLength }));
    WriteCacheState? cacheBefore = null;
    if (exerciseCache || args[3] is "--write-dirty-prefix" or "--write-through-check" or "--write-concurrent-check" or "--write-toggle-check" or "--write-cancellation-check" or "--write-performance-check")
    {
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}");
        cacheBefore = cache.GetWriteCacheState();
        if (!cacheBefore.Enabled || cacheBefore.Faulted || cacheBefore.DirtyBytes != 0) throw new IOException("Cache exercise requires a healthy enabled, initially clean cache.");
    }

    // An independent in-memory oracle. Pattern is stable across processes/reboots.
    var oracle = new byte[regionLength];
    for (var i = 0; i < oracle.Length; i += 8)
        BinaryPrimitives.WriteUInt64LittleEndian(oracle.AsSpan(i, 8), Mix((ulong)i / 8 ^ patternSeed));
    if (args[3] == "--write-performance-check")
    {
        if (cacheBefore!.BudgetBytes != (4 << 20)) throw new IOException("Performance comparison requires the same 4 MiB budget throughout.");
        using var disk = new RawDisk(diskNumber, expectedSize, true);
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}", writable: true);
        cache.Control(WriteCacheAction.LabDelay, value: 0);
        cache.Control(WriteCacheAction.LabFault, value: 0);
        foreach (var size in new[] { 2 << 20, regionLength })
            for (var round = 0; round < 4; round++) // First pair is warm-up, remaining three are measured.
                foreach (var enabled in round % 2 == 0 ? new[] { false, true } : new[] { true, false })
                {
                    cache.Control(WriteCacheAction.Disable);
                    if (enabled) cache.Control(WriteCacheAction.Enable);
                    patternSeed = BinaryPrimitives.ReadUInt64LittleEndian(Guid.NewGuid().ToByteArray()) | 1UL;
                    for (var i = 0; i < size; i += 8)
                        BinaryPrimitives.WriteUInt64LittleEndian(oracle.AsSpan(i, 8), Mix((ulong)i / 8 ^ patternSeed));
                    for (var i = 0; i < size; i += blockSize)
                        if (disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize))) throw new IOException("Performance oracle is not novel.");
                    var before = cache.GetWriteCacheState();
                    var timer = Stopwatch.StartNew();
                    for (var i = 0; i < size; i += blockSize) disk.Write(regionStart + i, oracle.AsSpan(i, blockSize));
                    var writeSeconds = timer.Elapsed.TotalSeconds;
                    var returned = cache.GetWriteCacheState();
                    var flushTimer = Stopwatch.StartNew();
                    disk.Flush();
                    var flushSeconds = flushTimer.Elapsed.TotalSeconds;
                    var totalSeconds = timer.Elapsed.TotalSeconds;
                    var after = cache.GetWriteCacheState();
                    for (var i = 0; i < size; i += blockSize)
                        if (!disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize))) throw new IOException("Performance data verification failed.");
                    var cachedBytes = enabled ? (ulong)size : 0;
                    if (after.Faulted || after.DirtyBytes != 0 || after.InFlightBytes != 0 || after.AcceptedBytes - before.AcceptedBytes != cachedBytes || after.DrainedBytes - before.DrainedBytes != cachedBytes)
                        throw new IOException("Performance cache accounting failed.");
                    Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], warmup = round == 0, round, enabled, bytes = size, patternSeed,
                        writeSeconds, flushSeconds, totalSeconds, dirtyAtWriteReturn = returned.DirtyBytes, throttleWaits = after.ThrottleWaits - before.ThrottleWaits,
                        reservedBytes = after.ReservedBytes, budgetBytes = after.BudgetBytes, utc = DateTime.UtcNow }));
                }
        cache.Control(WriteCacheAction.Disable);
        return 0;
    }
    if (args[3] == "--write-cancellation-check")
    {
        if (cacheBefore!.PayloadCapacity < (2 << 20) || cacheBefore.PayloadCapacity > (8 << 20) || cacheBefore.PayloadCapacity % blockSize != 0)
            throw new IOException("Cancellation test requires 2–8 MiB usable cache, divisible by 64 KiB.");
        using var ordinary = new RawDisk(diskNumber, expectedSize, true);
        using var cancelledDisk = new RawDisk(diskNumber, expectedSize, true);
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}", writable: true);
        const int cancelledOffset = regionLength - blockSize;
        var originalTarget = ordinary.Read(regionStart + cancelledOffset, blockSize);
        if (originalTarget.AsSpan().SequenceEqual(oracle.AsSpan(cancelledOffset, blockSize))) throw new IOException("Cancellation oracle is not novel.");
        foreach (var queued in new[] { true, false })
        {
            cache.Control(WriteCacheAction.LabDelay, value: 2000);
            var before = cache.GetWriteCacheState();
            var fillBytes = queued ? 2 << 20 : checked((int)before.PayloadCapacity);
            for (var i = 0; i < fillBytes; i += blockSize) ordinary.Write(regionStart + i, oracle.AsSpan(i, blockSize));
            Task? barrier = null;
            if (queued)
            {
                barrier = Task.Run(() =>
                {
                    if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
                    using var control = new CacheDevice($"PhysicalDrive{diskNumber}", writable: true);
                    control.Control(WriteCacheAction.Flush);
                });
                if (!SpinWait.SpinUntil(() => OperatingSystem.IsWindows() && cache.GetWriteCacheState().Draining, 1000)) throw new IOException("Could not establish queued-flush cancellation window.");
            }
            using (var operation = new CancelledWrite(() => cancelledDisk.Write(regionStart + cancelledOffset, oracle.AsSpan(cancelledOffset, blockSize))))
            {
                if (!queued && !SpinWait.SpinUntil(() => OperatingSystem.IsWindows() && cache.GetWriteCacheState().ThrottleWaits > before.ThrottleWaits, 1000))
                    throw new IOException("Could not establish capacity-wait cancellation window.");
                operation.CancelAndVerify();
            }
            if (barrier is not null) await barrier;
            cache.Control(WriteCacheAction.LabDelay, value: 0);
            cache.Control(WriteCacheAction.Flush);
            var after = cache.GetWriteCacheState();
            if (after.Faulted || after.DirtyBytes != 0 || after.AcceptedBytes - before.AcceptedBytes != (ulong)fillBytes ||
                after.DrainedBytes - before.DrainedBytes != (ulong)fillBytes ||
                !ordinary.Read(regionStart + cancelledOffset, blockSize).AsSpan().SequenceEqual(originalTarget))
                throw new IOException("Cancelled write was admitted, changed data, or damaged cache accounting.");
            for (var i = 0; i < fillBytes; i += blockSize)
                if (!ordinary.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize))) throw new IOException("Uncancelled prefix damaged.");
            Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], stage = queued ? "queued-behind-flush" : "waiting-for-capacity", patternSeed, fillBytes, after, utc = DateTime.UtcNow }));
        }
        return 0;
    }
    if (args[3] is "--write-concurrent-check" or "--write-toggle-check")
    {
        var toggle = args[3] == "--write-toggle-check";
        const int workers = 4, segmentBytes = regionLength / workers;
        var handles = new List<RawDisk>();
        try
        {
            for (var i = 0; i < workers; i++) handles.Add(new RawDisk(diskNumber, expectedSize, true));
            for (var i = 0; i < regionLength; i += blockSize)
                if (handles[0].Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                    throw new IOException("Concurrent oracle must differ from existing disk data.");
            long completedBytes = 0;
            var writers = Enumerable.Range(0, workers).Select(worker => Task.Run(() =>
            {
                for (var i = worker * segmentBytes; i < (worker + 1) * segmentBytes; i += blockSize)
                {
                    handles[worker].Write(regionStart + i, oracle.AsSpan(i, blockSize));
                    if (!handles[worker].Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                        throw new IOException($"Concurrent writer {worker} read mismatch at {i}.");
                    Interlocked.Add(ref completedBytes, blockSize);
                }
            })).ToArray();
            var controls = Task.Run(async () =>
            {
                if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
                using var cache = new CacheDevice($"PhysicalDrive{diskNumber}", writable: true);
                var count = 0;
                do
                {
                    cache.Control(toggle ? WriteCacheAction.Disable : WriteCacheAction.Flush); ++count;
                    if (toggle) { await Task.Delay(20); cache.Control(WriteCacheAction.Enable); }
                    await Task.Delay(20);
                } while (count < 8 && Interlocked.Read(ref completedBytes) < regionLength);
                return count;
            });
            await Task.WhenAll(writers.Append(controls));
            handles[0].Flush();
            for (var i = 0; i < regionLength; i += blockSize)
                if (!handles[0].Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                    throw new IOException($"Final concurrent oracle mismatch at {i}.");
            using var stateHandle = new CacheDevice($"PhysicalDrive{diskNumber}");
            var after = stateHandle.GetWriteCacheState();
            var accepted = after.AcceptedBytes - cacheBefore!.AcceptedBytes;
            if (completedBytes != regionLength || after.Faulted || after.DirtyBytes != 0 || after.InFlightBytes != 0 ||
                accepted == 0 || accepted > regionLength || (!toggle && accepted != regionLength) || after.DrainedBytes - cacheBefore.DrainedBytes != accepted ||
                (toggle && (accepted == regionLength || controls.Result < 2)))
                throw new IOException("Concurrent test cache accounting mismatch.");
            Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], patternSeed, workers, completedBytes, cachedBytes = accepted, directBytes = (ulong)regionLength - accepted, concurrentControlCycles = controls.Result, after, utc = DateTime.UtcNow }));
            return 0;
        }
        finally { foreach (var disk in handles) disk.Dispose(); }
    }
    if (args[3] == "--write-dirty-prefix")
    {
        if (cacheBefore!.PayloadCapacity < (ulong)prefixBytes) throw new IOException("Dirty prefix must fit in the configured cache.");
        using (var disk = new RawDisk(diskNumber, expectedSize, true))
        {
            // Reject an idempotent test before issuing ANY write: every block must be novel.
            for (var i = 0; i < prefixBytes; i += blockSize)
                if (disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                    throw new IOException($"Shutdown oracle already exists at {i}; choose a fresh seed.");
            for (var i = 0; i < prefixBytes; i += blockSize) disk.Write(regionStart + i, oracle.AsSpan(i, blockSize));
        }
        // No FlushFileBuffers and no post-write data read. Closing the raw handle must not be mistaken for a flush.
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}");
        var dirty = cache.GetWriteCacheState();
        if (dirty.Faulted || dirty.DirtyBytes == 0 || dirty.AcceptedBytes - cacheBefore.AcceptedBytes != (ulong)prefixBytes)
            throw new IOException("Could not establish acknowledged dirty data for reboot test; use a bounded lab drain delay.");
        Console.WriteLine(JsonSerializer.Serialize(new { result = "READY_FOR_REBOOT", prefixBytes, patternSeed, everyBlockNovel = true, dirty, explicitFlushIssued = false, utc = DateTime.UtcNow }));
        return 0;
    }
    if (args[3] == "--write-through-check")
    {
        const int pendingBytes = 2 << 20;
        if (cacheBefore!.PayloadCapacity < pendingBytes) throw new IOException("Write-through test needs at least 2 MiB usable cache.");
        // Open both handles before dirtying data: handle metadata queries must not become the tested barrier.
        using var ordinary = new RawDisk(diskNumber, expectedSize, true);
        using var through = new RawDisk(diskNumber, expectedSize, true, writeThrough: true);
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}");
        for (var i = 0; i < pendingBytes + blockSize; i += blockSize)
            if (ordinary.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                throw new IOException("Write-through oracle must differ from existing disk data.");
        for (var i = 0; i < pendingBytes; i += blockSize) ordinary.Write(regionStart + i, oracle.AsSpan(i, blockSize));
        var dirty = cache.GetWriteCacheState();
        if (dirty.DirtyBytes == 0 || dirty.Faulted) throw new IOException("No dirty data before write-through barrier; use lab-delay.");
        var barrier = Stopwatch.StartNew();
        through.Write(regionStart + pendingBytes, oracle.AsSpan(pendingBytes, blockSize));
        barrier.Stop();
        var after = cache.GetWriteCacheState();
        if (after.Faulted || after.DirtyBytes != 0 || after.InFlightBytes != 0 || after.Flushes <= dirty.Flushes ||
            after.AcceptedBytes - cacheBefore.AcceptedBytes != pendingBytes || after.DrainedBytes - cacheBefore.DrainedBytes != pendingBytes)
            throw new IOException("Write-through did not drain prior cache writes or was itself acknowledged into RAM.");
        for (var i = 0; i < pendingBytes + blockSize; i += blockSize)
            if (!ordinary.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                throw new IOException($"Write-through prefix mismatch at {i}.");
        Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], patternSeed, pendingBytes, directWriteBytes = blockSize, dirty, after,
            barrierSeconds = barrier.Elapsed.TotalSeconds, explicitFlushIssuedByTest = false, utc = DateTime.UtcNow }));
        return 0;
    }
    if (prefixBytes != 0)
    {
        using var disk = new RawDisk(diskNumber, expectedSize, false);
        for (var i = 0; i < prefixBytes; i += blockSize)
            if (!disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                throw new IOException($"Recovered accepted-write prefix mismatch at {i}.");
        Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], patternSeed, verifiedBytes = prefixBytes, writesIssued = 0, utc = DateTime.UtcNow }));
        return 0;
    }
    var watch = Stopwatch.StartNew();
    using (var disk = new RawDisk(diskNumber, expectedSize, writing))
    {
        if (writing)
        {
            if (patternSeed != 0)
                for (var i = 0; i < regionLength; i += blockSize)
                    if (disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                        throw new IOException("Seeded oracle already exists on disk; choose a fresh seed.");
            for (var i = 0; i < regionLength; i += blockSize)
            {
                disk.Write(regionStart + i, oracle.AsSpan(i, blockSize));
                if (exerciseCache && !disk.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                    throw new IOException($"Immediate full-block read mismatch at {i}.");
            }
            Console.WriteLine($"Base pattern written: {regionLength} bytes in {watch.Elapsed.TotalSeconds:F3}s");
        }
        // Overwrite a 4 KiB slice inside every other 64 KiB block, then read its mixed surroundings.
        for (var i = 0; i < regionLength; i += blockSize * 2)
        {
            var slice = oracle.AsSpan(i + 4096, 4096);
            for (var j = 0; j < slice.Length; j++) slice[j] ^= 0xA7;
            if (writing)
            {
                disk.Write(regionStart + i + 4096, slice);
                var actual = disk.Read(regionStart + i, blockSize);
                if (!actual.AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                    throw new IOException($"Read-after-overwrite mismatch at region offset {i}.");
            }
        }
        if (writing) disk.Flush();
    }
    using (var reopened = new RawDisk(diskNumber, expectedSize, false))
        for (var i = 0; i < regionLength; i += blockSize)
            if (!reopened.Read(regionStart + i, blockSize).AsSpan().SequenceEqual(oracle.AsSpan(i, blockSize)))
                throw new IOException($"Reopened data mismatch at region offset {i}.");
    if (cacheBefore is not null)
    {
        using var cache = new CacheDevice($"PhysicalDrive{diskNumber}");
        var after = cache.GetWriteCacheState();
        if (after.Faulted || after.DirtyBytes != 0 || after.InFlightBytes != 0 ||
            after.AcceptedBytes - cacheBefore.AcceptedBytes != 69206016 ||
            after.DrainedBytes - cacheBefore.DrainedBytes != 69206016 || after.CacheReadBytes <= cacheBefore.CacheReadBytes ||
            after.Flushes <= cacheBefore.Flushes)
            throw new IOException("Cache counters do not prove admission, RAM reads, full drain and flush.");
        Console.WriteLine(JsonSerializer.Serialize(new { cacheExercise = "PASS", before = cacheBefore, after }));
    }
    Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[3], patternSeed, diskNumber, regionStart, regionLength,
        writeBytes = args[3] == "--verify-only" ? 0 : regionLength + regionLength / (blockSize * 2) * 4096,
        exactTransferCountsChecked = true, elapsedSeconds = watch.Elapsed.TotalSeconds, utc = DateTime.UtcNow }));
    return 0;
}
catch (Exception ex) { Console.Error.WriteLine("FAIL: " + ex); return 1; }

static ulong Mix(ulong x)
{
    x += 0x9e3779b97f4a7c15;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
    x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
    return x ^ (x >> 31);
}
internal sealed record Identity(long Size, bool Boot, bool System, bool Raw, int Partitions, uint Logical, uint Physical, string Instance);

internal sealed class RawDisk : IDisposable
{
    private readonly SafeFileHandle handle;
    private readonly IntPtr buffer;
    private readonly bool writable;
    public RawDisk(int number, long expectedSize, bool write, bool writeThrough = false)
    {
        writable = write;
        // No Windows file cache; VirtualAlloc is allocation-granularity aligned.
        handle = Native.CreateFileW($"\\\\.\\PhysicalDrive{number}", write ? 0xC0000000u : 0x80000000u, 3,
            IntPtr.Zero, 3, 0x20000000u | (writeThrough ? 0x80000000u : 0), IntPtr.Zero);
        if (handle.IsInvalid) { var e = Marshal.GetLastWin32Error(); handle.Dispose(); throw new Win32Exception(e); }
        try
        {
            var length = new byte[8];
            if (!Native.DeviceIoControl(handle, 0x7405c, IntPtr.Zero, 0, length, 8, out var count, IntPtr.Zero)) throw new Win32Exception();
            if (count != 8 || BinaryPrimitives.ReadInt64LittleEndian(length) != expectedSize) throw new IOException("Opened disk length changed.");
            // Check the opened handle's device number, not just the name passed to CreateFile.
            var id = new byte[12];
            if (!Native.DeviceIoControl(handle, 0x2d1080, IntPtr.Zero, 0, id, 12, out count, IntPtr.Zero)) throw new Win32Exception();
            if (count != 12 || BinaryPrimitives.ReadInt32LittleEndian(id.AsSpan(4)) != number) throw new IOException("Opened device number mismatch.");
            buffer = Native.VirtualAlloc(IntPtr.Zero, 65536, 0x3000, 4);
            if (buffer == IntPtr.Zero) throw new Win32Exception();
        }
        catch { handle.Dispose(); throw; }
    }
    private void Seek(long offset, int length)
    {
        if (offset < (1L << 30) || offset > (1L << 30) + (64 << 20) - length ||
            length <= 0 || length > 65536 || length % 4096 != 0 || offset % 4096 != 0)
            throw new IOException("Operation outside reserved aligned test region.");
        if (!Native.SetFilePointerEx(handle, offset, out var position, 0)) throw new Win32Exception();
        if (position != offset) throw new IOException("Seek mismatch.");
    }
    public void Write(long offset, ReadOnlySpan<byte> data)
    {
        if (!writable) throw new IOException("Read-only test handle.");
        Seek(offset, data.Length);
        Marshal.Copy(data.ToArray(), 0, buffer, data.Length);
        if (!Native.WriteFile(handle, buffer, (uint)data.Length, out var count, IntPtr.Zero)) throw new Win32Exception();
        if (count != data.Length) throw new IOException($"Short write: {count}/{data.Length}.");
    }
    public byte[] Read(long offset, int length)
    {
        Seek(offset, length);
        if (!Native.ReadFile(handle, buffer, (uint)length, out var count, IntPtr.Zero)) throw new Win32Exception();
        if (count != length) throw new IOException($"Short read: {count}/{length}.");
        var data = new byte[length]; Marshal.Copy(buffer, data, 0, length); return data;
    }
    public void Flush() { if (!Native.FlushFileBuffers(handle)) throw new Win32Exception(); }
    public void Dispose() { handle.Dispose(); if (buffer != IntPtr.Zero) Native.VirtualFree(buffer, 0, 0x8000); }
    private static class Native
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        internal static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputBytes, [Out] byte[] output, uint outputBytes, out uint returned, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetFilePointerEx(SafeFileHandle handle, long offset, out long position, uint method);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ReadFile(SafeFileHandle handle, IntPtr buffer, uint length, out uint read, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool WriteFile(SafeFileHandle handle, IntPtr buffer, uint length, out uint written, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool FlushFileBuffers(SafeFileHandle handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr VirtualAlloc(IntPtr address, nuint size, uint type, uint protection);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool VirtualFree(IntPtr address, nuint size, uint type);
    }
}
