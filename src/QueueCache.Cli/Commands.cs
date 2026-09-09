using System.CommandLine;
using System.Runtime.Versioning;
using System.Text.Json;
using QueueCache.Management;
using QueueCache.Operations;

namespace QueueCache.Cli;

[SupportedOSPlatform("windows")]
internal static class Commands
{
    public static RootCommand Create(Func<string[], Task<int>> compatibility)
    {
        var root = new RootCommand("QueueCache experimental disk cache. Fast mode is volatile; target secondary disks only.");
        var apply = new Command("apply", "Apply a complete configuration. Fast is the default preset; explicit risk acknowledgement required.");
        var volume = new Argument<string>("volume") { Description = "NTFS secondary volume, e.g. Q:" };
        ValidateVolume(volume);
        var budget = new Option<int>("--budget-mib") { DefaultValueFactory = _ => 4096 };
        var preset = new Option<CachePreset>("--preset") { DefaultValueFactory = _ => CachePreset.Fast };
        var accept = new Option<bool>("--accept-volatile-flush");
        var disabled = new Option<bool>("--disabled");
        apply.Arguments.Add(volume); apply.Options.Add(budget); apply.Options.Add(preset); apply.Options.Add(accept); apply.Options.Add(disabled);
        apply.SetAction(async (p, token) =>
        {
            var configuration = new CacheConfiguration(p.GetValue(budget), p.GetValue(preset), !p.GetValue(disabled));
            configuration.Validate(p.GetValue(accept)); // Fail before opening a disk.
            var target = await DiskTarget.InspectAsync(p.GetValue(volume)!, token);
            var state = await Task.Run(() => ConfigurationManager.Apply(target, configuration, p.GetValue(accept), new ConsoleProgress()), token);
            Console.WriteLine(JsonSerializer.Serialize(state, JsonOptions));
            return 0;
        });
        root.Subcommands.Add(apply);
        foreach (var benchmark in new[] { false, true })
        {
            var command = new Command(benchmark ? "benchmark" : "test", "File-only current-boot workload. No reboot, format, fault injection or policy changes. Files are retained.");
            var drive = new Argument<string>("volume");
            ValidateVolume(drive);
            var size = new Option<int>("--size-mib") { DefaultValueFactory = _ => 256 };
            var passes = new Option<int>("--passes") { DefaultValueFactory = _ => 4 };
            var reportPath = new Option<string>("--report") { Description = "Create a NEW JSON report (existing files are never overwritten)." };
            command.Arguments.Add(drive); command.Options.Add(reportPath);
            if (benchmark) { command.Options.Add(size); command.Options.Add(passes); }
            command.SetAction(async (p, token) =>
            {
                // Reserve output first so an invalid/existing report path fails before disk activity.
                using var output = p.GetValue(reportPath) is { } path ? new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read) : null;
                var target = await DiskTarget.InspectAsync(p.GetValue(drive)!, token);
                var progress = new ConsoleProgress();
                var report = benchmark
                    ? await DiskWorkloads.BenchmarkAsync(target, p.GetValue(size), p.GetValue(passes), progress, token)
                    : await DiskWorkloads.TestAsync(target, progress, token);
                Console.WriteLine(JsonSerializer.Serialize(report, JsonOptions));
                if (output is not null) { JsonSerializer.Serialize(output, report, JsonOptions); output.Flush(true); }
                return report.Passed ? 0 : 1;
            });
            root.Subcommands.Add(command);
        }
        // Preserve existing installer and operator scripts while moving their presentation incrementally.
        Add("list", [], []);
        foreach (var name in new[] { "status", "cache-status" }) Add(name, ["device"], ["--json"]);
        foreach (var name in new[] { "watch", "diagnostics", "enable", "flush", "disable", "retry" }) Add(name, ["device"], []);
        foreach (var name in new[] { "configure", "start", "lab-delay", "lab-fault" }) Add(name, ["device", "value"], []);
        Add("policy", ["device", "policy"], ["--accept-volatile-flush"]);
        var filter = new Command("lab-filter", "Internal guarded installer integration.");
        foreach (var action in new[] { "inspect", "add", "remove" })
        {
            var command = new Command(action);
            var instance = new Argument<string>("instance"); command.Arguments.Add(instance);
            var service = new Argument<string>("service");
            var marker = new Option<bool>("--lab-installer");
            if (action != "inspect") { command.Arguments.Add(service); command.Options.Add(marker); }
            command.SetAction(p => compatibility(action == "inspect"
                ? ["lab-filter", action, p.GetValue(instance)!]
                : p.GetValue(marker) ? ["lab-filter", action, p.GetValue(instance)!, p.GetValue(service)!, "--lab-installer"]
                : ["lab-filter", action, p.GetValue(instance)!, p.GetValue(service)!]));
            filter.Subcommands.Add(command);
        }
        root.Subcommands.Add(filter);
        return root;

        void Add(string name, string[] argumentNames, string[] optionNames)
        {
            var command = new Command(name, name.StartsWith("lab-") ? "Advanced lab-only diagnostic hook. Not part of qcache test." : $"Cache {name}.");
            var arguments = argumentNames.Select(n => new Argument<string>(n)).ToArray();
            var options = optionNames.Select(n => new Option<bool>(n)).ToArray();
            foreach (var a in arguments) command.Arguments.Add(a);
            foreach (var o in options) command.Options.Add(o);
            command.SetAction(p => compatibility(new[] { name }.Concat(arguments.Select(a => p.GetValue(a)!))
                .Concat(options.Where(o => p.GetValue(o)).Select(o => o.Name)).ToArray()));
            root.Subcommands.Add(command);
        }
    }
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static void ValidateVolume(Argument<string> argument) => argument.Validators.Add(result =>
    {
        var value = result.GetValueOrDefault<string>();
        if (value is null || value.Length != 2 || !char.IsAsciiLetter(value[0]) || value[1] != ':')
            result.AddError("Expected an explicit volume such as Q:.");
    });
    private sealed class ConsoleProgress : IProgress<string> { public void Report(string value) => Console.Error.WriteLine(value); }
}
