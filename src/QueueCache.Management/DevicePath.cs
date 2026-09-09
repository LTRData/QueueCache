using System.Text.RegularExpressions;

namespace QueueCache.Management;

public static partial class DevicePath
{
    // Accept whole volumes/disks only. Never normalize an arbitrary file into a device.
    public static string Normalize(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        var name = value.StartsWith(@"\\.\", StringComparison.Ordinal) || value.StartsWith(@"\\?\", StringComparison.Ordinal)
            ? value[4..] : value;
        if (!AllowedName().IsMatch(name))
            throw new ArgumentException("Use a volume such as D: or a disk such as PhysicalDrive1.", nameof(value));
        return @"\\.\" + name;
    }

    [GeneratedRegex(@"\A(?:[A-Za-z]:|PhysicalDrive[0-9]+)\z", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex AllowedName();
}
