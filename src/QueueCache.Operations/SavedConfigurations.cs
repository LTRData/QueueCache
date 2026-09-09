using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32;

namespace QueueCache.Operations;

public sealed record SavedConfiguration(int Version, string Volume, string Instance, long Bytes,
    CacheConfiguration Configuration, bool VolatileFlushAccepted)
{
    public void Validate()
    {
        if (Version != 1 || Volume is null || Volume.Length != 2 || !char.IsAsciiLetter(Volume[0]) || Volume[1] != ':' ||
            string.IsNullOrWhiteSpace(Instance) || Instance.Length > 4096 || Bytes <= 0 || Configuration is null)
            throw new InvalidDataException("Invalid or unsupported saved configuration.");
        Configuration.Validate(VolatileFlushAccepted);
    }
}
public sealed record RestoreResult(string Volume, bool Applied, string Detail);

/// <summary>Machine profiles use HKLM (administrator-writable), never user-writable startup scripts.</summary>
[SupportedOSPlatform("windows")]
public static class SavedConfigurations
{
    private const string KeyPath = @"SOFTWARE\QueueCache\Profiles";
    public static void Save(DiskTarget target, CacheConfiguration configuration, bool acceptVolatileFlush)
    {
        configuration.Validate(acceptVolatileFlush);
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.CreateSubKey(KeyPath, writable: true);
        var name = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(target.Instance.ToUpperInvariant())));
        key.SetValue(name, JsonSerializer.Serialize(new SavedConfiguration(1, $"{target.Letter}:", target.Instance,
            target.Bytes, configuration, acceptVolatileFlush)), RegistryValueKind.String);
        key.Flush();
    }

    public static IReadOnlyList<SavedConfiguration> List()
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.OpenSubKey(KeyPath);
        if (key is null) return [];
        var profiles = new List<SavedConfiguration>();
        foreach (var name in key.GetValueNames())
        {
            if (key.GetValueKind(name) != RegistryValueKind.String || key.GetValue(name) is not string json || json.Length > 16384)
                throw new InvalidDataException("Invalid saved configuration.");
            var profile = JsonSerializer.Deserialize<SavedConfiguration>(json) ?? throw new InvalidDataException("Missing profile.");
            profile.Validate();
            profiles.Add(profile);
        }
        return profiles;
    }

    public static async Task<IReadOnlyList<RestoreResult>> RestoreAsync(IProgress<string>? progress = null, CancellationToken token = default)
    {
        var results = new List<RestoreResult>();
        foreach (var profile in List())
        {
            token.ThrowIfCancellationRequested();
            try
            {
                var target = await DiskTarget.InspectAsync(profile.Volume, token);
                if (!string.Equals(target.Instance, profile.Instance, StringComparison.OrdinalIgnoreCase) || target.Bytes != profile.Bytes)
                    throw new IOException("Saved identity does not match this volume. Refusing to select another disk.");
                await Task.Run(() => ConfigurationManager.Apply(target, profile.Configuration, profile.VolatileFlushAccepted, progress), token);
                results.Add(new(profile.Volume, true, "Applied matching saved configuration."));
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex) { results.Add(new(profile.Volume, false, ex.Message)); }
        }
        return results;
    }
}
