using System.Buffers.Binary;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using QueueCache.Management;

// Never formats, partitions, deletes, or overwrites existing files. Requires a prepared NTFS lab volume.
if (!OperatingSystem.IsWindows() || args.Length is not (5 or 6) || args[0].Length != 1 || !char.IsAsciiLetter(args[0][0]) ||
    !int.TryParse(args[1], out var number) || number <= 0 || !long.TryParse(args[2], out var expectedSize) || expectedSize < (4L << 30) ||
    args[4] is not ("--write-new-files" or "--concurrent" or "--baseline" or "--dirty-reboot" or "--dirty-reboot-unsafe" or "--test-flush-policy" or "--test-coalescing" or "--verify-files") || (args[4] == "--verify-files" && args.Length != 6))
{
    Console.Error.WriteLine("Usage: qcache-file-tests <drive letter> <disk number> <exact disk bytes> <PnP instance> --write-new-files [64..8192 MiB per file] | --concurrent [MiB] | --baseline [MiB] | --dirty-reboot | --dirty-reboot-unsafe | --test-flush-policy | --test-coalescing | --verify-files <run GUID>");
    return 2;
}
try
{
    var letter = char.ToUpperInvariant(args[0][0]);
    var root = $"{letter}:\\";
    var probe = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
    probe.ArgumentList.Add("-NoProfile"); probe.ArgumentList.Add("-Command");
    probe.ArgumentList.Add($"$ErrorActionPreference='Stop'; $p=@(Get-Partition -DriveLetter {letter}); if($p.Count -ne 1){{throw 'Ambiguous volume'}}; $d=Get-Disk -Number $p[0].DiskNumber; $c=Get-CimInstance Win32_DiskDrive -Filter ('Index='+$d.Number); [pscustomobject]@{{Number=[int]$d.Number; Size=[long]$d.Size; Boot=$d.IsBoot; System=$d.IsSystem; Instance=$c.PNPDeviceID}} | ConvertTo-Json -Compress");
    using var process = Process.Start(probe) ?? throw new IOException("Cannot inspect volume.");
    var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
    try { await process.WaitForExitAsync(timeout.Token); }
    catch (OperationCanceledException) { process.Kill(); throw new IOException("Volume inspection timed out."); }
    if (process.ExitCode != 0) throw new IOException(await stderr);
    var identity = JsonSerializer.Deserialize<Identity>(await stdout) ?? throw new IOException("Missing volume identity.");
    if (identity.Number != number || identity.Size != expectedSize || identity.Boot || identity.System ||
        !string.Equals(identity.Instance, args[3], StringComparison.OrdinalIgnoreCase) ||
        !string.Equals(new DriveInfo(root).DriveFormat, "NTFS", StringComparison.OrdinalIgnoreCase))
        throw new IOException("Expected NTFS volume on the exact non-OS secondary disk.");
    var writing = args[4] != "--verify-files";
    var baseline = args[4] == "--baseline";
    long length = 64L << 20;
    if (args[4] is "--dirty-reboot" or "--dirty-reboot-unsafe" or "--test-flush-policy" or "--test-coalescing" && args.Length != 5) throw new ArgumentException("Policy/reboot/coalescing tests use a fixed 64 MiB file size.");
    if (writing && args.Length == 6)
    {
        if (!int.TryParse(args[5], out var mib) || mib < 64 || mib > 8192) throw new ArgumentException("File size must be 64..8192 MiB.");
        length = (long)mib << 20;
    }
    using var volume = new CheckedVolume(letter, number, expectedSize, writing);
    using var cache = new CacheDevice($"PhysicalDrive{number}");
    var before = cache.GetWriteCacheState();
    if (writing && before.UnsafeDefer) throw new IOException("Start file tests in strict policy. Policy-specific tests switch modes only after preparing durable metadata.");
    var runId = writing ? Guid.NewGuid() : Guid.ParseExact(args[5], "N");
    var directory = Path.Combine(root, "QueueCache-FileTest-" + runId.ToString("N"));
    if (writing)
    {
        if (before.Enabled == baseline || before.Faulted || before.DirtyBytes != 0 || before.DeviceBytes != (ulong)expectedSize)
            throw new IOException("File test requires a healthy, initially clean cache; enabled for write-new-files, disabled for baseline.");
        if (new DriveInfo(root).AvailableFreeSpace < length * 2 + (1L << 30)) throw new IOException("Need space for both files plus 1 GiB headroom.");
        if (Directory.Exists(directory) || File.Exists(directory)) throw new IOException("New test directory already exists.");
        Directory.CreateDirectory(directory);
    }
    RejectReparse(directory);
    var source = Path.Combine(directory, "source.bin");
    var copy = Path.Combine(directory, "copied-renamed.bin");
    var manifestPath = Path.Combine(directory, "manifest.json");
    const int blockBytes = 1 << 20;
    const long overwriteOffset = (1 << 20) + 4096;
    Manifest manifest;
    if (writing)
    {
        var seed = BinaryPrimitives.ReadUInt64LittleEndian(Guid.NewGuid().ToByteArray()) | 1UL;
        var block = new byte[blockBytes];
        var watch = Stopwatch.StartNew();
        var temporaryCopy = Path.Combine(directory, "copy.bin");
        // A second writer flushes and verifies its file while the first writer may still be active.
        var concurrentCopy = args[4] == "--concurrent" ? Task.Run(() => WriteAndVerifyPatternFile(temporaryCopy, seed, length)) : null;
        try
        {
            using var stream = new FileStream(source, FileMode.CreateNew, FileAccess.Write, FileShare.None, blockBytes, FileOptions.SequentialScan);
            for (long offset = 0; offset < length; offset += blockBytes) { Pattern(block, offset, seed); stream.Write(block); }
            var writeReturned = watch.Elapsed.TotalSeconds;
            stream.Flush(flushToDisk: true);
            Console.WriteLine(JsonSerializer.Serialize(new { stage = "source-written", runId, length, writeReturnSeconds = writeReturned, includingFileFlushSeconds = watch.Elapsed.TotalSeconds }));
        }
        finally
        {
            // Never leave a writer running after the parent reports failure.
            if (concurrentCopy is not null) await concurrentCopy;
        }
        if (concurrentCopy is null) File.Copy(source, temporaryCopy, overwrite: false);
        File.Move(temporaryCopy, copy, overwrite: false);
        using (var stream = new FileStream(source, FileMode.Open, FileAccess.Write, FileShare.None))
        {
            // This file was created by this invocation, never a pre-existing user file.
            stream.Position = overwriteOffset;
            var overwrite = new byte[4096]; Pattern(overwrite, overwriteOffset, seed);
            for (var i = 0; i < overwrite.Length; i++) overwrite[i] ^= 0xA7;
            stream.Write(overwrite); stream.Flush(flushToDisk: true);
        }
        manifest = new Manifest(1, runId, seed, length, HashOracle(seed, true, length), HashOracle(seed, false, length));
        using (var stream = new FileStream(manifestPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { JsonSerializer.Serialize(stream, manifest); stream.Flush(flushToDisk: true); }
        // Flush the filesystem's data AND metadata, not merely the block-cache control endpoint.
        volume.Flush();
        Console.WriteLine(JsonSerializer.Serialize(new { stage = "files-written-and-volume-flushed", runId, elapsedSeconds = watch.Elapsed.TotalSeconds }));
    }
    else
    {
        RejectReparse(manifestPath);
        if (new FileInfo(manifestPath).Length > 16384) throw new IOException("Manifest exceeds the lab metadata limit.");
        manifest = JsonSerializer.Deserialize<Manifest>(File.ReadAllText(manifestPath)) ?? throw new IOException("Missing manifest.");
        length = manifest.Length;
        if (manifest.Version is not (1 or 2) || manifest.RunId != runId || length < (64L << 20) || length > (8192L << 20) || length % blockBytes != 0 || manifest.Seed == 0 ||
            manifest.SourceHash != HashOracle(manifest.Seed, true, length, manifest.Version == 2) || manifest.CopyHash != HashOracle(manifest.Seed, false, length))
            throw new IOException("Invalid manifest or independent oracle hashes.");
    }
    foreach (var (file, expected) in new[] { (source, manifest.SourceHash), (copy, manifest.CopyHash) })
    {
        RejectReparse(file);
        using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length != length || Convert.ToHexString(SHA256.HashData(stream)) != expected) throw new IOException($"File integrity mismatch: {file}");
    }
    if (writing) volume.Flush();
    var after = cache.GetWriteCacheState();
    if (after.Faulted || writing && (after.DirtyBytes != 0 || after.InFlightBytes != 0 || (!baseline && after.AcceptedBytes <= before.AcceptedBytes) || after.Flushes <= before.Flushes))
        throw new IOException("File bytes matched but cache admission/drain/flush evidence was missing.");
    if (args[4] == "--test-coalescing")
    {
        using var control = new CacheDevice($"PhysicalDrive{number}", writable: true);
        control.Control(WriteCacheAction.Disable);
        control.Control(WriteCacheAction.FlushPolicy, value: 1);
        control.Control(WriteCacheAction.Enable);
        var initial = control.GetWriteCacheState();
        if (initial.PayloadCapacity < 128UL << 20) throw new IOException("Coalescing test requires at least 128 MiB payload capacity.");
        control.Control(WriteCacheAction.LabDelay, value: 2000);
        var payload = new byte[64 << 20];
        ulong finalSeed = 0, maximumDirty = 0;
        // A single 64 MiB extent receives 2 GiB of writes. Older in-flight data
        // must not be modified or cause a later completion to forget newer bytes.
        for (var iteration = 0; iteration < 32; iteration++)
        {
            finalSeed = manifest.Seed ^ (ulong)(iteration + 1);
            Pattern(payload, 0, finalSeed);
            UnbufferedFileWrite.WritePrefix(source, payload, writeThrough: true, flush: true);
            var observed = control.GetWriteCacheState();
            maximumDirty = Math.Max(maximumDirty, observed.DirtyBytes);
            if (observed.Faulted || observed.DirtyBytes > 68UL << 20)
                throw new IOException("Repeated 64 MiB extent exceeded the dirty working-set bound.");
            var prefix = UnbufferedFileWrite.ReadPrefix(source, 2 << 20);
            if (!prefix.AsSpan().SequenceEqual(payload.AsSpan(0, prefix.Length)))
                throw new IOException("Newest data not visible while an older version drains.");
        }
        var queued = control.GetWriteCacheState();
        if (queued.CoalescedBytes - initial.CoalescedBytes < 64UL << 20)
            throw new IOException("No meaningful overwrite coalescing observed.");
        control.Control(WriteCacheAction.LabDelay, value: 0);
        if (queued.DirtyBytes == 0) throw new IOException("No pending payload for the completion-error/retry test.");
        control.Control(WriteCacheAction.LabFault, value: 4);
        var failedCompletion = false;
        try { control.Control(WriteCacheAction.Flush); }
        catch (Win32Exception) { failedCompletion = true; }
        var faulted = control.GetWriteCacheState();
        if (!failedCompletion || !faulted.Faulted || faulted.DirtyBytes == 0)
            throw new IOException("Coalesced payload not retained after injected lower completion error.");
        control.Control(WriteCacheAction.Retry);
        var final = control.GetWriteCacheState();
        using (var verified = File.OpenRead(source))
            if (Convert.ToHexString(SHA256.HashData(verified)) != HashOracle(finalSeed, false, length))
                throw new IOException("Drained coalesced file hash mismatch.");
        if (final.Faulted || final.DirtyBytes != 0 || final.AcceptedBytes != final.DrainedBytes + final.CoalescedBytes + final.DiscardedBytes)
            throw new IOException("Coalescing conservation/drain failed.");
        control.Control(WriteCacheAction.Disable);
        control.Control(WriteCacheAction.FlushPolicy, value: 0);
        control.Control(WriteCacheAction.Enable);
        // Restore the original oracle so the retained manifest remains usable for
        // independent post-reboot verification of both files.
        Pattern(payload, 0, manifest.Seed);
        for (var i = (1 << 20) + 4096; i < (1 << 20) + 8192; i++) payload[i] ^= 0xA7;
        UnbufferedFileWrite.WritePrefix(source, payload, flush: true);
        volume.Flush();
        using (var restored = File.OpenRead(source))
            if (Convert.ToHexString(SHA256.HashData(restored)) != manifest.SourceHash) throw new IOException("Restored oracle mismatch.");
        Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[4], runId, maximumDirty, initial, queued, final, coalescedSourceHash = HashOracle(finalSeed, false, length) }));
        return 0;
    }
    if (args[4] is "--dirty-reboot" or "--dirty-reboot-unsafe" or "--test-flush-policy")
    {
        // Persist the independent expected result BEFORE admitting fresh dirty data.
        // Only files created by this invocation are modified. No automatic reboot or destructive disk I/O.
        manifest = manifest with { Version = 2, SourceHash = HashOracle(manifest.Seed, true, length, true) };
        using (var metadata = new FileStream(manifestPath, FileMode.Create, FileAccess.Write, FileShare.None))
        { JsonSerializer.Serialize(metadata, manifest); metadata.Flush(true); }
        volume.Flush();
        using var control = new CacheDevice($"PhysicalDrive{number}", writable: true);
        var testUnsafe = args[4] != "--dirty-reboot";
        if (testUnsafe)
        {
            control.Control(WriteCacheAction.Disable);
            control.Control(WriteCacheAction.FlushPolicy, value: 1);
            control.Control(WriteCacheAction.Enable);
            if (!control.GetWriteCacheState().UnsafeDefer) throw new IOException("Unsafe policy was not selected.");
        }
        var diagnosticsBefore = testUnsafe ? control.GetDiagnostics() : null;
        control.Control(WriteCacheAction.LabDelay, value: 2000);
        var prefix = new byte[2 << 20]; Pattern(prefix, 0, manifest.Seed);
        for (var i = (1 << 20) + 4096; i < (1 << 20) + 8192; i++) prefix[i] ^= 0xA7;
        for (var i = 0; i < prefix.Length; i++) prefix[i] ^= 0x5A;
        UnbufferedFileWrite.WritePrefix(source, prefix, writeThrough: testUnsafe, flush: testUnsafe);
        var dirty = control.GetWriteCacheState();
        if (dirty.DirtyBytes == 0 || dirty.Faulted) throw new IOException("No outstanding dirty payload observed; cannot claim a dirty-reboot test.");
        if (testUnsafe)
        {
            var observed = control.GetDiagnostics();
            if (observed.DeferredFlushes <= diagnosticsBefore!.DeferredFlushes || observed.DeferredWriteThroughWrites <= diagnosticsBefore.DeferredWriteThroughWrites)
                throw new IOException("No observed deferred application flush and write-through.");
            Console.WriteLine(JsonSerializer.Serialize(new { stage = "deferred-flush-and-write-through", dirty, observed }));
        }
        if (args[4] == "--test-flush-policy")
        {
            // Administrative drains must still fail visibly on lower flush failure in unsafe mode.
            var switchRejected = false;
            try { control.Control(WriteCacheAction.FlushPolicy, value: 0); }
            catch (Win32Exception) { switchRejected = true; }
            if (!switchRejected || !control.GetWriteCacheState().UnsafeDefer) throw new IOException("Policy changed while enabled/dirty.");
            control.Control(WriteCacheAction.LabDelay, value: 0);
            control.Control(WriteCacheAction.LabFault, value: 3);
            var flushFailed = false;
            try { control.Control(WriteCacheAction.Flush); }
            catch (Win32Exception) { flushFailed = true; }
            if (!flushFailed || !control.GetWriteCacheState().Faulted) throw new IOException("Manual flush masked lower failure.");
            control.Control(WriteCacheAction.Retry);
            control.Control(WriteCacheAction.Disable);
            control.Control(WriteCacheAction.FlushPolicy, value: 0);
            control.Control(WriteCacheAction.Enable);
            var final = control.GetWriteCacheState();
            using var verified = File.OpenRead(source);
            if (final.UnsafeDefer || final.Faulted || final.DirtyBytes != 0 || Convert.ToHexString(SHA256.HashData(verified)) != manifest.SourceHash)
                throw new IOException("Policy recovery/drain/integrity failed.");
            Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[4], runId, directory, final, diagnostics = control.GetDiagnostics(), manifest }));
            return 0;
        }
        Console.WriteLine(JsonSerializer.Serialize(new { result = "READY_FOR_NORMAL_REBOOT", runId, directory, dirty, manifest, utc = DateTime.UtcNow }));
        Console.WriteLine("Synthetic 2000 ms drain delay remains enabled. Restart normally now, then --verify-files this run ID. This is not yet a durability PASS.");
        return 0;
    }
    Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[4], runId, directory, identity, before, after, manifest, utc = DateTime.UtcNow }));
    return 0;
}
catch (Exception ex) { Console.Error.WriteLine("FAIL: " + ex); return 1; }

static void RejectReparse(string path)
{
    if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Reparse points are not test targets.");
}
static void WriteAndVerifyPatternFile(string path, ulong seed, long length)
{
    var block = new byte[1 << 20];
    using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, block.Length, FileOptions.SequentialScan))
    {
        for (long offset = 0; offset < length; offset += block.Length) { Pattern(block, offset, seed); stream.Write(block); }
        stream.Flush(true);
    }
    using var read = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    if (Convert.ToHexString(SHA256.HashData(read)) != HashOracle(seed, false, length)) throw new IOException("Concurrent reader integrity mismatch.");
}
static void Pattern(Span<byte> data, long offset, ulong seed)
{
    for (var i = 0; i < data.Length; i += 8)
    {
        var x = ((ulong)(offset + i) / 8 ^ seed) + 0x9e3779b97f4a7c15;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
        x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
        BinaryPrimitives.WriteUInt64LittleEndian(data.Slice(i, 8), x ^ (x >> 31));
    }
}
static string HashOracle(ulong seed, bool overwrite, long length, bool dirtyPrefix = false)
{
    using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
    var block = new byte[1 << 20];
    for (long offset = 0; offset < length; offset += block.Length)
    {
        Pattern(block, offset, seed);
        if (overwrite && offset == (1 << 20)) for (var i = 4096; i < 8192; i++) block[i] ^= 0xA7;
        if (dirtyPrefix && offset < (2 << 20)) for (var i = 0; i < block.Length; i++) block[i] ^= 0x5A;
        hash.AppendData(block);
    }
    return Convert.ToHexString(hash.GetHashAndReset());
}
internal sealed record Identity(int Number, long Size, bool Boot, bool System, string Instance);
internal sealed record Manifest(int Version, Guid RunId, ulong Seed, long Length, string SourceHash, string CopyHash);

internal sealed class CheckedVolume : IDisposable
{
    private readonly SafeFileHandle handle;
    public CheckedVolume(char letter, int disk, long expectedDiskBytes, bool writable)
    {
        handle = CreateFileW($"\\\\.\\{letter}:", writable ? 0xC0000000u : 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid) { handle.Dispose(); throw new Win32Exception(); }
        try
        {
            var extents = new byte[32];
            if (!DeviceIoControl(handle, 0x560000, IntPtr.Zero, 0, extents, (uint)extents.Length, out var returned, IntPtr.Zero)) throw new Win32Exception();
            var start = BinaryPrimitives.ReadInt64LittleEndian(extents.AsSpan(16));
            var length = BinaryPrimitives.ReadInt64LittleEndian(extents.AsSpan(24));
            if (returned != extents.Length || BinaryPrimitives.ReadUInt32LittleEndian(extents) != 1 ||
                BinaryPrimitives.ReadInt32LittleEndian(extents.AsSpan(8)) != disk || start < 0 || length <= 0 || start > expectedDiskBytes - length)
                throw new IOException("Opened volume is not wholly on the expected secondary disk.");
        }
        catch { handle.Dispose(); throw; }
    }
    public void Flush() { if (!FlushFileBuffers(handle)) throw new Win32Exception(); }
    public void Dispose() => handle.Dispose();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputBytes, [Out] byte[] output, uint outputBytes, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FlushFileBuffers(SafeFileHandle handle);
}
