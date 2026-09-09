using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using QueueCache.Management;
using System.Runtime.Versioning;

namespace QueueCache.Cli;

[SupportedOSPlatform("windows")]
internal static class LegacyCommands
{
public static async Task<int> Execute(string[] args)
{

if (args.Length == 0 || args is ["--help"] or ["help"])
{
    Console.WriteLine("""
        QueueCache experimental lab controller
          qcache list
          qcache status <D:|PhysicalDrive1> [--json]
          qcache watch <D:|PhysicalDrive1>
          qcache cache-status <device> [--json]
          qcache diagnostics <device>
          qcache policy <device> strict
          qcache policy <device> unsafe-defer --accept-volatile-flush
          qcache configure <device> <budget MiB>
          qcache start <device> <budget MiB>  (configure and enable; e.g. 4096 = 4 GiB)
          qcache enable|flush|disable|retry <device>
          qcache lab-delay <device> <0..2000 ms>
          qcache lab-fault <device> <0=clear|1=write error|2=short write|3=flush error|4=completion error|5=short completion|6=descriptor allocation|7=partial allocation>
        Write-cache controls require the explicit lab write-cache build and elevation.
        Configure only while disabled and clean. Abrupt failure loses volatile dirty data.
        Unsafe-defer acknowledges OS flush/write-through before persistence. Manual flush/disable still drain.
        Policy changes require disabled/clean state; selected policy lasts until changed or reboot.
        Lab delay/fault commands are synthetic diagnostic hooks, not normal tuning controls.
        """);
    return 0;
}
if (!OperatingSystem.IsWindows()) { Console.Error.WriteLine("Device access requires Windows."); return 1; }
using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; stop.Cancel(); };
try
{
    // Installer plumbing. Disk safety/identity gates live in lab/Manage-Lab.ps1.
    if (args is ["lab-filter", "inspect", var instance])
    {
        Console.WriteLine(JsonSerializer.Serialize(DeviceFilters.Inspect(instance)));
        return 0;
    }
    if (args.Length == 5 && args[0] == "lab-filter" && args[1] is "add" or "remove")
    {
        if (args[4] != "--lab-installer") throw new ArgumentException("Use the lab installer with disk safety checks.");
        Console.WriteLine(JsonSerializer.Serialize(DeviceFilters.Change(args[2], args[3], args[1] == "add")));
        return 0;
    }
    if (args is ["list"])
    {
        foreach (var name in CacheDevice.EnumerateDevices()) Console.WriteLine(name);
        Console.WriteLine("Listed Windows devices; this does not imply QueueCache is attached.");
        return 0;
    }
    if (args is ["diagnostics", var diagnosticTarget])
    {
        using var device = new CacheDevice(diagnosticTarget);
        Console.WriteLine(JsonSerializer.Serialize(device.GetDiagnostics(), new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }
    if (args is ["policy", _, "strict"] or ["policy", _, "unsafe-defer", "--accept-volatile-flush"])
    {
        using var device = new CacheDevice(args[1], writable: true);
        device.Control(WriteCacheAction.FlushPolicy, value: args[2] == "strict" ? 0UL : 1UL);
        Console.WriteLine(RenderCache(device.GetWriteCacheState()));
        if (args[2] != "strict") Console.WriteLine("WARNING: successful OS flush/write-through no longer promises persistence. Normal shutdown and qcache flush/disable still drain. Abrupt failure can corrupt the filesystem.");
        return 0;
    }
    if (args.Length is 2 or 3 && args[0] == "cache-status" && (args.Length == 2 || args[2] == "--json"))
    {
        using var device = new CacheDevice(args[1]);
        var state = device.GetWriteCacheState();
        Console.WriteLine(args.Length == 3 ? JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }) : RenderCache(state));
        return 0;
    }
    if (args.Length == 2 && args[0] is "enable" or "flush" or "disable" or "retry")
    {
        using var device = new CacheDevice(args[1], writable: true);
        device.Control(Enum.Parse<WriteCacheAction>(args[0], ignoreCase: true));
        Console.WriteLine(RenderCache(device.GetWriteCacheState())); return 0;
    }
    if (args.Length == 3 && args[0] is "configure" or "start" or "lab-delay" or "lab-fault")
    {
        var configure = args[0] is "configure" or "start";
        if (!ulong.TryParse(args[2], NumberStyles.None, CultureInfo.InvariantCulture, out var amount)) throw new ArgumentException("Expected a non-negative integer.");
        if (configure && (amount < 1 || amount > 4096)) throw new ArgumentException("Budget must be 1..4096 MiB.");
        if (args[0] == "lab-delay" && amount > 2000 || args[0] == "lab-fault" && amount > 7) throw new ArgumentException("Lab hook value is outside its range.");
        using var device = new CacheDevice(args[1], writable: true);
        device.Control(configure ? WriteCacheAction.Configure : args[0] == "lab-delay" ? WriteCacheAction.LabDelay : WriteCacheAction.LabFault,
            configure ? amount * 1048576 : 0, configure ? 0 : amount);
        if (args[0] == "start") device.Control(WriteCacheAction.Enable);
        Console.WriteLine(RenderCache(device.GetWriteCacheState())); return 0;
    }
    if (args.Length is 2 or 3 && args[0] == "status" && (args.Length == 2 || args[2] == "--json"))
    {
        using var device = new CacheDevice(args[1]);
        var stats = device.GetStatistics();
        Console.WriteLine(args.Length == 3 ? JsonSerializer.Serialize(stats, new JsonSerializerOptions { WriteIndented = true }) : Render(stats));
        return 0;
    }
    if (args is ["watch", var watchTarget])
    {
        using var device = new CacheDevice(watchTarget);
        WriteCacheState? previousCache = null;
        try { previousCache = device.GetWriteCacheState(); }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1) { }
        if (previousCache is not null)
        {
            var clock = Stopwatch.StartNew(); var previousTime = clock.Elapsed.TotalSeconds;
            Console.WriteLine("Dirty includes in-flight payload; reserved RAM includes unused slots. Ctrl+C stops monitoring, NOT caching.");
            Console.WriteLine("Accepted = copied into RAM; drained = completed by lower device, not necessarily durable media. Flush errors must be checked.");
            while (!stop.IsCancellationRequested)
            {
                await Task.Delay(500, stop.Token);
                var current = device.GetWriteCacheState(); var now = clock.Elapsed.TotalSeconds; var seconds = now - previousTime;
                var rates = CacheTelemetry.Between(previousCache, current, TimeSpan.FromSeconds(seconds));
                Console.WriteLine(FormattableString.Invariant($"{DateTime.Now:HH:mm:ss} {RenderCache(current)} | Accepted {rates.AcceptedMiBPerSecond:F1} Drained {rates.DrainedMiBPerSecond:F1} MiB/s | New waits {rates.NewThrottleWaits}{(rates.CountersReset ? " COUNTERS RESET" : "")}"));
                previousCache = current; previousTime = now;
            }
            return 0;
        }
        Console.WriteLine($"Watching {device.Path}; Ctrl+C to stop. Approximate legacy counters; queue includes overhead.");
        CacheStatistics? previous = null;
        var timer = Stopwatch.StartNew();
        var lastTime = timer.Elapsed.TotalSeconds;
        while (!stop.IsCancellationRequested)
        {
            var stats = device.GetStatistics();
            var now = timer.Elapsed.TotalSeconds;
            var rate = previous is null || now <= lastTime || stats.WrittenBytes < previous.WrittenBytes
                ? "n/a" : $"{(stats.WrittenBytes - previous.WrittenBytes) / (now - lastTime) / 1048576:F1} MiB/s";
            Console.WriteLine($"{DateTime.Now:HH:mm:ss} {Render(stats)} | Incoming {rate}");
            previous = stats;
            lastTime = now;
            await Task.Delay(500, stop.Token);
        }
        return 0;
    }
    Console.Error.WriteLine("Invalid arguments. Run qcache --help.");
    return 2;
}
catch (OperationCanceledException) when (stop.IsCancellationRequested) { return 0; }
catch (Exception ex) when (ex is Win32Exception or IOException or ArgumentException)
{
    Console.Error.WriteLine(ex.Message);
    return 1;
}

}
static string Render(CacheStatistics stats)
{
    var fraction = stats.MaxQueueBytes > 0 ? Math.Clamp((double)stats.QueueMemoryBytes / stats.MaxQueueBytes, 0, 1) : 0;
    var filled = (int)Math.Round(fraction * 20);
    var bucket = new string('#', filled) + new string('-', 20 - filled);
    return string.Create(CultureInfo.InvariantCulture,
        $"[{bucket}] Queue memory {stats.QueueMemoryBytes / 1048576.0:F1}/{stats.MaxQueueBytes / 1048576.0:F1} MiB | Items {stats.QueueItems} | Enabled {stats.Enabled} | Error 0x{stats.LastError:X8}");
}

static string RenderCache(WriteCacheState s)
{
    var filled = s.PayloadCapacity == 0 ? 0 : (int)Math.Clamp(Math.Round(20.0 * s.DirtyBytes / s.PayloadCapacity), 0, 20);
    return string.Create(CultureInfo.InvariantCulture,
        $"[{new string('#', filled)}{new string('-', 20 - filled)}] Dirty {s.DirtyBytes / 1048576.0:F2}/{s.PayloadCapacity / 1048576.0:F2} MiB | In flight {s.InFlightBytes / 1048576.0:F2} | Reserved {s.ReservedBytes / 1048576.0:F2}/{s.BudgetBytes / 1048576.0:F2} | {s.FlushPolicy} Enabled {s.Enabled} Barrier {s.Draining} | Error 0x{s.LastError:X8} | Throttle waits {s.ThrottleWaits} | Coalesced {s.CoalescedBytes / 1048576.0:F1} MiB");
}
}
