#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// ============================================================================
// Clean-Room CRC32 (IEEE 802.3) & RFC 1951 / RFC 1950 Deflate Decompressor
// ============================================================================

inline uint32_t calc_crc32(const void* data, size_t len, uint32_t init_crc = 0) {
    static uint32_t table[256];
    static bool table_init = false;
    if (!table_init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_init = true;
    }
    uint32_t crc = init_crc ^ 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

struct HuffTable {
    uint16_t counts[16];
    uint16_t symbols[288];
    int16_t fast[1 << 9]; // 9-bit fast lookup: (len << 12) | sym, or -1
};

inline bool build_huff(HuffTable* h, const uint8_t* lengths, int num_syms) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < num_syms; ++i) {
        if (lengths[i] > 15) return false;
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0;
    uint16_t offsets[16];
    uint16_t code = 0;
    uint16_t first_code[16];
    offsets[0] = 0;
    first_code[0] = 0;
    for (int len = 1; len <= 15; ++len) {
        code = (uint16_t)((code + h->counts[len - 1]) << 1);
        first_code[len] = code;
        offsets[len] = offsets[len - 1] + h->counts[len - 1];
    }
    for (int i = 0; i < (1 << 9); ++i) h->fast[i] = -1;
    uint16_t cur_code[16];
    memcpy(cur_code, first_code, sizeof(cur_code));
    for (int i = 0; i < num_syms; ++i) {
        int len = lengths[i];
        if (len != 0) {
            uint16_t pos = offsets[len]++;
            if (pos >= 288) return false;
            h->symbols[pos] = (uint16_t)i;
            uint16_t c = cur_code[len]++;
            if (len <= 9) {
                // Reverse bits of c (length len)
                uint16_t rev = 0;
                for (int b = 0; b < len; ++b) {
                    if (c & (1 << (len - 1 - b))) rev |= (1 << b);
                }
                int16_t val = (int16_t)((len << 10) | i);
                int step = 1 << len;
                for (int fill = rev; fill < (1 << 9); fill += step) {
                    h->fast[fill] = val;
                }
            }
        }
    }
    return true;
}

struct BitStream {
    const uint8_t* src;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;

    void init(const uint8_t* s, size_t sz) {
        src = s; size = sz; pos = 0; bitbuf = 0; bitcnt = 0;
    }
    inline void refill() {
        while (bitcnt <= 24 && pos < size) {
            bitbuf |= (uint32_t)src[pos++] << bitcnt;
            bitcnt += 8;
        }
    }
    inline uint32_t read_bits(int n) {
        if (bitcnt < n) refill();
        uint32_t v = bitbuf & ((1u << n) - 1u);
        bitbuf >>= n;
        bitcnt -= n;
        return v;
    }
    inline int decode(const HuffTable* h) {
        if (bitcnt < 15) refill();
        int16_t fast_val = h->fast[bitbuf & 0x1FF];
        if (fast_val >= 0) {
            int len = fast_val >> 10;
            bitbuf >>= len;
            bitcnt -= len;
            return fast_val & 0x3FF;
        }
        int code = 0;
        int first = 0;
        int index = 0;
        uint32_t b = bitbuf;
        for (int len = 1; len <= 15; ++len) {
            code |= (b & 1);
            b >>= 1;
            int count = h->counts[len];
            if (code - count < first) {
                bitbuf >>= len;
                bitcnt -= len;
                return h->symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool inflate_raw(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out, size_t expected_uncompressed = 0) {
    out.clear();
    if (expected_uncompressed > 0) out.reserve(expected_uncompressed);

    static const uint16_t len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t len_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const uint16_t dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const uint8_t dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static const uint8_t clen_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    BitStream bs;
    bs.init(src, src_len);

    HuffTable lit_huff, dist_huff;
    int bfinal = 0;

    while (!bfinal) {
        bfinal = bs.read_bits(1);
        int btype = bs.read_bits(2);
        if (btype == 0) {
            // Stored block: align to byte boundary
            int drop = bs.bitcnt & 7;
            bs.bitbuf >>= drop;
            bs.bitcnt -= drop;
            size_t real_pos = bs.pos - (bs.bitcnt >> 3);
            bs.bitbuf = 0;
            bs.bitcnt = 0;
            if (real_pos + 4 > src_len) return false;
            uint16_t len = (uint16_t)(src[real_pos] | (src[real_pos + 1] << 8));
            uint16_t nlen = (uint16_t)(src[real_pos + 2] | (src[real_pos + 3] << 8));
            if ((len ^ 0xFFFF) != nlen) return false;
            real_pos += 4;
            if (real_pos + len > src_len) return false;
            size_t old_sz = out.size();
            out.resize(old_sz + len);
            if (len > 0) memcpy(out.data() + old_sz, src + real_pos, len);
            bs.pos = real_pos + len;
        } else if (btype == 1 || btype == 2) {
            if (btype == 1) {
                uint8_t lengths[288];
                for (int i = 0; i <= 143; ++i) lengths[i] = 8;
                for (int i = 144; i <= 255; ++i) lengths[i] = 9;
                for (int i = 256; i <= 279; ++i) lengths[i] = 7;
                for (int i = 280; i <= 287; ++i) lengths[i] = 8;
                build_huff(&lit_huff, lengths, 288);
                uint8_t dlengths[30];
                for (int i = 0; i < 30; ++i) dlengths[i] = 5;
                build_huff(&dist_huff, dlengths, 30);
            } else {
                int hlit = bs.read_bits(5) + 257;
                int hdist = bs.read_bits(5) + 1;
                int hclen = bs.read_bits(4) + 4;
                if (hlit > 286 || hdist > 30) return false;
                uint8_t clen[19] = {0};
                for (int i = 0; i < hclen; ++i)
                    clen[clen_order[i]] = (uint8_t)bs.read_bits(3);
                HuffTable code_huff;
                if (!build_huff(&code_huff, clen, 19)) return false;
                uint8_t lengths[288 + 32] = {0};
                int idx = 0;
                int total = hlit + hdist;
                while (idx < total) {
                    int sym = bs.decode(&code_huff);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lengths[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        int rep = bs.read_bits(2) + 3;
                        uint8_t prev = lengths[idx - 1];
                        while (rep-- > 0 && idx < total) lengths[idx++] = prev;
                    } else if (sym == 17) {
                        int rep = bs.read_bits(3) + 3;
                        while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                    } else if (sym == 18) {
                        int rep = bs.read_bits(7) + 11;
                        while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                    } else {
                        return false;
                    }
                }
                if (!build_huff(&lit_huff, lengths, hlit)) return false;
                if (!build_huff(&dist_huff, lengths + hlit, hdist)) return false;
            }

            while (true) {
                int sym = bs.decode(&lit_huff);
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int length = len_base[sym] + bs.read_bits(len_extra[sym]);
                    int dsym = bs.decode(&dist_huff);
                    if (dsym < 0 || dsym >= 30) return false;
                    size_t dist = dist_base[dsym] + bs.read_bits(dist_extra[dsym]);
                    if (dist > out.size() || dist == 0) return false;
                    size_t start = out.size() - dist;
                    size_t cur_sz = out.size();
                    out.resize(cur_sz + length);
                    uint8_t* dst = out.data() + cur_sz;
                    const uint8_t* s = out.data() + start;
                    if (dist >= (size_t)length) {
                        memcpy(dst, s, length);
                    } else {
                        for (int i = 0; i < length; ++i) dst[i] = s[i];
                    }
                }
            }
        } else {
            return false;
        }
    }
    return true;
}

inline bool inflate_zlib(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out, size_t expected_uncompressed = 0) {
    if (src_len < 6) return false;
    // Skip 2-byte zlib header (CMF + FLG) and 4-byte Adler32 trailer
    return inflate_raw(src + 2, src_len - 6, out, expected_uncompressed);
}
