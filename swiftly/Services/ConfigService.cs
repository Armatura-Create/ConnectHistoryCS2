using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ConnectHistory;

/// Загрузка конфигурации: два JSON-файла, схемы к ним и README рядом.
///
/// JSON, а не KeyValues, ради двух вещей: схема, которую понимает редактор админа,
/// и диагностика ошибки с номером строки и позиции.
public sealed partial class ConfigService
{
    // JsonSerializerOptions дорогие в создании и потокобезопасны — держим по одному экземпляру.
    // Trailing commas и //-комментарии разрешены осознанно: это самые частые «ошибки»
    // в руками правленом конфиге, а данные из них читаются однозначно.
    private static readonly JsonSerializerOptions ReadOptions = new()
    {
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        PropertyNameCaseInsensitive = true,
        Converters = { new JsonStringEnumConverter() }
    };

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter() }
    };

    private readonly ILogger _logger;
    private readonly List<string> _failedFiles = [];

    public ConfigService(ILogger logger)
    {
        _logger = logger;
    }

    /// Каталог, из которого прочитан конфиг. Нужен командам для подсказки админу.
    public string Directory { get; private set; } = "";

    /// Принимает КАТАЛОГ конфигов целиком, а не корень сервера: SwiftlyS2 сам знает,
    /// где лежат конфиги плагина (Core.Configuration.BasePath), и складывать этот путь
    /// заново означало бы дублировать чужое соглашение и разъехаться с ним при первом
    /// же изменении в фреймворке.
    public Config LoadOrCreate(string configDirectory)
    {
        _failedFiles.Clear();

        var directory = configDirectory;
        System.IO.Directory.CreateDirectory(directory);
        Directory = directory;

        var settingsPath = Path.Combine(directory, "Settings.json");
        var messagesPath = Path.Combine(directory, "Messages.json");

        // Схемы и README перезаписываются всегда: иначе после обновления плагина они
        // продолжают описывать старую версию и врут админу. Справочные файлы — не повод
        // сорвать загрузку конфига: каталог может быть read-only.
        try
        {
            WriteSchemas(directory);
            WriteReadme(directory);
        }
        catch (Exception ex)
        {
            _logger.Error($"[Config] Не удалось обновить README.txt/*.schema.json в {directory}. " +
                          "Сама конфигурация читается как обычно", ex);
        }

        if (!File.Exists(settingsPath))
        {
            var settings = CreateDefaultSettings();

            _logger.Info("═══════════════════════════════════════════════════════════════");
            _logger.Info("  ConnectHistory: первый запуск — создаю файлы конфигурации");
            _logger.Info("═══════════════════════════════════════════════════════════════");

            SaveConfig(settingsPath, settings, "Settings.schema.json");
            ProtectSecrets(settingsPath);
            _logger.Info($"✓ Settings.json создан ({settingsPath})");

            if (!File.Exists(messagesPath))
            {
                SaveConfig(messagesPath, CreateDefaultMessages(), "Messages.schema.json");
                _logger.Info("✓ Messages.json создан");
            }

            _logger.Warn("Заполните секцию Database в Settings.json и выполните css_ch_reload");

            return Merge(settings, LoadPart<MessagesConfig>(messagesPath, "Messages.json"));
        }

        var loadedSettings = LoadPart<SettingsConfig>(settingsPath, "Settings.json");
        var loadedMessages = LoadPart<MessagesConfig>(messagesPath, "Messages.json");

        if (!File.Exists(messagesPath))
        {
            var messages = CreateDefaultMessages();
            SaveConfig(messagesPath, messages, "Messages.schema.json");
            loadedMessages = messages;
        }

        ProtectSecrets(settingsPath);

        var config = Merge(loadedSettings, loadedMessages);
        Validate(config, directory);
        return config;
    }

    /// Читает одну часть конфига. Любая проблема с файлом — не повод падать:
    /// возвращаем null, а Merge подставит значения по умолчанию.
    /// Сообщение об ошибке обязано говорить, ЧТО и ГДЕ чинить: путь, строка, позиция.
    private T? LoadPart<T>(string path, string fileName) where T : class
    {
        if (!File.Exists(path))
        {
            _logger.Info($"[Config] {fileName} не найден — используются значения по умолчанию");
            return null;
        }

        string json;
        try
        {
            json = File.ReadAllText(path);
        }
        catch (Exception ex)
        {
            _logger.Error($"[Config] {fileName}: не удалось прочитать {path}. Проверьте права доступа. " +
                          "Используются значения по умолчанию", ex);
            _failedFiles.Add(fileName);
            return null;
        }

        if (string.IsNullOrWhiteSpace(json))
        {
            _logger.Error($"[Config] {fileName} пуст ({path}). Используются значения по умолчанию");
            _failedFiles.Add(fileName);
            return null;
        }

        try
        {
            var result = JsonSerializer.Deserialize<T>(json, ReadOptions);
            if (result == null)
            {
                _logger.Error($"[Config] {fileName}: файл содержит null вместо объекта ({path})");
                _failedFiles.Add(fileName);
            }

            return result;
        }
        catch (JsonException ex)
        {
            // LineNumber нумеруется с нуля — приводим к привычному виду
            var line = ex.LineNumber.HasValue
                ? (ex.LineNumber.Value + 1).ToString(CultureInfo.InvariantCulture)
                : "?";
            var pos = ex.BytePositionInLine?.ToString(CultureInfo.InvariantCulture) ?? "?";

            _logger.Error($"[Config] {fileName}: ошибка в JSON — строка {line}, позиция {pos}. Файл: {path}. " +
                          "Весь файл проигнорирован, используются значения по умолчанию. " +
                          $"Проверьте синтаксис (лишняя/пропущенная запятая, кавычки, скобки). Подробности: {ex.Message}");
            _failedFiles.Add(fileName);
            return null;
        }
        catch (Exception ex)
        {
            _logger.Error($"[Config] {fileName}: не удалось разобрать {path}", ex);
            _failedFiles.Add(fileName);
            return null;
        }
    }

    private static Config Merge(SettingsConfig? settings, MessagesConfig? messages)
    {
        var s = settings ?? new SettingsConfig();

        return new Config
        {
            Debug = s.Debug,
            ServerId = s.ServerId,
            DefaultLang = string.IsNullOrWhiteSpace(s.DefaultLang) ? "RU" : s.DefaultLang,
            DisplayTimeZone = string.IsNullOrWhiteSpace(s.DisplayTimeZone) ? "UTC" : s.DisplayTimeZone,
            Server = s.Server ?? new ServerConfig(),
            Database = s.Database ?? new DatabaseConfig(),
            Collect = s.Collect ?? new CollectConfig(),
            Storage = s.Storage ?? new StorageConfig(),
            Commands = s.Commands ?? new CommandsConfig(),
            // Языки регистронезависимы: движок отдаёт "ru", в конфигах исторически "RU"
            Messages = ToCaseInsensitive(messages?.Messages) ?? CreateDefaultMessages().Messages!
        };
    }

    private static Dictionary<string, Dictionary<string, string>>? ToCaseInsensitive(
        Dictionary<string, Dictionary<string, string>>? source)
    {
        if (source == null) return null;

        var result = new Dictionary<string, Dictionary<string, string>>(source.Count, StringComparer.OrdinalIgnoreCase);
        foreach (var (key, translations) in source)
        {
            result[key] = translations == null
                ? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
                : new Dictionary<string, string>(translations, StringComparer.OrdinalIgnoreCase);
        }

        return result;
    }

    /// Пишет конфиг, добавляя ссылку на JSON Schema первым свойством.
    /// Редактор с поддержкой схемы даёт автодополнение полей и подсветку опечаток —
    /// это заменяет половину документации. Само "$schema" System.Text.Json игнорирует.
    private static void SaveConfig<T>(string path, T config, string schemaFile)
    {
        var json = JsonSerializer.Serialize(config, WriteOptions);
        json = json.Insert(1, $"\n  \"$schema\": \"./{schemaFile}\",");
        File.WriteAllText(path, json, Encoding.UTF8);
    }

    /// В Settings.json лежит пароль от базы. Файл читают все, у кого есть доступ
    /// к каталогу сервера, — сузим права до владельца там, где ОС это умеет.
    internal static void ProtectSecrets(string path)
    {
        if (OperatingSystem.IsWindows()) return;

        try
        {
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or PlatformNotSupportedException)
        {
            // Не смогли — не повод не работать
        }
    }

    private void Validate(Config config, string directory)
    {
        if (_failedFiles.Count > 0)
        {
            _logger.Info("===============================================================");
            _logger.Info($"  ВНИМАНИЕ: не удалось прочитать {_failedFiles.Count.ToString(CultureInfo.InvariantCulture)} файл(ов) конфигурации:");
            foreach (var file in _failedFiles) _logger.Info($"    - {file}");
            _logger.Info("  Для них взяты значения по умолчанию. Смотрите строки [ERROR] выше:");
            _logger.Info("  там указаны файл, строка и позиция ошибки.");
            _logger.Info($"  Каталог конфигов: {directory}");
            _logger.Info("  Починив файлы, примените их командой css_ch_reload.");
            _logger.Info("===============================================================");
        }

        if (string.IsNullOrWhiteSpace(config.Database.Host) ||
            string.IsNullOrWhiteSpace(config.Database.Database) ||
            string.IsNullOrWhiteSpace(config.Database.User))
        {
            _logger.Error("[Config] Секция Database заполнена не полностью (Host/Database/User). " +
                          "История подключений собираться не будет");
        }

        if (config.ServerId <= 0)
            _logger.Warn("[Config] ServerId <= 0. Разным серверам нужны разные ServerId, " +
                         "иначе их сессии смешаются в одну кучу");

        if (config.Collect.IpHash && string.IsNullOrEmpty(config.Collect.IpHashSalt))
            _logger.Warn("[Config] Collect.IpHash включён, но IpHashSalt пуст. " +
                         "Хеш без соли обратим перебором IPv4 за минуты — хеширование отключено");

        if (!config.Collect.PlayerIp && !config.Collect.IpHash && !config.Collect.GeoIp)
            _logger.Info("[Config] Сбор сетевых данных полностью отключён");

        var ssl = config.Database.SslMode;
        var localDb = config.Database.Host is "127.0.0.1" or "localhost" or "::1";
        if (!localDb && DatabaseService.ParseSslMode(ssl) is MySqlConnector.MySqlSslMode.None)
        {
            _logger.Warn("[Config] SslMode=None при удалённой базе: ники, SteamID и IP игроков " +
                         "идут по сети открытым текстом. Рекомендуется Required");
        }

        _logger.Info($"✓ Конфигурация загружена из {directory}");
    }
}
