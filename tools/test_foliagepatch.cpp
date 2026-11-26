// Offline check of src/foliagepatch.cpp: g++ -std=c++17 -I src tools/test_foliagepatch.cpp src/foliagepatch.cpp -o /tmp/tfp
// ./tfp in.dxbc [out.dxbc]...   prints the original's checksum check and what the patch did.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "foliagepatch.h"

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i += 2) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        s4vr::foliagepatch::Info info;
        std::vector<uint8_t> out;
        const bool sum = s4vr::foliagepatch::checksumMatches(in.data(), in.size());
        const bool ok = s4vr::foliagepatch::patch(in.data(), in.size(), out, &info);
        std::printf("%s: %zu bytes, original checksum %s, patch %s family %d%s%s", argv[i], in.size(), sum ? "ok" : "WRONG",
                    ok ? "ok" : "REFUSED", info.family, ok ? "" : ": ", ok ? "" : info.why.c_str());
        if (ok) std::printf(" -> %zu bytes, new checksum %s", out.size(), s4vr::foliagepatch::checksumMatches(out.data(), out.size()) ? "ok" : "WRONG");
        std::printf("\n");
        if (!sum) ++bad;
        if (ok && i + 1 < argc) std::ofstream(argv[i + 1], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), out.size());
    }
    return bad ? 1 : 0;
}
