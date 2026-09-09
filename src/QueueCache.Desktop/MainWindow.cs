using System.Runtime.Versioning;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Threading;
using QueueCache.Management;
using QueueCache.Operations;

namespace QueueCache.Desktop;

/// <summary>Thin desktop frontend. All cache/configuration/workload operations use the shared libraries directly.</summary>
[SupportedOSPlatform("windows")]
public sealed class MainWindow : Window
{
    private readonly ComboBox volumes = new() { Width = 130 };
    private readonly NumericUpDown budget = new() { Minimum = 1, Maximum = 4096, Value = 4096, Increment = 256, Width = 140 };
    private readonly ComboBox preset = new() { ItemsSource = Enum.GetValues<CachePreset>(), SelectedIndex = 0, Width = 130 };
    private readonly CheckBox acknowledge = new() { Content = "I accept loss/corruption after a crash, including acknowledged flushes." };
    private readonly ProgressBar bucket = new() { Minimum = 0, Maximum = 100, Height = 28 };
    private readonly TextBlock snapshot = new() { Text = "Select a secondary NTFS volume. C: / OS / paging disks are excluded.", TextWrapping = Avalonia.Media.TextWrapping.Wrap };
    private readonly TextBox log = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = Avalonia.Media.TextWrapping.Wrap, Height = 220 };
    private readonly StackPanel actions = new() { Orientation = Orientation.Horizontal, Spacing = 8 };
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private DiskTarget? target;
    private WriteCacheState? previous;
    private DateTimeOffset sampled;
    private bool sampling, busy, closing;
    private CancellationTokenSource? operation;

    public MainWindow()
    {
        Title = "QueueCache — experimental RAM write cache"; Width = 900; Height = 650; MinWidth = 760; MinHeight = 600;
        volumes.ItemsSource = DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.Fixed).Select(d => d.Name[..2]).ToArray();
        var select = Button("Inspect", Inspect);
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        controls.Children.Add(new TextBlock { Text = "Volume", VerticalAlignment = VerticalAlignment.Center }); controls.Children.Add(volumes); controls.Children.Add(select);
        controls.Children.Add(new TextBlock { Text = "Budget MiB", VerticalAlignment = VerticalAlignment.Center }); controls.Children.Add(budget); controls.Children.Add(preset);
        actions.Children.Add(Button("Apply & enable", async () =>
        {
            var selected = RequireTarget();
            var configuration = new CacheConfiguration((int)(budget.Value ?? 4096), (CachePreset)(preset.SelectedItem ?? CachePreset.Fast));
            var accepted = acknowledge.IsChecked == true;
            configuration.Validate(accepted);
            await Task.Run(() => ConfigurationManager.Apply(selected, configuration, accepted, Progress()));
        }));
        actions.Children.Add(Button("Flush", () => Control(WriteCacheAction.Flush)));
        actions.Children.Add(Button("Disable & drain", () => Control(WriteCacheAction.Disable)));
        actions.Children.Add(Button("File tests", async () => ShowReport(await DiskWorkloads.TestAsync(RequireTarget(), Progress(), operation!.Token))));
        actions.Children.Add(Button("Benchmark", async () => ShowReport(await DiskWorkloads.BenchmarkAsync(RequireTarget(), progress: Progress(), cancellationToken: operation!.Token))));
        var cancel = new Button { Content = "Cancel workload", HorizontalAlignment = HorizontalAlignment.Left };
        cancel.Click += (_, _) => operation?.Cancel();
        var panel = new StackPanel { Margin = new Thickness(24), Spacing = 16 };
        panel.Children.Add(new TextBlock { Text = "RAM write cache", FontSize = 26 });
        panel.Children.Add(controls); panel.Children.Add(acknowledge); panel.Children.Add(actions); panel.Children.Add(bucket); panel.Children.Add(snapshot);
        panel.Children.Add(new TextBlock { Text = "Closing this window does NOT disable caching. Tests retain new files; benchmark is sequential and includes data generation/copy overhead.", TextWrapping = Avalonia.Media.TextWrapping.Wrap });
        panel.Children.Add(cancel); panel.Children.Add(log); Content = panel;
        volumes.SelectionChanged += (_, _) => { target = null; previous = null; bucket.Value = 0; snapshot.Text = "Click Inspect to validate this volume before use."; };
        timer.Tick += async (_, _) => await Sample(); timer.Start();
        Closing += (_, e) =>
        {
            if (busy) { e.Cancel = true; Append("An operation is still running. Cancel the workload or wait for the drain; the window cannot close yet."); return; }
            closing = true; timer.Stop();
        };
    }
    private Button Button(string caption, Func<Task> action)
    {
        var button = new Button { Content = caption };
        button.Click += async (_, _) =>
        {
            if (busy) return;
            busy = true; actions.IsEnabled = false; volumes.IsEnabled = false; operation = new();
            try { await action(); }
            catch (OperationCanceledException) { Append("Cancelled. Files retained; caching continues."); }
            catch (Exception ex) { Append("ERROR: " + ex.Message); }
            finally { operation.Dispose(); operation = null; busy = false; actions.IsEnabled = true; volumes.IsEnabled = true; }
            await Sample();
        };
        return button;
    }
    private async Task Inspect()
    {
        target = null; previous = null;
        var inspected = await DiskTarget.InspectAsync(volumes.SelectedItem as string ?? "", operation!.Token);
        var state = await Task.Run(() => { using var device = new CacheDevice(inspected.Device); return device.GetWriteCacheState(); });
        ConfigurationManager.EnsureHealthy(state);
        target = inspected;
        Append($"Validated {inspected.Root} → {inspected.Device}, {inspected.Bytes} bytes. Driver attached.");
    }
    private DiskTarget RequireTarget() => target ?? throw new InvalidOperationException("Inspect the selected disk first.");
    private Task Control(WriteCacheAction action)
    {
        var selected = RequireTarget();
        return Task.Run(() => { using var device = new CacheDevice(selected.Device, writable: true); device.Control(action); });
    }
    private async Task Sample()
    {
        if (sampling || busy || closing || target is not { } selected) return;
        sampling = true;
        try
        {
            var state = await Task.Run(() => { using var device = new CacheDevice(selected.Device); return device.GetWriteCacheState(); });
            if (target != selected || closing) return;
            var now = DateTimeOffset.UtcNow;
            var rate = previous is null ? null : CacheTelemetry.Between(previous, state, now - sampled);
            bucket.Value = state.PayloadCapacity == 0 ? 0 : 100.0 * state.DirtyBytes / state.PayloadCapacity;
            snapshot.Text = $"{selected.Root}  {state.FlushPolicy}  Enabled: {state.Enabled}\nDirty {state.DirtyBytes / 1048576.0:F1} / {state.PayloadCapacity / 1048576.0:F1} MiB   In flight {state.InFlightBytes / 1048576.0:F2} MiB\nAccepted {rate?.AcceptedMiBPerSecond:F1} / Drained {rate?.DrainedMiBPerSecond:F1} MiB/s   Waits {state.ThrottleWaits}   Error 0x{state.LastError:X8}";
            previous = state; sampled = now;
        }
        catch (Exception ex) { snapshot.Text = "Status unavailable: " + ex.Message; }
        finally { sampling = false; }
    }
    private IProgress<string> Progress() => new Progress<string>(Append);
    private void ShowReport(WorkloadReport report) => Append(JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
    private void Append(string text)
    {
        log.Text = $"{log.Text}\n{text}";
        if (log.Text.Length > 24000) log.Text = log.Text[^24000..];
        log.CaretIndex = log.Text.Length;
    }
}
