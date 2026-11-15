namespace XournalStudio.Services;

/// <summary>Appends to the file named by XOURNAL_STUDIO_LOG, if set (for tests).</summary>
public static class DebugLog
{
    private static readonly string? FilePath = Environment.GetEnvironmentVariable("XOURNAL_STUDIO_LOG");

    public static void Write(string message)
    {
        if (FilePath != null)
        {
            File.AppendAllText(FilePath, $"{DateTime.Now:HH:mm:ss.fff} {message}{Environment.NewLine}");
        }
    }
}
