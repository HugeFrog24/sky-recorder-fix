#include "locator.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <initializer_list>

static sky::Digest digest(std::initializer_list<uint32_t> words) {
    return sky::fingerprint(reinterpret_cast<const uint8_t *>(words.begin()), words.size() * 4);
}
static bool same(sky::Digest a, sky::Digest b) { return a.a == b.a && a.b == b.b; }
static void test_fingerprint() {
    assert(same(digest({0x90000013, 0x94000013, 0xf941e260, 0xd65f03c0}),
                digest({0x90000013, 0x94000014, 0xf943e260, 0xd65f03c0})));
    assert(!same(digest({0x90000009, 0xd63f0100, 0xf9400120, 0xd65f03c0}),
                 digest({0x90000009, 0xd63f0100, 0xf9400520, 0xd65f03c0})));
    assert(same(digest({0x90000013, 0x54000060, 0x52800013, 0xd65f03c0, 0xf9400260, 0xd65f03c0}),
                digest({0x90000013, 0x54000060, 0x52800013, 0xd65f03c0, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0x54000040, 0x52800013, 0xf9400260, 0xd65f03c0}),
                 digest({0x90000013, 0x54000040, 0x52800013, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0xa9404c14, 0xf9400260, 0xd65f03c0}),
                 digest({0x90000013, 0xa9404c14, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0xa8c10260, 0xf9400260, 0xd65f03c0}),
                 digest({0x90000013, 0xa8c10260, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0xf8008660, 0xf9400260, 0xd65f03c0}),
                 digest({0x90000013, 0xf8008660, 0xf9400660, 0xd65f03c0})));
    assert(same(digest({0x90000013, 0xf9000013, 0xf9400260, 0xd65f03c0}),
                digest({0x90000013, 0xf9000013, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0x14000003, 0xf9400260, 0xd65f03c0, 0xd65f03c0}),
                 digest({0x90000013, 0x14000003, 0xf9400660, 0xd65f03c0, 0xd65f03c0})));
    assert(!same(digest({0x90000013, 0xf9400260, 0x52800013, 0x17fffffe}),
                 digest({0x90000013, 0xf9400660, 0x52800013, 0x17fffffe})));
    assert(!same(digest({0x90000013, 0x00000000, 0xf9400260, 0xd65f03c0}),
                 digest({0x90000013, 0x00000000, 0xf9400660, 0xd65f03c0})));
    assert(!same(digest({0x9000001f, 0xf94003e0, 0xd65f03c0}), digest({0x9000001f, 0xf94007e0, 0xd65f03c0})));
    assert(!same(digest({0x91000020, 0xf9400000, 0xd65f03c0}), digest({0x91000420, 0xf9400000, 0xd65f03c0})));
    uint32_t maximum[1024]{};
    for (auto &w : maximum) {
        w = 0xd503201f;
    }
    assert(sky::fingerprint(reinterpret_cast<const uint8_t *>(maximum), sizeof(maximum)).a);
    assert(!sky::fingerprint(reinterpret_cast<const uint8_t *>(maximum), sizeof(maximum) + 4).a);
    assert(!sky::fingerprint(reinterpret_cast<const uint8_t *>(maximum), 3).a);
}

using sky::Address;
struct ElfHeader {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phsize, phcount, shsize, shcount, shindex;
};
struct Program {
    uint32_t type, flags;
    uint64_t offset, address, physical, filesize, memsize, align;
};
struct Loaded {
    sky::Image image;
    std::vector<std::vector<uint8_t>> bytes;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            return false;
        }
        ElfHeader h{};
        if (fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.ident, "\177ELF\2\1", 6) != 0 || h.machine != 183 ||
            h.phsize != sizeof(Program) || h.phcount > 128) {
            (void)fclose(f);
            return false;
        }
        std::vector<Program> ph(h.phcount);
        if (h.phoff > LONG_MAX || fseek(f, long(h.phoff), SEEK_SET) != 0) {
            (void)fclose(f);
            return false;
        }
        if (fread(ph.data(), sizeof(Program), ph.size(), f) != ph.size()) {
            (void)fclose(f);
            return false;
        }
        bytes.reserve(16);
        Address dynamic = 0;
        size_t dynamic_size = 0;
        for (const auto &p : ph) {
            if (p.type == 0x6474e550) {
                image.unwind = p.address;
            }
            if (p.type == 2) {
                dynamic = p.address;
                dynamic_size = p.filesize;
            }
            if (p.type != 1) {
                continue;
            }
            if (image.count == 16 || p.filesize > p.memsize || p.memsize > uint64_t(512) * 1024 * 1024) {
                (void)fclose(f);
                return false;
            }
            bytes.emplace_back(size_t(p.memsize));
            if (p.offset > LONG_MAX || fseek(f, long(p.offset), SEEK_SET) != 0) {
                (void)fclose(f);
                return false;
            }
            if (fread(bytes.back().data(), 1, size_t(p.filesize), f) != p.filesize) {
                (void)fclose(f);
                return false;
            }
            image.spans[image.count++] = {p.address, bytes.back().data(), size_t(p.memsize), p.flags};
        }
        if (fclose(f) != 0) {
            return false;
        }
        Address rela = 0;
        size_t rela_size = 0;
        for (size_t i = 0; i + 16 <= dynamic_size; i += 16) {
            auto tag = image.read<uint64_t>(dynamic + i);
            auto value = image.read<uint64_t>(dynamic + i + 8);
            if (tag == 7) {
                rela = value;
            }
            if (tag == 8) {
                rela_size = size_t(value);
            }
        }
        if (!rela || !image.view(rela, rela_size)) {
            return false;
        }
        for (size_t i = 0; i + 24 <= rela_size; i += 24) {
            Address const target = image.read<Address>(rela + i);
            Address const info = image.read<Address>(rela + i + 8);
            Address value = image.read<Address>(rela + i + 16);
            if (uint32_t(info) != 1027) {
                continue;
            }
            auto *p = const_cast<uint8_t *>(image.view(target, 8));
            if (!p) {
                return false;
            }
            memcpy(p, &value, 8);
        }
        return true;
    }
    template <class T> void write(Address address, T value) {
        auto *p = const_cast<uint8_t *>(image.view(address, sizeof(T)));
        assert(p);
        memcpy(p, &value, sizeof(value));
    }
};
static void rejected(const sky::Image &image) {
    sky::Profile p{};
    const char *status = nullptr;
    assert(!sky::locate(image, p, status));
    assert(!p.renderer);
}
static void mutations(Loaded &l, const sky::Profile &p) {
    for (unsigned long const pc : p.functions) {
        uint32_t const original = l.image.read<uint32_t>(pc);
        l.write(pc, original ^ 32);
        rejected(l.image);
        l.write(pc, original);
    }
    for (unsigned long const slot : p.slots) {
        Address const original = l.image.read<Address>(slot);
        l.write(slot, Address(0));
        rejected(l.image);
        l.write(slot, original);
    }
    uint32_t const required[] = {0xb94e9008, 0xf947526a, 0xf9474e69, 0x52801708, 0xb9406ee8,
                                 0xb9407ee8, 0x3942c2ea, 0xb94066ea, 0x3942cae8};
    for (uint32_t const instruction : required) {
        bool found = false;
        for (unsigned i = 0; i < 1272; i += 4) {
            Address const pc = p.functions[sky::Pixels] + i;
            if (l.image.read<uint32_t>(pc) != instruction) {
                continue;
            }
            l.write(pc, instruction ^ (1U << 10));
            rejected(l.image);
            l.write(pc, instruction);
            found = true;
        }
        assert(found);
    }
    auto saved = l.image.unwind;
    l.image.unwind = 0;
    rejected(l.image);
    l.image.unwind = saved;
    uint32_t const count = l.image.read<uint32_t>(saved + 8);
    l.write(saved + 8, UINT32_MAX);
    rejected(l.image);
    l.write(saved + 8, count);
    Address const duplicate = p.renderer;
    auto value = l.image.read<Address>(duplicate);
    l.write(duplicate, p.functions[sky::Pixels]);
    rejected(l.image);
    l.write(duplicate, value);
    Address const pc = p.functions[sky::Begin] + 52;
    auto instruction = l.image.read<uint32_t>(pc);
    l.write(pc, instruction ^ (1U << 10));
    sky::Profile changed{};
    const char *status = nullptr;
    bool const accepted = sky::locate(l.image, changed, status);
    assert(!accepted);
    l.write(pc, instruction);
    {
        auto im = l.image;
        std::vector<uint8_t> code(1276);
        memcpy(code.data(), im.view(p.functions[sky::Pixels], 1272), 1272);
        uint32_t nop = 0xd503201f;
        memcpy(code.data() + 1272, &nop, 4);
        std::vector<uint8_t> table(12 + (size_t(count + 2) * 8));
        memcpy(table.data(), im.view(saved, 12 + (size_t(count) * 8)), 12 + (size_t(count) * 8));
        uint32_t larger = count + 2;
        memcpy(table.data() + 8, &larger, 4);
        int32_t offsets[] = {int32_t(0x10000000 - saved), 0, int32_t(0x10000000 + 1272 - saved), 0};
        memcpy(table.data() + 12 + (size_t(count) * 8), offsets, sizeof(offsets));
        assert(im.count < 15);
        for (size_t i = im.count; i > 0; --i) {
            im.spans[i] = im.spans[i - 1];
        }
        im.spans[0] = {saved, table.data(), table.size(), 4};
        ++im.count;
        im.spans[im.count++] = {0x10000000, code.data(), code.size(), 5};
        rejected(im);
    }
    {
        const Address bias = 0x7100000000ULL;
        std::vector<std::pair<Address, Address>> changes;
        for (size_t i = 0; i < l.image.count; ++i) {
            auto &s = l.image.spans[i];
            if (s.flags & 1) {
                continue;
            }
            for (Address a = (s.address + 7) & ~Address(7); a - s.address + 8 <= s.size; a += 8) {
                Address const value = l.image.read<Address>(a);
                if (value && l.image.view(value, 1)) {
                    changes.emplace_back(a, value);
                }
            }
        }
        for (auto change : changes) {
            l.write(change.first, change.second + bias);
        }
        l.image.bias = bias;
        sky::Profile rebased{};
        assert(sky::locate(l.image, rebased, status));
        assert(!memcmp(&p, &rebased, sizeof(p)));
        l.image.bias = 0;
        for (auto change : changes) {
            l.write(change.first, change.second);
        }
    }
    puts("mutation checks passed");
}
int main(int argc, char **argv) try {
    if (setvbuf(stdout, nullptr, _IONBF, 0) != 0) {
        return 2;
    }
    test_fingerprint();
    assert(argc >= 2);
    Loaded loaded;
    if (!loaded.load(argv[1])) {
        return 2;
    }
    if (argc > 2 && !strcmp(argv[2], "--fingerprints")) {
        uint32_t const count = loaded.image.read<uint32_t>(loaded.image.unwind + 8);
        for (int arg = 3; arg < argc; ++arg) {
            Address const address = strtoull(argv[arg], nullptr, 0);
            bool found = false;
            for (uint32_t i = 0; i + 1 < count; ++i) {
                Address const start =
                    loaded.image.unwind + int64_t(loaded.image.read<int32_t>(loaded.image.unwind + 12 + (8ULL * i)));
                if (start != address) {
                    continue;
                }
                Address const end =
                    loaded.image.unwind + int64_t(loaded.image.read<int32_t>(loaded.image.unwind + 20 + (8ULL * i)));
                const auto *bytes = loaded.image.view(start, size_t(end - start), 5);
                assert(bytes && end > start);
                auto h = sky::fingerprint(bytes, size_t(end - start));
                printf("%llu %llu %llu\n", static_cast<unsigned long long>(end - start),
                       static_cast<unsigned long long>(h.a), static_cast<unsigned long long>(h.b));
                found = true;
                break;
            }
            assert(found);
        }
        return 0;
    }
    sky::Profile p{};
    const char *status = nullptr;
    auto start = std::chrono::steady_clock::now();
    bool const ok = sky::locate(loaded.image, p, status);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    printf(R"({"accepted":%s,"status":"%s","microseconds":%lld,"functions":[)", ok ? "true" : "false", status,
           static_cast<long long>(us));
    for (unsigned i = 0; i < sky::RoleCount; ++i) {
        printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(p.functions[i]));
    }
    printf("],\"slots\":[");
    for (unsigned i = 0; i < 5; ++i) {
        printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(p.slots[i]));
    }
    printf("],\"renderer\":%llu,\"submit\":%llu,\"wait\":%llu}\n", static_cast<unsigned long long>(p.renderer),
           static_cast<unsigned long long>(p.submit), static_cast<unsigned long long>(p.wait));
    if (ok && argc > 2) {
        mutations(loaded, p);
    }
    return 0;
} catch (const std::exception &error) {
    (void)fprintf(stderr, "%s\n", error.what());
    return 1;
}
