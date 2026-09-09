using System.ComponentModel;
using System.Diagnostics;
using System.Text.Json;
using QueueCache.Management;

// Read-only smoke test. A separate explicit test is required for write/flush integrity.
if (args.Length != 5 || args[4] is not ("attached" or "detached") ||
    !int.TryParse(args[1], out var diskNumber) || diskNumber <= 0 ||
    !long.TryParse(args[2], out var expectedBytes) || expectedBytes < (2L << 30))
{
    Console.Error.WriteLine("Usage: qcache-lab-tests <qcache.exe> <secondary disk number> <expected bytes> <driver key> <attached|detached>");
    return 2;
}
if (!OperatingSystem.IsWindows()) return 2;
try
{
    var target = $"PhysicalDrive{diskNumber}";
    var expectedAttached = args[4] == "attached";
    // The wrapper verifies PnP identity and boot/system/RAW flags immediately before launch.
    // This executable never opens a write handle, even when invoked without the wrapper.
    var before = await CliStats(args[0], target, expectedAttached);
    using (var device = new CacheDevice(target))
    {
        if (expectedAttached)
        {
            var stats = device.GetStatistics();
            Check(stats.DeviceBytes == expectedBytes && !stats.Enabled && stats.QueueItems == 0 && stats.LastError == 0,
                "expected healthy pass-through statistics");
        }
        else
        {
            try { device.GetStatistics(); throw new IOException("Filter remains attached."); }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 1) { }
        }
    }
    var offsets = new[] { 0L, 1L << 20, 1L << 30, expectedBytes - 65536 };
    using (var stream = new FileStream(DevicePath.Normalize(target), FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 65536))
    {
        foreach (var offset in offsets)
        {
            var first = new byte[65536];
            var second = new byte[65536];
            stream.Seek(offset, SeekOrigin.Begin); stream.ReadExactly(first);
            stream.Seek(offset, SeekOrigin.Begin); stream.ReadExactly(second);
            Check(first.AsSpan().SequenceEqual(second), $"repeated 64 KiB reads match at {offset}");
        }
    }
    var after = await CliStats(args[0], target, expectedAttached);
    if (before is not null && after is not null)
    {
        Check(after.ReadBytes - before.ReadBytes >= 524288, "driver observed test reads");
        Check(after.WrittenBytes == before.WrittenBytes, "no writes observed during read test");
        Check(after.LastError == 0 && !after.Enabled && after.QueueItems == 0, "healthy after reads");
    }
    await CliStats(args[0], "PhysicalDrive0", false);
    using (var os = new CacheDevice("PhysicalDrive0"))
    {
        try { os.GetStatistics(); throw new IOException("Unexpected QueueCache attachment on disk 0."); }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1) { }
    }
    Console.WriteLine(JsonSerializer.Serialize(new { result = "PASS", mode = args[4], diskNumber, expectedBytes,
        expectedDriverKey = args[3], bytesRead = 524288, writesIssued = 0, utc = DateTime.UtcNow }));
    return 0;
}
catch (Exception ex) { Console.Error.WriteLine("FAIL: " + ex.Message); return 1; }

static void Check(bool condition, string test)
{
    if (!condition) throw new IOException(test);
    Console.WriteLine("PASS: " + test);
}
static async Task<CacheStatistics?> CliStats(string executable, string device, bool attached)
{
    var start = new ProcessStartInfo(Path.GetFullPath(executable)) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
    foreach (var arg in new[] { "status", device, "--json" }) start.ArgumentList.Add(arg);
    using var process = Process.Start(start) ?? throw new IOException("Cannot launch qcache.exe.");
    var stdout = process.StandardOutput.ReadToEndAsync();
    var stderr = process.StandardError.ReadToEndAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
    try { await process.WaitForExitAsync(timeout.Token); }
    catch (OperationCanceledException) { process.Kill(); throw new IOException("qcache.exe timed out."); }
    var output = await stdout;
    var error = await stderr;
    Console.WriteLine($"> qcache.exe status {device} --json (exit {process.ExitCode})");
    if (attached)
    {
        Check(process.ExitCode == 0, "CLI statistics succeeded: " + error);
        return JsonSerializer.Deserialize<CacheStatistics>(output) ?? throw new IOException("Empty CLI JSON.");
    }
    Check(process.ExitCode == 1, "CLI rejected statistics on unfiltered device");
    return null;
}
