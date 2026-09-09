using QueueCache.Management;
using System.Runtime.Versioning;

namespace QueueCache.Operations;

public enum CachePreset { Fast, Strict }
public sealed record CacheConfiguration(int BudgetMiB = 4096, CachePreset Preset = CachePreset.Fast, bool Enabled = true)
{
    public void Validate(bool acceptVolatileFlush)
    {
        if (BudgetMiB is < 1 or > 4096) throw new ArgumentException("Budget must be 1..4096 MiB.");
        if (!Enum.IsDefined(Preset)) throw new ArgumentException("Unknown cache preset.");
        if (Preset == CachePreset.Fast && !acceptVolatileFlush)
            throw new ArgumentException("Fast mode acknowledges writes/flushes in volatile RAM. Explicitly accept volatile flushes first.");
    }
}

/// <summary>Both frontends use this orchestrator. Failed changes stop immediately; no fictitious rollback.</summary>
[SupportedOSPlatform("windows")]
public static class ConfigurationManager
{
    public static WriteCacheState Apply(DiskTarget target, CacheConfiguration configuration,
        bool acceptVolatileFlush, IProgress<string>? progress = null)
    {
        configuration.Validate(acceptVolatileFlush);
        using var device = new CacheDevice(target.Device, writable: true);
        var state = device.GetWriteCacheState();
        EnsureHealthy(state);
        if (state.DeviceBytes != (ulong)target.Bytes) throw new IOException("Disk size changed.");
        var budget = (ulong)configuration.BudgetMiB << 20;
        if (state.Enabled == configuration.Enabled && state.BudgetBytes == budget &&
            state.UnsafeDefer == (configuration.Preset == CachePreset.Fast)) return state;
        progress?.Report("Draining and disabling before applying configuration. This may take time.");
        device.Control(WriteCacheAction.Disable);
        progress?.Report("Applying preset and RAM budget.");
        device.Control(WriteCacheAction.FlushPolicy, value: configuration.Preset == CachePreset.Fast ? 1UL : 0UL);
        if (state.BudgetBytes != budget) device.Control(WriteCacheAction.Configure, budget);
        if (configuration.Enabled) device.Control(WriteCacheAction.Enable);
        var result = device.GetWriteCacheState();
        EnsureHealthy(result);
        if (result.Enabled != configuration.Enabled || result.BudgetBytes != budget ||
            result.UnsafeDefer != (configuration.Preset == CachePreset.Fast)) throw new IOException("Applied state does not match requested configuration.");
        progress?.Report("Configuration applied and verified.");
        return result;
    }

    public static void EnsureHealthy(WriteCacheState state)
    {
        if (state.Faulted || state.LastError != 0 || state.Removed || state.Suspended || state.Draining)
            throw new IOException("Cache unavailable, faulted or busy draining. Inspect status; no automatic recovery performed.");
    }
}
