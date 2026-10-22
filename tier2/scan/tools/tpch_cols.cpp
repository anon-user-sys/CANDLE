// tpch_cols: stream TPC-H lineitem text into the two columns the scan uses.
//
//   tpch_cols <out.keys.u64> <out.qty.u8> [in.tbl]      (reads stdin when no input given)
//
// Writes l_orderkey (column 0) as raw little-endian uint64 and l_quantity (column 4, integer
// part: "17.00" -> 17) as uint8, in input row order. Memory stays flat, so dbgen can write into
// a FIFO that feeds this tool and the text never reaches disk (pattern from the hash join's
// tools/tpch_keycol). Prints "rows max_key" to stdout on success; any malformed row or
// l_quantity outside [1,50] is an error.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::size_t kInBuf = 64U << 20;
constexpr std::size_t kFlush = 1U << 22;   // rows per output flush

bool flush(FILE* fk, FILE* fq, std::vector<std::uint64_t>& keys, std::vector<std::uint8_t>& qty) {
    const bool ok = std::fwrite(keys.data(), sizeof(std::uint64_t), keys.size(), fk) == keys.size() &&
                    std::fwrite(qty.data(), 1, qty.size(), fq) == qty.size();
    keys.clear(); qty.clear();
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: tpch_cols <out.keys.u64> <out.qty.u8> [in.tbl]\n");
        return 2;
    }
    FILE* in = argc == 4 ? std::fopen(argv[3], "rb") : stdin;
    FILE* fk = std::fopen(argv[1], "wb");
    FILE* fq = std::fopen(argv[2], "wb");
    if (!in || !fk || !fq) { std::perror("tpch_cols: open"); return 1; }

    std::vector<char> buf(kInBuf);
    std::vector<std::uint64_t> keys; keys.reserve(kFlush);
    std::vector<std::uint8_t> qty; qty.reserve(kFlush);
    std::uint64_t rows = 0, max_key = 0;
    std::size_t carry = 0;

    for (;;) {
        const std::size_t got = std::fread(buf.data() + carry, 1, kInBuf - carry, in);
        const std::size_t len = carry + got;
        if (len == 0) break;
        std::size_t pos = 0;
        for (;;) {
            const char* nl = static_cast<const char*>(std::memchr(buf.data() + pos, '\n', len - pos));
            if (!nl) break;
            const char* p = buf.data() + pos;
            const char* end = nl;
            pos = static_cast<std::size_t>(nl - buf.data()) + 1;
            if (p == end || (end - p == 1 && *p == '\r')) continue;   // blank line

            std::uint64_t k = 0;
            const char* k0 = p;
            while (p < end && *p >= '0' && *p <= '9') k = k * 10 + static_cast<std::uint64_t>(*p++ - '0');
            for (int field = 0; field < 4 && p < end; ++field) {      // skip to column 4
                while (p < end && *p != '|') ++p;
                if (p < end) ++p;
            }
            std::uint64_t q = 0;
            const char* q0 = p;
            while (p < end && *p >= '0' && *p <= '9') q = q * 10 + static_cast<std::uint64_t>(*p++ - '0');
            if (p == k0 || p == q0 || q < 1 || q > 50) {
                std::fprintf(stderr, "tpch_cols: malformed row %llu (key/quantity)\n", (unsigned long long)rows);
                return 1;
            }
            keys.push_back(k);
            qty.push_back(static_cast<std::uint8_t>(q));
            if (k > max_key) max_key = k;
            ++rows;
            if (keys.size() == kFlush && !flush(fk, fq, keys, qty)) { std::perror("tpch_cols: write"); return 1; }
        }
        carry = len - pos;
        if (got == 0) {
            if (carry) { std::fprintf(stderr, "tpch_cols: input ends without a newline\n"); return 1; }
            break;
        }
        std::memmove(buf.data(), buf.data() + pos, carry);
        if (carry == kInBuf) { std::fprintf(stderr, "tpch_cols: line longer than %zu bytes\n", kInBuf); return 1; }
    }
    if (!flush(fk, fq, keys, qty) || std::fclose(fk) != 0 || std::fclose(fq) != 0) { std::perror("tpch_cols: write"); return 1; }
    std::printf("%llu %llu\n", (unsigned long long)rows, (unsigned long long)max_key);
    return 0;
}
