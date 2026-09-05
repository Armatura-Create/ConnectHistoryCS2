#include "sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace ch {
namespace {

constexpr size_t kBlockSize = 64;
constexpr size_t kDigestSize = 32;

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t Ror(uint32_t v, uint32_t n) { return (v >> n) | (v << (32 - n)); }

class Sha256 {
public:
    void Update(const uint8_t* data, size_t length) {
        _bitLength += static_cast<uint64_t>(length) * 8;

        while (length > 0) {
            const size_t take = std::min(kBlockSize - _bufferLength, length);
            std::memcpy(_buffer.data() + _bufferLength, data, take);
            _bufferLength += take;
            data += take;
            length -= take;

            if (_bufferLength == kBlockSize) {
                Compress(_buffer.data());
                _bufferLength = 0;
            }
        }
    }

    // Дополнение по стандарту: 0x80, нули до 56 байт в блоке, затем длина
    // сообщения в битах, big-endian. Паддинг пишется прямо в буфер, а не через
    // Update: та учла бы его в счётчике длины, и в хвост уехало бы неверное число.
    void Final(uint8_t* digest) {
        const uint64_t bitLength = _bitLength;

        _buffer[_bufferLength++] = 0x80;

        // Длина не помещается в текущий блок — дожимаем его нулями и начинаем новый
        if (_bufferLength > 56) {
            while (_bufferLength < kBlockSize) _buffer[_bufferLength++] = 0x00;
            Compress(_buffer.data());
            _bufferLength = 0;
        }

        while (_bufferLength < 56) _buffer[_bufferLength++] = 0x00;

        for (int i = 7; i >= 0; --i) {
            _buffer[_bufferLength++] = static_cast<uint8_t>((bitLength >> (i * 8)) & 0xFF);
        }
        Compress(_buffer.data());

        for (size_t i = 0; i < 8; ++i) {
            digest[i * 4 + 0] = static_cast<uint8_t>((_state[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((_state[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((_state[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>(_state[i] & 0xFF);
        }
    }

private:
    void Compress(const uint8_t* block) {
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
        uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
        _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
    }

    std::array<uint32_t, 8> _state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<uint8_t, kBlockSize> _buffer{};
    size_t _bufferLength = 0;
    uint64_t _bitLength = 0;
};

std::string ToHexLower(const uint8_t* bytes, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return out;
}

void RawSha256(const uint8_t* data, size_t length, uint8_t* digest) {
    Sha256 sha;
    sha.Update(data, length);
    sha.Final(digest);
}

}  // namespace

std::string Sha256Hex(const std::string& data) {
    uint8_t digest[kDigestSize];
    RawSha256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
    return ToHexLower(digest, kDigestSize);
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
    std::array<uint8_t, kBlockSize> paddedKey{};

    if (key.size() > kBlockSize) {
        uint8_t digest[kDigestSize];
        RawSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(), digest);
        std::memcpy(paddedKey.data(), digest, kDigestSize);
    } else {
        std::memcpy(paddedKey.data(), key.data(), key.size());
    }

    std::array<uint8_t, kBlockSize> inner{};
    std::array<uint8_t, kBlockSize> outer{};
    for (size_t i = 0; i < kBlockSize; ++i) {
        inner[i] = static_cast<uint8_t>(paddedKey[i] ^ 0x36);
        outer[i] = static_cast<uint8_t>(paddedKey[i] ^ 0x5c);
    }

    uint8_t innerDigest[kDigestSize];
    {
        Sha256 sha;
        sha.Update(inner.data(), kBlockSize);
        sha.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        sha.Final(innerDigest);
    }

    uint8_t result[kDigestSize];
    {
        Sha256 sha;
        sha.Update(outer.data(), kBlockSize);
        sha.Update(innerDigest, kDigestSize);
        sha.Final(result);
    }

    return ToHexLower(result, kDigestSize);
}

}  // namespace ch
