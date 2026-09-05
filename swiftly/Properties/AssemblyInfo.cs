using System.Runtime.CompilerServices;

// Тесты проверяют внутреннюю логику: арифметику сессий, сборку строки подключения
// и её маскирование. Публичный API ради тестов не расширяем.
[assembly: InternalsVisibleTo("ConnectHistory.Swiftly.Tests")]
