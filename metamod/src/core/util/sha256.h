// SHA-256 и HMAC-SHA256.
//
// Своя реализация, а не OpenSSL: ради одного хеша тянуть в игровой процесс ещё
// одну библиотеку и её версионные конфликты дороже, чем 150 строк алгоритма,
// который проверяется контрольными векторами из RFC 6234 и RFC 4231.
#pragma once

#include <cstdint>
#include <string>

namespace ch {

// Шестнадцатеричная строка в нижнем регистре, 64 символа.
std::string Sha256Hex(const std::string& data);

// HMAC-SHA256, шестнадцатеричная строка в нижнем регистре.
std::string HmacSha256Hex(const std::string& key, const std::string& data);

}  // namespace ch
