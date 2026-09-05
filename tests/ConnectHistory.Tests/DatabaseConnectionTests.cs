using MySqlConnector;
using Xunit;

namespace ConnectHistory.Tests;

/// Строку подключения нельзя собирать интерполяцией: пароли содержат спецсимволы,
/// а ';' в такой строке — это уже подмена параметров подключения.
public class DatabaseConnectionTests
{
    private static DatabaseConfig Config(string password) => new()
    {
        Host = "db.example.com",
        Port = 3306,
        Database = "history",
        User = "ch_user",
        Password = password,
        SslMode = "Required"
    };

    [Fact]
    public void PasswordWithSemicolon_DoesNotInjectConnectionOptions()
    {
        var evil = "p@ss;Allow User Variables=true;SslMode=None";
        var built = DatabaseService.BuildConnectionString(Config(evil));

        // Обратный разбор обязан вернуть ИСХОДНЫЙ пароль целиком,
        // а SslMode остаться тем, что задан в конфиге.
        var parsed = new MySqlConnectionStringBuilder(built);
        Assert.Equal(evil, parsed.Password);
        Assert.Equal(MySqlSslMode.Required, parsed.SslMode);
    }

    [Fact]
    public void PasswordWithBackslashesAndCommas_SurvivesRoundTrip()
    {
        var password = @"aB3+x9\Qw?Er[_,Ty$Ui/Op%As#Df";
        var parsed = new MySqlConnectionStringBuilder(DatabaseService.BuildConnectionString(Config(password)));
        Assert.Equal(password, parsed.Password);
    }

    [Fact]
    public void ConnectionString_PinsDateTimeKindToUtc()
    {
        // MySQL хранит DATETIME без пояса, поэтому «в базе UTC» — соглашение,
        // которое надо чем-то держать. С DateTimeKind=Utc драйвер откажется писать
        // локальное время и пометит прочитанное как Utc.
        var parsed = new MySqlConnectionStringBuilder(DatabaseService.BuildConnectionString(Config("pwd")));
        Assert.Equal(MySqlDateTimeKind.Utc, parsed.DateTimeKind);
    }

    [Fact]
    public void UnknownSslMode_FallsBackToPreferred_NotNone()
        => Assert.Equal(MySqlSslMode.Preferred, DatabaseService.ParseSslMode("totally-wrong"));

    [Theory]
    [InlineData("ch_", "ch_")]
    [InlineData("", "")]
    [InlineData("stats2_", "stats2_")]
    [InlineData("ch`; DROP TABLE users; --", "ch_")]
    [InlineData("ch-prefix", "ch_")]
    [InlineData("way_too_long_prefix_value", "ch_")]
    public void TablePrefix_PassesWhitelistOrFallsBack(string input, string expected)
        => Assert.Equal(expected, DatabaseService.SanitizePrefix(input));

    [Fact]
    public void ConnectionString_IsMaskedForLogs()
    {
        var built = DatabaseService.BuildConnectionString(Config("super-secret"));
        var masked = SqlSanitizer.Mask(built);

        Assert.DoesNotContain("super-secret", masked, System.StringComparison.Ordinal);
        Assert.Contains("password=***", masked, System.StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Mask_HandlesExceptionTextWithPwdAlias()
        => Assert.DoesNotContain("hunter2", SqlSanitizer.Mask("Access denied (server=db;pwd=hunter2;db=x)"),
            System.StringComparison.Ordinal);

    /// В .NET, как и в PCRE, якорь $ совпадает ПЕРЕД завершающим переводом строки.
    /// Пока в белом списке стоял $, префикс "ch_\n" проходил проверку и уезжал
    /// в текст SQL как часть имени таблицы — параметризовать идентификатор нельзя.
    [Theory]
    [InlineData("ch_\n")]
    [InlineData("ch_\r\n")]
    [InlineData("ch_\n; DROP TABLE ch_sessions; --")]
    public void PrefixWithTrailingNewlineIsRejected(string prefix)
        => Assert.Equal("ch_", DatabaseService.SanitizePrefix(prefix));
}
