using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using QueueCache.Management;

namespace QueueCache.Operations;

public sealed record CheckResult(string Name, string Result, string Detail);
public sealed record WorkloadReport(Guid RunId, string Directory, string Mode, DateTimeOffset Started,
    IReadOnlyList<CheckResult> Checks, WriteCacheState Before, WriteCacheState After,
    long BytesWritten, double WriteSeconds, double DrainSeconds)
{
    public bool Passed => Checks.All(c => c.Result != "FAIL");
    public double WriteMBPerSecond => WriteSeconds > 0 ? BytesWritten / WriteSeconds / 1_000_000 : 0;
}

/// <summary>File-only current-boot workloads. Never reboots, formats, injects faults or changes cache policy.</summary>
[SupportedOSPlatform("windows")]
public static class DiskWorkloads
{
    public static Task<WorkloadReport> TestAsync(DiskTarget target, IProgress<string>? progress = null,
        CancellationToken cancellationToken = default) => Task.Run(() => Run(target, false, 64, 4, progress, cancellationToken), cancellationToken);

    public static Task<WorkloadReport> BenchmarkAsync(DiskTarget target, int sizeMiB = 256, int passes = 4,
        IProgress<string>? progress = null, CancellationToken cancellationToken = default)
    {
        if (sizeMiB is < 16 or > 8192 || passes is < 1 or > 64) throw new ArgumentException("Size must be 16..8192 MiB; passes 1..64.");
        return Task.Run(() => Run(target, true, sizeMiB, passes, progress, cancellationToken), cancellationToken);
    }

    private static WorkloadReport Run(DiskTarget target, bool benchmark, int sizeMiB, int passes,
        IProgress<string>? progress, CancellationToken token)
    {
        target.CheckExtents();
        using var cache = new CacheDevice(target.Device, writable: true);
        var before = cache.GetWriteCacheState();
        ConfigurationManager.EnsureHealthy(before);
        if (before.DeviceBytes != (ulong)target.Bytes) throw new IOException("Driver target mismatch.");
        var length = (long)sizeMiB << 20;
        if (new DriveInfo(target.Root).AvailableFreeSpace < length * 2 + (1L << 30)) throw new IOException("Insufficient free space (including 1 GiB headroom).");
        var id = Guid.NewGuid();
        var directory = Path.Combine(target.Root, "QueueCache-Test-" + id.ToString("N"));
        if (Directory.Exists(directory)) throw new IOException("Test directory already exists.");
        Directory.CreateDirectory(directory);
        if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) throw new IOException("Reparse test target rejected.");
        var started = DateTimeOffset.UtcNow;
        var checks = new List<CheckResult>();
        long bytesWritten = 0;
        double writeSeconds = 0, drainSeconds = 0;
        var file = Path.Combine(directory, "source.bin");
        var seed = BinaryPrimitives.ReadUInt64LittleEndian(id.ToByteArray());
        var block = new byte[1 << 20];
        var read = new byte[block.Length];
        progress?.Report($"Target {target.Root} ({target.Device}); retained files: {directory}. Policy/settings remain unchanged.");
        try
        {
            using (var data = new AlignedFile(file, block.Length, create: true))
            {
                for (var pass = 0; pass < passes; pass++)
                {
                    progress?.Report($"Pass {pass + 1}/{passes}: writing {sizeMiB} MiB of {(pass == 0 ? "new data" : "overwrites")}.");
                    var timer = Stopwatch.StartNew();
                    for (long offset = 0; offset < length; offset += block.Length)
                    {
                        token.ThrowIfCancellationRequested();
                        Pattern(block, offset, seed + (ulong)pass);
                        data.Write(offset, block); bytesWritten += block.Length;
                    }
                    writeSeconds += timer.Elapsed.TotalSeconds;
                    // Verify before any explicit flush, bypassing the Windows file cache.
                    for (long offset = 0; offset < length; offset += block.Length)
                    {
                        token.ThrowIfCancellationRequested();
                        Pattern(block, offset, seed + (ulong)pass); data.Read(offset, read);
                        if (!block.AsSpan().SequenceEqual(read)) throw new IOException($"Live read mismatch at {offset}, pass {pass}.");
                    }
                }
                checks.Add(new("write/overwrite/live-read", "PASS", "Every byte verified using unbuffered reads before explicit flush."));
                data.Flush();
            }
            progress?.Report("Waiting for explicit driver drain; cancellation cannot discard acknowledged data.");
            var drain = Stopwatch.StartNew(); cache.Control(WriteCacheAction.Flush); drainSeconds = drain.Elapsed.TotalSeconds;
            using (var data = new AlignedFile(file, block.Length, create: false))
            {
                for (long offset = 0; offset < length; offset += block.Length)
                {
                    Pattern(block, offset, seed + (ulong)passes - 1); data.Read(offset, read);
                    if (!block.AsSpan().SequenceEqual(read)) throw new IOException("Post-drain reopen mismatch.");
                }
            }
            checks.Add(new("flush/reopen", "PASS", "Explicit drain completed and all reopened bytes matched."));
            if (!benchmark)
            {
                var copy = Path.Combine(directory, "copy.bin");
                File.Copy(file, copy, overwrite: false);
                var renamed = Path.Combine(directory, "renamed.bin");
                File.Move(copy, renamed, overwrite: false);
                using var left = File.OpenRead(file); using var right = File.OpenRead(renamed);
                if (!SHA256.HashData(left).AsSpan().SequenceEqual(SHA256.HashData(right))) throw new IOException("Copy/rename hash mismatch.");
                checks.Add(new("copy/rename", "PASS", "SHA-256 matched; both newly created files retained."));
                // Delete only a file created by this invocation. Windows decides when
                // to issue TRIM; lack of a notification is SKIP, never a fabricated PASS.
                var trimFile = Path.Combine(directory, "discard-probe.bin");
                var trimBefore = cache.GetWriteCacheState();
                using (var discard = new AlignedFile(trimFile, block.Length, create: true))
                    for (long offset = 0; offset < length; offset += block.Length)
                    { token.ThrowIfCancellationRequested(); Pattern(block, offset, seed); discard.Write(offset, block); }
                File.Delete(trimFile);
                checks.Add(new("delete-new-file", "PASS", "Only this run's discard-probe.bin was deleted; source and copy retained."));
                var trimAfter = cache.GetWriteCacheState();
                checks.Add(new("TRIM-observed", trimAfter.TrimRequests > trimBefore.TrimRequests ? "PASS" : "SKIP",
                    $"Windows controls notification timing; discarded delta {trimAfter.DiscardedBytes - trimBefore.DiscardedBytes} bytes. This is not a complete range/race test."));
            }
            var after = cache.GetWriteCacheState();
            ConfigurationManager.EnsureHealthy(after);
            if (after.Errors != before.Errors || after.Enabled != before.Enabled || after.UnsafeDefer != before.UnsafeDefer || after.BudgetBytes != before.BudgetBytes)
                throw new IOException("Errors increased or configuration changed during the run.");
            checks.Add(new("settings/health", "PASS", "No new errors; enabled state, policy and budget preserved."));
            checks.Add(new("reboot/faults/raw-disk/policy-toggle", "SKIP", "Deliberately excluded from the current-boot file-only suite."));
            checks.Add(new("TRIM-races/capacity/concurrent/cancellation", "SKIP", "Not covered by this first file-only suite; dedicated coverage remains required."));
            return new(id, directory, benchmark ? "sequential-file-benchmark" : "current-boot-test", started, checks,
                before, after, bytesWritten, writeSeconds, drainSeconds);
        }
        catch (OperationCanceledException)
        {
            progress?.Report($"Cancelled; files retained at {directory}. Cache continues draining; no settings changed.");
            throw;
        }
        catch (Exception ex)
        {
            checks.Add(new("workload", "FAIL", ex.Message));
            return new(id, directory, benchmark ? "sequential-file-benchmark" : "current-boot-test", started, checks,
                before, cache.GetWriteCacheState(), bytesWritten, writeSeconds, drainSeconds);
        }
    }

    private static void Pattern(Span<byte> data, long offset, ulong seed)
    {
        for (var i = 0; i < data.Length; i += 8)
        {
            var value = ((ulong)(offset + i) / 8 ^ seed) + 0x9e3779b97f4a7c15;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9;
            value = (value ^ (value >> 27)) * 0x94d049bb133111eb;
            BinaryPrimitives.WriteUInt64LittleEndian(data[i..], value ^ (value >> 31));
        }
    }
}
