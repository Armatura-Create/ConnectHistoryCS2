using System;
using System.IO;
using System.Text.Json;
using Xunit;

namespace ConnectHistory.Tests;

/// Конфиг правит человек руками на боевом сервере, и он ошибётся в запятой.
/// Битый файл не имеет права ронять плагин и не имеет права быть перезаписанным.
public class ConfigResilienceTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
    private string ConfigDir => Path.Combine(_root, "configs/plugins/ConnectHistory");

    public void Dispose()
    {
        if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true);
        GC.SuppressFinalize(this);
    }

    private Config Load() => new ConfigService(new NullLogger()).LoadOrCreate(_root);

    [Fact]
    public void FirstRun_CreatesConfigsSchemasAndReadme()
    {
        var config = Load();

        Assert.True(File.Exists(Path.Combine(ConfigDir, "Settings.json")));
        Assert.True(File.Exists(Path.Combine(ConfigDir, "Messages.json")));
        Assert.True(File.Exists(Path.Combine(ConfigDir, "Settings.schema.json")));
        Assert.True(File.Exists(Path.Combine(ConfigDir, "Messages.schema.json")));
        Assert.True(File.Exists(Path.Combine(ConfigDir, "README.txt")));

        // Дефолты обезличены: первый запуск не должен ломиться в чужую базу
        Assert.Equal(string.Empty, config.Database.User);
        Assert.Equal("ch_", config.Database.TablePrefix);

        // Пояс отображения обязан быть виден админу в файле, а не только в схеме
        Assert.Equal("UTC", config.DisplayTimeZone);
        Assert.Contains("\"DisplayTimeZone\"", File.ReadAllText(Path.Combine(ConfigDir, "Settings.json")),
            StringComparison.Ordinal);
    }

    [Fact]
    public void DisplayTimeZone_IsReadFromTheFile()
    {
        Load();
        File.WriteAllText(Path.Combine(ConfigDir, "Settings.json"),
            "{ \"DisplayTimeZone\": \"Europe/Moscow\" }");

        var config = Load();

        Assert.Equal("Europe/Moscow", config.DisplayTimeZone);
        Assert.Equal(TimeSpan.FromHours(3),
            TimeZoneResolver.Resolve(config.DisplayTimeZone).GetUtcOffset(DateTime.UtcNow));
    }

    [Fact]
    public void GeneratedConfig_ReferencesItsSchemaAndStaysReadable()
    {
        Load();

        var json = File.ReadAllText(Path.Combine(ConfigDir, "Settings.json"));
        Assert.Contains("\"$schema\": \"./Settings.schema.json\"", json, StringComparison.Ordinal);

        // "$schema" не описано в моделях, и System.Text.Json обязан его молча игнорировать
        var parsed = JsonSerializer.Deserialize<SettingsConfig>(json,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        Assert.NotNull(parsed);
    }

    [Fact]
    public void BrokenJson_DoesNotThrowAndDoesNotOverwriteTheFile()
    {
        Load();

        var path = Path.Combine(ConfigDir, "Settings.json");
        const string broken = "{ \"ServerId\": 3,, }";
        File.WriteAllText(path, broken);

        var config = Load();

        Assert.Equal(broken, File.ReadAllText(path));   // файл админа не тронут
        Assert.Equal(1, config.ServerId);               // взяты значения по умолчанию
        Assert.NotEmpty(config.Messages);
    }

    [Fact]
    public void CommentsAndTrailingCommasAreAccepted()
    {
        Load();

        File.WriteAllText(Path.Combine(ConfigDir, "Settings.json"), """
            {
              // самая частая "ошибка" в руками правленом конфиге
              "ServerId": 42,
              "Database": { "Host": "10.0.0.9", },
            }
            """);

        var config = Load();
        Assert.Equal(42, config.ServerId);
        Assert.Equal("10.0.0.9", config.Database.Host);
    }

    [Fact]
    public void MissingSections_FallBackToDefaultsInsteadOfNulls()
    {
        Load();
        File.WriteAllText(Path.Combine(ConfigDir, "Settings.json"), "{ \"ServerId\": 5 }");

        var config = Load();

        Assert.Equal(5, config.ServerId);
        Assert.NotNull(config.Collect);
        Assert.NotNull(config.Storage);
        Assert.NotNull(config.Commands);
        Assert.True(config.Storage.SpoolEnabled);
    }

    [Fact]
    public void SettingsFile_IsNotWorldReadable()
    {
        if (OperatingSystem.IsWindows()) return;

        Load();
        var mode = File.GetUnixFileMode(Path.Combine(ConfigDir, "Settings.json"));

        // В файле пароль от базы — группа и остальные не должны его читать
        Assert.False(mode.HasFlag(UnixFileMode.GroupRead));
        Assert.False(mode.HasFlag(UnixFileMode.OtherRead));
    }
}
