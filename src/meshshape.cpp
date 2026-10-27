#include "meshshape.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "modelparse.h"

namespace s4vr::meshshape {

namespace {

constexpr float kSame = 0.015f;     // metres: layers whose outlines differ less are joined
constexpr float kWeld = 0.001f;     // metres: corners this close are one (pieces touching there are one piece)
constexpr float kTiny = 0.03f;      // metres: smaller pieces (keys, knobs) are left out
constexpr size_t kMaxCorners = 12;  // per outline (the smallest corners are cut off)
constexpr size_t kMaxPrisms = 64;   // per model; more: thicker layers, pieces joined

struct P2 {
    float x, z;
};

float cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); }

// Convex hull, anticlockwise (cross > 0 inside).
std::vector<P2> hull(std::vector<P2> p) {
    std::sort(p.begin(), p.end(), [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
    p.erase(std::unique(p.begin(), p.end(), [](const P2& a, const P2& b) {
                return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f;
            }),
            p.end());
    if (p.size() < 3) return p;
    std::vector<P2> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// At most kMaxCorners: the corner adding the least area goes first.
void simplify(std::vector<P2>& h) {
    while (h.size() > kMaxCorners) {
        size_t best = 0;
        float least = 1e30f;
        for (size_t i = 0; i < h.size(); ++i) {
            float a = std::fabs(cross(h[(i + h.size() - 1) % h.size()], h[i], h[(i + 1) % h.size()]));
            if (a < least) least = a, best = i;
        }
        h.erase(h.begin() + static_cast<long>(best));
    }
}

// A flat piece seen edge-on (two points, or a line of them): a thin box round it.
std::vector<P2> outline(const std::vector<P2>& pts) {
    std::vector<P2> h = hull(pts);
    if (h.size() >= 3) {
        simplify(h);
        return h;
    }
    if (h.size() < 2) return {};
    float dx = h[1].x - h[0].x, dz = h[1].z - h[0].z, len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) return {};
    float nx = -dz / len * 0.005f, nz = dx / len * 0.005f;
    return {{h[0].x - nx, h[0].z - nz}, {h[1].x - nx, h[1].z - nz}, {h[1].x + nx, h[1].z + nz}, {h[0].x + nx, h[0].z + nz}};
}

// How far q is outside the convex outline h (<= 0 inside).
float outside(const std::vector<P2>& h, const P2& q) {
    float worst = -1e30f;
    for (size_t i = 0; i < h.size(); ++i) {
        const P2 &a = h[i], &b = h[(i + 1) % h.size()];
        float len = std::hypot(b.x - a.x, b.z - a.z);
        if (len < 1e-6f) continue;
        worst = std::max(worst, -cross(a, b, q) / len);
    }
    return worst;
}

bool alike(const std::vector<P2>& a, const std::vector<P2>& b) {
    for (const P2& q : a)
        if (outside(b, q) > kSame) return false;
    for (const P2& q : b)
        if (outside(a, q) > kSame) return false;
    return true;
}

struct Prism {
    std::vector<P2> h;
    float y0, y1;
};

struct Tri {
    float v[3][3];
};

// One piece's layers, alike neighbours joined.
void slice(const std::vector<Tri>& tris, const std::vector<int>& mine, float band, std::vector<Prism>& out) {
    float lo = 1e30f, hi = -1e30f;
    for (int t : mine)
        for (auto& v : tris[t].v) lo = std::min(lo, v[1]), hi = std::max(hi, v[1]);
    const int n = std::clamp(static_cast<int>(std::ceil((hi - lo) / band)), 1, 400);
    std::vector<std::vector<P2>> pts(n);
    std::vector<float> ylo(n, 1e30f), yhi(n, -1e30f);
    for (int t : mine) {
        const Tri& tr = tris[t];
        float t0 = std::min({tr.v[0][1], tr.v[1][1], tr.v[2][1]}), t1 = std::max({tr.v[0][1], tr.v[1][1], tr.v[2][1]});
        int i0 = std::clamp(static_cast<int>((t0 - lo) / band), 0, n - 1);
        int i1 = std::clamp(static_cast<int>((t1 - lo) / band), 0, n - 1);
        for (int i = i0; i <= i1; ++i) {
            const float b0 = lo + i * band, b1 = i == n - 1 ? hi + 1e-4f : lo + (i + 1) * band;
            // The triangle cut to b0 <= y <= b1 (one plane, then the other).
            std::vector<std::array<float, 3>> poly = {{tr.v[0][0], tr.v[0][1], tr.v[0][2]},
                                                      {tr.v[1][0], tr.v[1][1], tr.v[1][2]},
                                                      {tr.v[2][0], tr.v[2][1], tr.v[2][2]}};
            for (int side = 0; side < 2 && !poly.empty(); ++side) {
                auto in = [&](const std::array<float, 3>& p) { return side == 0 ? p[1] >= b0 : p[1] <= b1; };
                const float cut = side == 0 ? b0 : b1;
                std::vector<std::array<float, 3>> next;
                for (size_t k = 0; k < poly.size(); ++k) {
                    const auto &c = poly[k], &d = poly[(k + 1) % poly.size()];
                    if (in(c)) next.push_back(c);
                    if (in(c) != in(d)) {
                        float f = (cut - c[1]) / (d[1] - c[1]);
                        next.push_back({c[0] + (d[0] - c[0]) * f, cut, c[2] + (d[2] - c[2]) * f});
                    }
                }
                poly.swap(next);
            }
            for (auto& p : poly) {
                pts[i].push_back({p[0], p[2]});
                ylo[i] = std::min(ylo[i], p[1]), yhi[i] = std::max(yhi[i], p[1]);
            }
        }
    }
    Prism cur;
    std::vector<P2> curPts;
    bool open = false;
    for (int i = 0; i < n; ++i) {
        if (pts[i].empty()) {
            if (open) out.push_back(cur), open = false;
            continue;
        }
        std::vector<P2> h = outline(pts[i]);
        if (h.empty()) continue;
        if (open && alike(cur.h, h)) {
            curPts.insert(curPts.end(), pts[i].begin(), pts[i].end());
            cur.h = outline(curPts);
            cur.y1 = std::max(cur.y1, yhi[i]);
            continue;
        }
        if (open) out.push_back(cur);
        cur = {h, ylo[i], yhi[i]};
        curPts = pts[i];
        open = true;
    }
    if (open) out.push_back(cur);
}

int findRoot(std::vector<int>& up, int a) {
    while (up[a] != a) a = up[a] = up[up[a]];
    return a;
}

}  // namespace

bool fromModel(const std::vector<uint8_t>& res, std::vector<float>& out, std::string& note) {
    const auto started = std::chrono::steady_clock::now();
    out.clear();
    cbo::parse::Geometry g;
    std::string why;
    if (!cbo::parse::readGeometry(res, -1, 0, g, why)) {
        note = why;
        return false;
    }
    std::vector<Tri> tris;
    for (const auto& m : g.meshes)
        for (uint32_t k = 0; k + 2 < m.indexCount; k += 3) {
            Tri t;
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                uint32_t v = g.indices[m.firstIndex + k + c];
                if (v >= g.vertices.size()) {
                    ok = false;
                    break;
                }
                for (int a = 0; a < 3; ++a) t.v[c][a] = g.vertices[v].pos[a];
            }
            if (ok) tris.push_back(t);
        }
    if (tris.empty()) {
        note = "no triangles";
        return false;
    }
    // Pieces: triangles sharing a corner (welded by position) are one.
    std::unordered_map<uint64_t, int> corner;
    std::vector<int> up(tris.size());
    for (size_t t = 0; t < tris.size(); ++t) up[t] = static_cast<int>(t);
    for (size_t t = 0; t < tris.size(); ++t)
        for (auto& v : tris[t].v) {
            uint64_t key = 0;
            for (int a = 0; a < 3; ++a)
                key = key << 21 | (static_cast<uint64_t>(static_cast<int64_t>(std::lround(v[a] / kWeld)) + (1 << 20)) & 0x1FFFFF);
            auto found = corner.emplace(key, static_cast<int>(t));
            if (!found.second) {
                int a = findRoot(up, static_cast<int>(t)), b = findRoot(up, found.first->second);
                if (a != b) up[a] = b;
            }
        }
    std::unordered_map<int, std::vector<int>> pieces;
    for (size_t t = 0; t < tris.size(); ++t) pieces[findRoot(up, static_cast<int>(t))].push_back(static_cast<int>(t));
    std::vector<std::vector<int>> kept;
    for (auto& [root, list] : pieces) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int t : list)
            for (auto& v : tris[t].v)
                for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v[a]), hi[a] = std::max(hi[a], v[a]);
        if (hi[0] - lo[0] < kTiny && hi[1] - lo[1] < kTiny && hi[2] - lo[2] < kTiny) continue;
        kept.push_back(std::move(list));
    }
    // Too many prisms: thicker layers, then the pieces as one (a TV's buttons and frame), then thicker again.
    std::vector<Prism> prisms;
    float band = 0.05f;
    bool joined = false;
    for (int tries = 0; tries < 7; ++tries) {
        prisms.clear();
        if (joined) {
            std::vector<int> all;
            for (auto& list : kept) all.insert(all.end(), list.begin(), list.end());
            slice(tris, all, band, prisms);
        } else {
            for (auto& list : kept) slice(tris, list, band, prisms);
        }
        if (prisms.size() <= kMaxPrisms) break;
        if (!joined && band < 0.09f)
            band = 0.1f;
        else if (!joined)
            joined = true, band = 0.05f;
        else
            band *= 2;
    }
    if (prisms.size() > kMaxPrisms) prisms.resize(kMaxPrisms);
    for (const Prism& p : prisms) {
        out.push_back(static_cast<float>(p.h.size()));
        out.push_back(p.y0);
        out.push_back(p.y1);
        for (const P2& q : p.h) out.push_back(q.x), out.push_back(q.z);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    char text[160];
    std::snprintf(text, sizeof(text), "%zu triangles, %zu pieces, %zu prisms (%s, %.0f cm layers), %.1f ms", tris.size(),
                  kept.size(), prisms.size(), joined ? "pieces joined" : "each piece", band * 100, ms);
    note = text;
    return !prisms.empty();
}

}  // namespace s4vr::meshshape
