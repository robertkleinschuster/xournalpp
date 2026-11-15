using System.Text.Json;

namespace XournalStudio.Services;

/// <summary>Small JSON settings file in %LOCALAPPDATA%\XournalStudioWinUI (the app is unpackaged).</summary>
public sealed class AppSettings
{
    public string? LastFile { get; set; }

    // XOURNAL_STUDIO_CONFIG_DIR overrides the folder (for tests).
    private static string FilePath =>
        Path.Combine(Environment.GetEnvironmentVariable("XOURNAL_STUDIO_CONFIG_DIR") ??
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XournalStudioWinUI"),
            "settings.json");

    public static AppSettings Load()
    {
        try
        {
            return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath)) ?? new AppSettings();
        }
        catch (Exception)
        {
            return new AppSettings();
        }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this));
        }
        catch (Exception)
        {
            // Settings are a convenience only.
        }
    }
}
