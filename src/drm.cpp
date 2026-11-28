#include "drm.h"

#include <array>
#include <atomic>
#include <cctype>
#include <ctime>
#include <vector>

namespace edge_tts_drm {

namespace {

constexpr std::int64_t kWinEpoch = 11644473600LL;
constexpr std::uint64_t kTicksPerSecond = 10000000ULL;

constexpr std::array<std::uint32_t, 8> kSha256Initial = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

constexpr std::array<std::uint32_t, 8> kSha256State = {
    0xFD9FBEA2u, 0x91243CE7u, 0xBE499094u, 0xCD2E1E0Au,
    0x6C006F49u, 0x04BD35ABu, 0xFEB0A7E4u, 0x03EFFBD2u,
};

constexpr std::array<std::uint32_t, 14> kSha256Schedule = {
    0xEB24BF9Eu, 0x1558691Fu, 0xA0C09EE2u, 0x0CFA32BAu,
    0xA622EE8Cu, 0xD1DD9491u, 0xC0DD25A0u, 0xF9E70AB6u,
    0xF0C31573u, 0x7058CE4Eu, 0x7188E606u, 0x00E5AB33u,
    0xD258B060u, 0x72766564u,
};

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

constexpr std::uint32_t kSha256Adjustment = 0xC9DC06A7u;
constexpr std::uint32_t kSha256Sigma0Adjustment = 0x059C0B80u;
constexpr std::uint32_t kSha256Sigma1Adjustment = 0x000B92C0u;

struct Cs64Coefficient {
    std::uint32_t coefficient;
    std::uint32_t offset;
};

constexpr std::array<Cs64Coefficient, 14> kCs64Coefficients = {{
    {0xE82E91B1u, 0xB32700D0u},
    {0x44DE24C6u, 0x88DFF51Du},
    {0x3BEB65BCu, 0x29514038u},
    {0xC89C5933u, 0xB328CBDFu},
    {0x8D1EC211u, 0xD405B4D5u},
    {0x314EF5D5u, 0x26E55F3Cu},
    {0xE92491B1u, 0x607801C6u},
    {0xF0A5B9D5u, 0x82B2C1FBu},
    {0x06064CF3u, 0xFA51A3DAu},
    {0xDA79FA60u, 0x207CC464u},
    {0xBED20560u, 0x3C3C74F3u},
    {0x89F9C9F8u, 0xD2D14B61u},
    {0x35FA38C6u, 0xBB04A2C1u},
    {0xCB49E3C6u, 0xC5626167u},
}};

constexpr std::uint32_t kCs64Factor = 0x82893933u;
constexpr std::uint32_t kCs64Adjustment0 = 0xF2F90B1Au;
constexpr std::uint32_t kCs64Adjustment1 = 0x0B1A0000u;

std::atomic<std::int64_t> g_clockSkewSecs{0};

constexpr std::uint32_t rotr32(std::uint32_t value, unsigned int bits) {
    return (value >> bits) | (value << (32u - bits));
}

constexpr std::uint32_t rol16(std::uint32_t value) {
    return (value << 16u) | (value >> 16u);
}

constexpr std::uint32_t sigma0(std::uint32_t value) {
    return rotr32(value, 7u) ^ rotr32(value, 18u) ^ (value >> 3u);
}

constexpr std::uint32_t sigma1(std::uint32_t value) {
    return rotr32(value, 17u) ^ rotr32(value, 19u) ^ (value >> 10u);
}

std::uint32_t readBe32(const std::uint8_t *ptr) {
    return (static_cast<std::uint32_t>(ptr[0]) << 24u)
        | (static_cast<std::uint32_t>(ptr[1]) << 16u)
        | (static_cast<std::uint32_t>(ptr[2]) << 8u)
        | static_cast<std::uint32_t>(ptr[3]);
}

std::uint32_t readLe32(const std::uint8_t *ptr) {
    return static_cast<std::uint32_t>(ptr[0])
        | (static_cast<std::uint32_t>(ptr[1]) << 8u)
        | (static_cast<std::uint32_t>(ptr[2]) << 16u)
        | (static_cast<std::uint32_t>(ptr[3]) << 24u);
}

void writeBe32(std::uint8_t *out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    out[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

void writeLe32(std::uint8_t *out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
}

void expandSha256Schedule(std::array<std::uint32_t, 64> &words, std::size_t firstWord) {
    for (std::size_t index = firstWord; index < 64; ++index) {
        words[index] = sigma1(words[index - 2])
            + words[index - 7]
            + sigma0(words[index - 15])
            + words[index - 16];
    }
}

std::array<std::uint32_t, 8> sha256Rounds(
    const std::array<std::uint32_t, 8> &state,
    const std::array<std::uint32_t, 64> &words,
    std::size_t firstRound
) {
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t index = firstRound; index < 64; ++index) {
        const std::uint32_t constant = (firstRound == 14 && index == 14)
            ? kSha256Adjustment
            : kSha256RoundConstants[index];
        const std::uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        const std::uint32_t ch = g ^ (e & (f ^ g));
        const std::uint32_t t1 = h + s1 + ch + constant + words[index];
        const std::uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        const std::uint32_t maj = (a & b) | (c & (a | b));
        const std::uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    return {a, b, c, d, e, f, g, h};
}

std::array<std::uint32_t, 8> sha256Init(const std::uint8_t *prefix) {
    std::array<std::uint32_t, 64> words{};
    words[14] = (static_cast<std::uint32_t>(prefix[0]) << 16u)
        | (static_cast<std::uint32_t>(prefix[1]) << 8u)
        | static_cast<std::uint32_t>(prefix[2]);
    words[15] = readBe32(prefix + 3);

    for (std::size_t index = 16; index < 30; ++index) {
        std::uint32_t value = kSha256Schedule[index - 16];
        if (index >= 16) {
            const std::uint32_t s1 = sigma1(words[index - 2]);
            value += (index == 16) ? (s1 ^ kSha256Sigma1Adjustment) : s1;
        }
        if (index >= 21) {
            value += words[index - 7];
        }
        if (index >= 29) {
            const std::uint32_t s0 = sigma0(words[index - 15]);
            value += (index == 29) ? (s0 ^ kSha256Sigma0Adjustment) : s0;
        }
        words[index] = value;
    }

    words[30] = sigma1(words[28])
        + words[23]
        + sigma0(words[15])
        + words[14]
        + kSha256Adjustment
        - kSha256RoundConstants[14];

    expandSha256Schedule(words, 31);
    const auto working = sha256Rounds(kSha256State, words, 14);

    std::array<std::uint32_t, 8> state{};
    for (std::size_t i = 0; i < 8; ++i) {
        state[i] = kSha256Initial[i] + working[i];
    }
    return state;
}

std::array<std::uint8_t, 32> sha256Digest(const std::uint8_t *data, std::size_t len) {
    const std::size_t totalLength = 57 + len;
    const std::size_t padZeros = static_cast<std::size_t>(
        (55 - static_cast<std::int64_t>(totalLength % 64) + 64) % 64
    );

    std::vector<std::uint8_t> padded;
    padded.reserve((len - 7) + 1 + padZeros + 8);
    padded.insert(padded.end(), data + 7, data + len);
    padded.push_back(0x80u);
    padded.resize(padded.size() + padZeros, 0u);

    const std::uint64_t bitLength = static_cast<std::uint64_t>(totalLength) * 8ULL;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xFFu));
    }

    auto state = sha256Init(data);
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = readBe32(padded.data() + offset + i * 4);
        }
        expandSha256Schedule(words, 16);
        const auto working = sha256Rounds(state, words, 0);
        for (std::size_t i = 0; i < 8; ++i) {
            state[i] += working[i];
        }
    }

    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        writeBe32(out.data() + i * 4, state[i]);
    }
    return out;
}

std::vector<std::uint32_t> wordsLe4Even(const std::uint8_t *data, std::size_t len) {
    std::vector<std::uint8_t> padded;
    padded.reserve(len + 4);
    padded.push_back(0u);
    padded.insert(padded.end(), data, data + len);
    while (padded.size() % 4 != 0) {
        padded.push_back(0u);
    }

    std::vector<std::uint32_t> words;
    words.reserve(padded.size() / 4 + 1);
    for (std::size_t i = 0; i < padded.size(); i += 4) {
        words.push_back(readLe32(padded.data() + i));
    }
    if (words.size() % 2 != 0) {
        words.push_back(0u);
    }
    return words;
}

std::pair<std::uint32_t, std::uint32_t> cs64Digest(
    const std::uint8_t *data,
    std::size_t len,
    const std::array<std::uint8_t, 32> &digest
) {
    const std::uint32_t keyA = readLe32(digest.data()) | 1u;
    const std::uint32_t keyB = readLe32(digest.data() + 4) | 1u;
    const std::array<std::uint32_t, 2> factors = {
        keyA * kCs64Factor,
        keyB * kCs64Factor,
    };

    const auto inputWords = wordsLe4Even(data, len);
    std::vector<std::uint32_t> terms;
    std::vector<std::uint32_t> offsets;
    terms.reserve(kCs64Coefficients.size() + inputWords.size());
    offsets.reserve(kCs64Coefficients.size() + inputWords.size());

    for (std::size_t index = 0; index < kCs64Coefficients.size(); ++index) {
        terms.push_back(kCs64Coefficients[index].coefficient * factors[index % 2]);
        offsets.push_back(kCs64Coefficients[index].offset);
    }

    for (std::size_t index = 0; index < inputWords.size(); ++index) {
        const std::uint32_t word = inputWords[index];
        std::uint32_t term = word * (index % 2 == 0 ? keyA : keyB);
        std::uint32_t offset = rol16(word) * (index % 2 == 0 ? 0xEF0569FBu : 0xC31713DBu);
        if (index == 0) {
            term += kCs64Adjustment0 * factors[0];
            offset += kCs64Adjustment1;
        }
        terms.push_back(term);
        offsets.push_back(offset);
    }

    std::uint32_t swapAcc = 0;
    std::uint32_t swapLast = 0;
    std::uint32_t state = 0;
    std::uint32_t acc = 0;

    for (std::size_t index = 0; index < terms.size(); index += 2) {
        const std::uint32_t x = terms[index];
        const std::uint32_t y = terms[index + 1];

        std::uint32_t t = rol16(offsets[index] + x);
        std::uint32_t q = t * 0x76F50F34u;
        swapAcc += rol16(q) * 0xC3EFEA97u + q;

        t = rol16(offsets[index + 1] + y);
        q = t * 0x1881CE3Cu;
        swapLast = rol16(q) * 0x35BD1EC9u + q;
        swapAcc += swapLast;

        state = state * keyA + x;
        state = rol16(state) * 0xCF98B111u;
        state = rol16(state) * 0x87085B9Fu;
        state = rol16(state) * 0x12CEB96Du;
        state = rol16(state) * 0x257E1D83u;
        acc += state;

        t = state * keyB + y;
        t = rol16(t) * 0xA27416F5u;
        t = rol16(t) * 0xD38396FFu;
        state = rol16(rol16(t) * 0x7C932B89u) * 0xBFA49F69u;
        acc += state;
    }

    return {swapLast ^ state, swapAcc ^ acc};
}

std::string encodeHexUpper(const std::uint8_t *bytes, std::size_t len) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(bytes[i] >> 4u) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return out;
}

std::int64_t currentUnixSecs() {
    return static_cast<std::int64_t>(std::time(nullptr))
        + g_clockSkewSecs.load(std::memory_order_relaxed);
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= (month <= 2u) ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned shiftedMonth = month > 2u ? (month - 3u) : (month + 9u);
    const unsigned doy = (153u * shiftedMonth + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<std::int64_t>(era) * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;
}

int parseMonthName(const std::string &mon) {
    static constexpr const char *kMonths[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; ++i) {
        if (mon == kMonths[i]) {
            return i + 1;
        }
    }
    return 0;
}

bool parseUnsigned(const std::string &s, std::size_t pos, std::size_t len, int &out) {
    if (pos + len > s.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(s[pos + i]);
        if (!std::isdigit(ch)) {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    out = value;
    return true;
}

} // namespace

std::string generateSecMsGecV2At(std::int64_t unixSecs, const std::string &trustedClientToken) {
    const std::int64_t shifted = unixSecs + kWinEpoch;
    const std::uint64_t winSecs = shifted > 0 ? static_cast<std::uint64_t>(shifted) : 0ULL;
    const std::uint64_t roundedSecs = winSecs - (winSecs % 300ULL);
    const std::uint64_t ticks = roundedSecs * kTicksPerSecond;

    const std::string strToHash = std::to_string(ticks) + trustedClientToken;
    const auto *raw = reinterpret_cast<const std::uint8_t *>(strToHash.data());

    const auto digest = sha256Digest(raw, strToHash.size());
    const auto [w0, w1] = cs64Digest(raw, strToHash.size(), digest);
    std::array<std::uint8_t, 8> cs64Bytes{};
    writeLe32(cs64Bytes.data(), w0);
    writeLe32(cs64Bytes.data() + 4, w1);

    return encodeHexUpper(cs64Bytes.data(), cs64Bytes.size())
        + "-"
        + encodeHexUpper(digest.data(), digest.size());
}

std::string generateSecMsGecV2(const std::string &trustedClientToken) {
    return generateSecMsGecV2At(currentUnixSecs(), trustedClientToken);
}

std::optional<std::int64_t> parseRfc2616Date(const std::string &date) {
    // Expected IMF-fixdate format: "Sat, 28 Nov 2026 06:27:37 GMT"
    std::size_t start = 0;
    while (start < date.size() && std::isspace(static_cast<unsigned char>(date[start]))) {
        ++start;
    }
    const std::size_t comma = date.find(',', start);
    if (comma == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = comma + 1;
    while (pos < date.size() && date[pos] == ' ') {
        ++pos;
    }

    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parseUnsigned(date, pos, 2, day)) {
        return std::nullopt;
    }
    pos += 2;
    if (pos >= date.size() || date[pos] != ' ') {
        return std::nullopt;
    }
    ++pos;

    if (pos + 3 > date.size()) {
        return std::nullopt;
    }
    const int month = parseMonthName(date.substr(pos, 3));
    if (month == 0) {
        return std::nullopt;
    }
    pos += 3;
    if (pos >= date.size() || date[pos] != ' ') {
        return std::nullopt;
    }
    ++pos;

    if (!parseUnsigned(date, pos, 4, year)) {
        return std::nullopt;
    }
    pos += 4;
    if (pos >= date.size() || date[pos] != ' ') {
        return std::nullopt;
    }
    ++pos;

    if (!parseUnsigned(date, pos, 2, hour) || pos + 2 >= date.size() || date[pos + 2] != ':') {
        return std::nullopt;
    }
    pos += 3;
    if (!parseUnsigned(date, pos, 2, minute) || pos + 2 >= date.size() || date[pos + 2] != ':') {
        return std::nullopt;
    }
    pos += 3;
    if (!parseUnsigned(date, pos, 2, second)) {
        return std::nullopt;
    }

    if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }

    const std::int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86400LL + hour * 3600LL + minute * 60LL + second;
}

bool adjustClockSkewFromDate(const std::string &serverDate) {
    const auto serverSecs = parseRfc2616Date(serverDate);
    if (!serverSecs.has_value()) {
        return false;
    }
    const std::int64_t clientSecs = currentUnixSecs();
    g_clockSkewSecs.fetch_add(*serverSecs - clientSecs, std::memory_order_relaxed);
    return true;
}

} // namespace edge_tts_drm
