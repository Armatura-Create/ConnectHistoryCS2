// Контрольные векторы SHA-256 (FIPS 180-4) и HMAC-SHA256 (RFC 4231).
//
// Своя реализация хеша имеет право существовать только вместе с ними: ошибка
// в паддинге даёт правдоподобный, но неверный хеш, и обнаружится она годы спустя,
// когда кто-то попробует сопоставить ip_hash с другой системой.
#include "core/util/sha256.h"

#include "doctest.h"

TEST_CASE("SHA-256 на контрольных векторах") {
    CHECK(ch::Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ch::Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ch::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 переживает границу блока") {
    // 55, 56 и 64 байта — ровно те длины, на которых ломается неверный паддинг:
    // 56 не оставляет места под длину в текущем блоке, 64 заполняет блок целиком.
    CHECK(ch::Sha256Hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(ch::Sha256Hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(ch::Sha256Hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("HMAC-SHA256 на векторах RFC 4231") {
    // Case 1: ключ из 20 байт 0x0b, данные "Hi There"
    CHECK(ch::HmacSha256Hex(std::string(20, '\x0b'), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Case 2: ключ "Jefe"
    CHECK(ch::HmacSha256Hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Case 6: ключ длиннее блока (131 байт) — должен сначала хешироваться
    CHECK(ch::HmacSha256Hex(std::string(131, '\xaa'),
                            "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}
