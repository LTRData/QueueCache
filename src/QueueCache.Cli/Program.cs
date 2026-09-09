using QueueCache.Cli;
using System.CommandLine;

if (!OperatingSystem.IsWindows()) { Console.Error.WriteLine("QueueCache requires Windows."); return 1; }
try
{
    var root = Commands.Create(LegacyCommands.Execute);
    var parsed = root.Parse(args.Length == 0 ? ["--help"] : args);
    if (parsed.Errors.Count != 0)
    {
        foreach (var error in parsed.Errors) Console.Error.WriteLine(error.Message);
        return 2;
    }
    return await parsed.InvokeAsync(new InvocationConfiguration { EnableDefaultExceptionHandler = false });
}
catch (OperationCanceledException) { Console.Error.WriteLine("Cancelled. Caching continues; inspect status."); return 130; }
catch (Exception ex) { Console.Error.WriteLine(ex.Message); return 1; }
