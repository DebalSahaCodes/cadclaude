// pmodel - lightweight parametric 3D modeller
// Dear ImGui + GLFW + fixed-function OpenGL (no extension loader needed).
//
// Concepts
//   * Parameters : named values / expressions ("width = 40", "r = width/4")
//   * Objects    : primitives whose every dimension, position, rotation and
//                  array setting is an *expression* that can reference parameters
//   * Lights     : ambient + up to 4 directional/point lights, Phong shading
//   * Export     : Wavefront OBJ; projects save to a small text file
//
// Expression language: + - * / ^ ( ) numbers, parameter names, pi,
//   sin cos tan asin acos atan (degrees), atan2(y,x) sqrt abs floor ceil round
//   min(a,b) max(a,b) pow(a,b)

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static const double PI = 3.14159265358979323846;

// ----------------------------------------------------------------------------
// Expression evaluation
// ----------------------------------------------------------------------------
struct Expr {
    char buf[96];
    double val = 0;
    bool ok = true;
    Expr(const char* s = "") { snprintf(buf, sizeof buf, "%s", s); }
    void set(const char* s) { snprintf(buf, sizeof buf, "%s", s); }
};
using Vars = std::map<std::string, double>;

struct Parser {
    const char* s;
    const Vars& vars;
    bool err = false;

    void ws() { while (*s == ' ' || *s == '\t') ++s; }

    double expr() {
        double v = term();
        for (;;) {
            ws();
            if (*s == '+') { ++s; v += term(); }
            else if (*s == '-') { ++s; v -= term(); }
            else break;
        }
        return v;
    }
    double term() {
        double v = unary();
        for (;;) {
            ws();
            if (*s == '*') { ++s; v *= unary(); }
            else if (*s == '/') { ++s; double d = unary(); if (d == 0) err = true; else v /= d; }
            else break;
        }
        return v;
    }
    double unary() {
        ws();
        if (*s == '-') { ++s; return -unary(); }
        if (*s == '+') { ++s; return unary(); }
        return power();
    }
    double power() {
        double b = primary();
        ws();
        if (*s == '^') { ++s; double e = unary(); return std::pow(b, e); }
        return b;
    }
    double fn(const std::string& f, double a, double b, bool two) {
        const double D = PI / 180.0;
        if (!two) {
            if (f == "sin") return std::sin(a * D);
            if (f == "cos") return std::cos(a * D);
            if (f == "tan") return std::tan(a * D);
            if (f == "asin") return std::asin(a) / D;
            if (f == "acos") return std::acos(a) / D;
            if (f == "atan") return std::atan(a) / D;
            if (f == "sqrt") return std::sqrt(a);
            if (f == "abs") return std::fabs(a);
            if (f == "floor") return std::floor(a);
            if (f == "ceil") return std::ceil(a);
            if (f == "round") return std::round(a);
        } else {
            if (f == "min") return std::min(a, b);
            if (f == "max") return std::max(a, b);
            if (f == "pow") return std::pow(a, b);
            if (f == "atan2") return std::atan2(a, b) / D;
        }
        err = true;
        return 0;
    }
    double primary() {
        ws();
        if (*s == '(') {
            ++s;
            double v = expr();
            ws();
            if (*s == ')') ++s; else err = true;
            return v;
        }
        if (std::isdigit((unsigned char)*s) || *s == '.') {
            char* end;
            double v = std::strtod(s, &end);
            s = end;
            return v;
        }
        if (std::isalpha((unsigned char)*s) || *s == '_') {
            std::string id;
            while (std::isalnum((unsigned char)*s) || *s == '_') id += *s++;
            ws();
            if (*s == '(') {
                ++s;
                double a = expr(), b = 0;
                bool two = false;
                ws();
                if (*s == ',') { ++s; b = expr(); two = true; }
                ws();
                if (*s == ')') ++s; else err = true;
                return fn(id, a, b, two);
            }
            if (id == "pi") return PI;
            auto it = vars.find(id);
            if (it == vars.end()) { err = true; return 0; }
            return it->second;
        }
        err = true;
        return 0;
    }
};

static bool evalExpr(Expr& e, const Vars& v) {
    Parser p{e.buf, v};
    double r = p.expr();
    p.ws();
    if (p.err || *p.s || !std::isfinite(r)) { e.ok = false; return false; }
    e.val = r;
    e.ok = true;
    return true;
}

// ----------------------------------------------------------------------------
// Mesh generation
// ----------------------------------------------------------------------------
struct Mesh {
    std::vector<float> v, n;
    std::vector<unsigned> i;
    unsigned add(double x, double y, double z, double nx, double ny, double nz) {
        v.push_back((float)x); v.push_back((float)y); v.push_back((float)z);
        n.push_back((float)nx); n.push_back((float)ny); n.push_back((float)nz);
        return (unsigned)(v.size() / 3 - 1);
    }
    void quad(unsigned a, unsigned b, unsigned c, unsigned d) {
        i.insert(i.end(), {a, b, c, a, c, d});
    }
    void tri(unsigned a, unsigned b, unsigned c) { i.insert(i.end(), {a, b, c}); }
};

static Mesh makeBox(double X, double Y, double Z, int segs) {
    Mesh m;
    int N = std::clamp(segs / 4, 1, 32);  // subdivide faces so per-vertex lighting looks good
    double h[3] = {X / 2, Y / 2, Z / 2};
    for (int k = 0; k < 3; k++)
        for (int s = -1; s <= 1; s += 2) {
            int u = (k + 1) % 3, w = (k + 2) % 3;
            double nn[3] = {0, 0, 0};
            nn[k] = s;
            unsigned base = (unsigned)(m.v.size() / 3);
            for (int a = 0; a <= N; a++)
                for (int b = 0; b <= N; b++) {
                    double p[3];
                    p[k] = s * h[k];
                    p[u] = (-1.0 + 2.0 * a / N) * h[u];
                    p[w] = (-1.0 + 2.0 * b / N) * h[w];
                    m.add(p[0], p[1], p[2], nn[0], nn[1], nn[2]);
                }
            auto id = [&](int a, int b) { return base + (unsigned)(a * (N + 1) + b); };
            for (int a = 0; a < N; a++)
                for (int b = 0; b < N; b++) {
                    if (s > 0) m.quad(id(a, b), id(a + 1, b), id(a + 1, b + 1), id(a, b + 1));
                    else       m.quad(id(a, b + 1), id(a + 1, b + 1), id(a + 1, b), id(a, b));
                }
        }
    return m;
}

static Mesh makeCyl(double r1, double r2, double h, int seg) {
    Mesh m;
    seg = std::max(3, seg);
    double hz = h / 2, sl = r1 - r2;
    for (int i = 0; i <= seg; i++) {
        double a = 2 * PI * i / seg, c = std::cos(a), s = std::sin(a);
        double nx = c * h, ny = s * h, nz = sl, L = std::sqrt(nx * nx + ny * ny + nz * nz);
        m.add(r1 * c, r1 * s, -hz, nx / L, ny / L, nz / L);
        m.add(r2 * c, r2 * s, hz, nx / L, ny / L, nz / L);
    }
    for (int i = 0; i < seg; i++) {
        unsigned b = 2 * i;
        m.quad(b, b + 2, b + 3, b + 1);
    }
    for (int cap = 0; cap < 2; cap++) {
        double r = cap ? r2 : r1, z = cap ? hz : -hz, nz = cap ? 1 : -1;
        if (r < 1e-9) continue;
        unsigned c0 = m.add(0, 0, z, 0, 0, nz), r0 = (unsigned)(m.v.size() / 3);
        for (int i = 0; i <= seg; i++) {
            double a = 2 * PI * i / seg;
            m.add(r * std::cos(a), r * std::sin(a), z, 0, 0, nz);
        }
        for (int i = 0; i < seg; i++) {
            if (cap) m.tri(c0, r0 + i, r0 + i + 1);
            else     m.tri(c0, r0 + i + 1, r0 + i);
        }
    }
    return m;
}

static Mesh makeSphere(double r, int seg) {
    Mesh m;
    seg = std::max(4, seg);
    int st = std::max(3, seg / 2), sl = seg;
    for (int i = 0; i <= st; i++) {
        double phi = PI * i / st, z = r * std::cos(phi), rr = r * std::sin(phi);
        for (int j = 0; j <= sl; j++) {
            double th = 2 * PI * j / sl, x = rr * std::cos(th), y = rr * std::sin(th);
            m.add(x, y, z, x / r, y / r, z / r);
        }
    }
    for (int i = 0; i < st; i++)
        for (int j = 0; j < sl; j++) {
            unsigned a = i * (sl + 1) + j, b = a + sl + 1;
            m.quad(a, b, b + 1, a + 1);
        }
    return m;
}

static Mesh makeTorus(double R, double r, int seg) {
    Mesh m;
    int maj = std::max(6, seg), mnr = std::max(6, seg / 2);
    for (int i = 0; i <= maj; i++) {
        double u = 2 * PI * i / maj, cu = std::cos(u), su = std::sin(u);
        for (int j = 0; j <= mnr; j++) {
            double v = 2 * PI * j / mnr, cv = std::cos(v), sv = std::sin(v);
            m.add((R + r * cv) * cu, (R + r * cv) * su, r * sv, cv * cu, cv * su, sv);
        }
    }
    for (int i = 0; i < maj; i++)
        for (int j = 0; j < mnr; j++) {
            unsigned a = i * (mnr + 1) + j, b = a + mnr + 1;
            m.quad(a, b, b + 1, a + 1);
        }
    return m;
}

// ----------------------------------------------------------------------------
// Scene model
// ----------------------------------------------------------------------------
enum ObjType { BOX, CYL, SPH, TOR, NTYPES };
static const char* kTypeNames[NTYPES] = {"Box", "Cylinder / Cone", "Sphere", "Torus"};
static const char* kPLabels[NTYPES][4] = {
    {"Width (X)", "Depth (Y)", "Height (Z)", ""},
    {"Bottom radius", "Top radius", "Height", ""},
    {"Radius", "", "", ""},
    {"Major radius", "Minor radius", "", ""}};
static const int kPCount[NTYPES] = {3, 3, 1, 2};
static const char* kPDefault[NTYPES][4] = {
    {"20", "20", "20", "1"}, {"10", "10", "30", "1"}, {"12", "1", "1", "1"}, {"20", "4", "1", "1"}};

// expression slots
enum { E_P = 0, E_POS = 4, E_ROT = 7, E_AOFF = 10, E_CNT = 13, NE = 14 };

struct Param {
    char name[32];
    Expr e;
    Param(const char* n = "p", const char* x = "1") { snprintf(name, sizeof name, "%s", n); e.set(x); }
};

struct Object {
    char name[64] = "Object";
    int type = BOX;
    int segs = 32;
    bool visible = true;
    float color[3] = {0.7f, 0.7f, 0.8f};
    Expr e[NE];
    Mesh mesh;
    double lastP[4] = {0, 0, 0, 0};
    int lastType = -1, lastSegs = -1;

    Object() {
        for (int i = 0; i < 3; i++) { e[E_POS + i].set("0"); e[E_ROT + i].set("0"); e[E_AOFF + i].set("0"); }
        e[E_CNT].set("1");
        setType(BOX);
    }
    void setType(int t) {
        type = t;
        for (int i = 0; i < 4; i++) e[i].set(kPDefault[t][i]);
    }
    int count() const { return std::clamp((int)std::lround(e[E_CNT].val), 1, 500); }
};

struct Light {
    bool on = false;
    int type = 0;  // 0 directional (position = direction *from*), 1 point
    float pos[3] = {0, 0, 1};
    float col[3] = {1, 1, 1};
    float inten = 1;
    float falloff = 0.0002f;
};

static std::vector<Param> g_params;
static std::vector<Object> g_objs;
static int g_sel = 0;
static Light g_lights[4];
static float g_ambI = 0.25f, g_ambCol[3] = {1, 1, 1};
static float g_spec = 0.5f, g_shin = 40.f;
static float g_bg[3] = {0.12f, 0.13f, 0.16f};
static bool g_grid = true, g_gizmos = true, g_wire = false;
static float g_gridSize = 200;
static char g_status[160] = "";

static void updateObject(Object& o, const Vars& v) {
    for (int i = 0; i < NE; i++) evalExpr(o.e[i], v);
    double p[4];
    for (int i = 0; i < 4; i++) p[i] = std::max(o.e[i].val, 0.0);
    bool same = o.lastType == o.type && o.lastSegs == o.segs;
    for (int i = 0; i < 4; i++) same = same && o.lastP[i] == p[i];
    if (same) return;
    o.lastType = o.type; o.lastSegs = o.segs;
    for (int i = 0; i < 4; i++) o.lastP[i] = p[i];
    const double eps = 1e-4;
    switch (o.type) {
        case BOX: o.mesh = makeBox(std::max(p[0], eps), std::max(p[1], eps), std::max(p[2], eps), o.segs); break;
        case CYL: {
            double r1 = p[0], r2 = p[1];
            if (r1 + r2 < 1e-6) r1 = eps;
            o.mesh = makeCyl(r1, r2, std::max(p[2], eps), o.segs);
            break;
        }
        case SPH: o.mesh = makeSphere(std::max(p[0], eps), o.segs); break;
        case TOR: o.mesh = makeTorus(std::max(p[0], eps), std::max(p[1], eps), o.segs); break;
    }
}

static void updateScene() {
    Vars vars;
    for (auto& p : g_params) {
        evalExpr(p.e, vars);  // on error keeps previous value
        vars[p.name] = p.e.val;
    }
    for (auto& o : g_objs) updateObject(o, vars);
}

static void instanceXform(const Object& o, int k, double pos[3], double rot[3]) {
    for (int i = 0; i < 3; i++) {
        pos[i] = o.e[E_POS + i].val + k * o.e[E_AOFF + i].val;
        rot[i] = o.e[E_ROT + i].val;
    }
}

static void rotv(const double r[3], const double in[3], double out[3]) {
    double rx = r[0] * PI / 180, ry = r[1] * PI / 180, rz = r[2] * PI / 180;
    double cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
    double x = in[0], y = in[1], z = in[2];
    double y1 = y * cx - z * sx, z1 = y * sx + z * cx, x1 = x;
    double x2 = x1 * cy + z1 * sy, z2 = -x1 * sy + z1 * cy, y2 = y1;
    out[0] = x2 * cz - y2 * sz;
    out[1] = x2 * sz + y2 * cz;
    out[2] = z2;
}

// ----------------------------------------------------------------------------
// File I/O: OBJ export and project save/load
// ----------------------------------------------------------------------------
static bool exportOBJ(const char* path, bool yUp) {
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "# pmodel export\n");
    unsigned base = 1;
    auto conv = [&](double* p) {
        if (yUp) { double y = p[1]; p[1] = p[2]; p[2] = -y; }
    };
    for (auto& o : g_objs) {
        if (!o.visible) continue;
        for (int k = 0; k < o.count(); k++) {
            double pos[3], rot[3];
            instanceXform(o, k, pos, rot);
            std::fprintf(f, "o %s_%d\n", o.name, k + 1);
            size_t nv = o.mesh.v.size() / 3;
            for (size_t i = 0; i < nv; i++) {
                double in[3] = {o.mesh.v[3 * i], o.mesh.v[3 * i + 1], o.mesh.v[3 * i + 2]}, out[3];
                rotv(rot, in, out);
                for (int c = 0; c < 3; c++) out[c] += pos[c];
                conv(out);
                std::fprintf(f, "v %.6f %.6f %.6f\n", out[0], out[1], out[2]);
            }
            for (size_t i = 0; i < nv; i++) {
                double in[3] = {o.mesh.n[3 * i], o.mesh.n[3 * i + 1], o.mesh.n[3 * i + 2]}, out[3];
                rotv(rot, in, out);
                conv(out);
                std::fprintf(f, "vn %.5f %.5f %.5f\n", out[0], out[1], out[2]);
            }
            for (size_t i = 0; i + 2 < o.mesh.i.size(); i += 3) {
                unsigned a = o.mesh.i[i] + base, b = o.mesh.i[i + 1] + base, c = o.mesh.i[i + 2] + base;
                if (a == b || b == c || a == c) continue;
                std::fprintf(f, "f %u//%u %u//%u %u//%u\n", a, a, b, b, c, c);
            }
            base += (unsigned)nv;
        }
    }
    std::fclose(f);
    return true;
}

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> r;
    std::string c;
    for (char ch : s) {
        if (ch == d) { r.push_back(c); c.clear(); }
        else if (ch != '\n' && ch != '\r') c += ch;
    }
    r.push_back(c);
    return r;
}

static std::string clean(const char* s) {
    std::string r = s;
    std::replace(r.begin(), r.end(), '|', '/');
    return r;
}

static bool saveProject(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "PMODEL|1\n");
    std::fprintf(f, "G|%g|%g|%g|%g|%g|%g|%g|%g|%g\n", g_ambI, g_ambCol[0], g_ambCol[1], g_ambCol[2], g_spec, g_shin, g_bg[0], g_bg[1], g_bg[2]);
    for (int i = 0; i < 4; i++) {
        auto& l = g_lights[i];
        std::fprintf(f, "L|%d|%d|%d|%g|%g|%g|%g|%g|%g\n", i, (int)l.on, l.type, l.pos[0], l.pos[1], l.pos[2], l.col[0], l.col[1], l.col[2]);
        std::fprintf(f, "M|%d|%g|%g\n", i, l.inten, l.falloff);
    }
    for (auto& p : g_params) std::fprintf(f, "P|%s|%s\n", clean(p.name).c_str(), clean(p.e.buf).c_str());
    for (auto& o : g_objs) {
        std::fprintf(f, "O|%d|%d|%d|%g|%g|%g|%s", o.type, o.segs, (int)o.visible, o.color[0], o.color[1], o.color[2], clean(o.name).c_str());
        for (int i = 0; i < NE; i++) std::fprintf(f, "|%s", clean(o.e[i].buf).c_str());
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

static bool loadProject(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    std::vector<Param> np;
    std::vector<Object> no;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        auto t = split(line, '|');
        if (t[0] == "G" && t.size() >= 10) {
            g_ambI = (float)atof(t[1].c_str());
            for (int i = 0; i < 3; i++) g_ambCol[i] = (float)atof(t[2 + i].c_str());
            g_spec = (float)atof(t[5].c_str());
            g_shin = (float)atof(t[6].c_str());
            for (int i = 0; i < 3; i++) g_bg[i] = (float)atof(t[7 + i].c_str());
        } else if (t[0] == "L" && t.size() >= 10) {
            int i = std::clamp(atoi(t[1].c_str()), 0, 3);
            Light& l = g_lights[i];
            l.on = atoi(t[2].c_str()) != 0;
            l.type = atoi(t[3].c_str());
            for (int c = 0; c < 3; c++) { l.pos[c] = (float)atof(t[4 + c].c_str()); l.col[c] = (float)atof(t[7 + c].c_str()); }
        } else if (t[0] == "M" && t.size() >= 4) {
            int i = std::clamp(atoi(t[1].c_str()), 0, 3);
            g_lights[i].inten = (float)atof(t[2].c_str());
            g_lights[i].falloff = (float)atof(t[3].c_str());
        } else if (t[0] == "P" && t.size() >= 3) {
            np.emplace_back(t[1].c_str(), t[2].c_str());
        } else if (t[0] == "O" && t.size() >= 8 + NE) {
            Object o;
            o.type = std::clamp(atoi(t[1].c_str()), 0, (int)NTYPES - 1);
            o.segs = std::clamp(atoi(t[2].c_str()), 4, 128);
            o.visible = atoi(t[3].c_str()) != 0;
            for (int c = 0; c < 3; c++) o.color[c] = (float)atof(t[4 + c].c_str());
            snprintf(o.name, sizeof o.name, "%s", t[7].c_str());
            for (int i = 0; i < NE; i++) o.e[i].set(t[8 + i].c_str());
            no.push_back(std::move(o));
        }
    }
    std::fclose(f);
    g_params = std::move(np);
    g_objs = std::move(no);
    g_sel = g_objs.empty() ? -1 : 0;
    return true;
}

// ----------------------------------------------------------------------------
// Demo scene
// ----------------------------------------------------------------------------
static void resetScene() {
    g_params = {Param("base_w", "80"), Param("base_d", "50"), Param("base_h", "6"), Param("post_r", "4"),
                Param("post_h", "30"), Param("posts", "4"), Param("pitch", "18"), Param("ball_r", "post_r*1.6")};
    g_objs.clear();

    Object plate;
    snprintf(plate.name, sizeof plate.name, "Base plate");
    plate.setType(BOX);
    plate.e[0].set("base_w"); plate.e[1].set("base_d"); plate.e[2].set("base_h");
    plate.e[E_POS + 2].set("base_h/2");
    plate.color[0] = 0.45f; plate.color[1] = 0.58f; plate.color[2] = 0.80f;
    g_objs.push_back(plate);

    Object posts;
    snprintf(posts.name, sizeof posts.name, "Posts");
    posts.setType(CYL);
    posts.e[0].set("post_r"); posts.e[1].set("post_r"); posts.e[2].set("post_h");
    posts.e[E_POS + 0].set("-(posts-1)*pitch/2");
    posts.e[E_POS + 2].set("base_h+post_h/2");
    posts.e[E_CNT].set("posts");
    posts.e[E_AOFF + 0].set("pitch");
    posts.color[0] = 0.88f; posts.color[1] = 0.52f; posts.color[2] = 0.32f;
    g_objs.push_back(posts);

    Object caps;
    snprintf(caps.name, sizeof caps.name, "Caps");
    caps.setType(SPH);
    caps.e[0].set("ball_r");
    caps.e[E_POS + 0].set("-(posts-1)*pitch/2");
    caps.e[E_POS + 2].set("base_h+post_h+ball_r*0.6");
    caps.e[E_CNT].set("posts");
    caps.e[E_AOFF + 0].set("pitch");
    caps.color[0] = 0.92f; caps.color[1] = 0.84f; caps.color[2] = 0.35f;
    g_objs.push_back(caps);

    Object halo;
    snprintf(halo.name, sizeof halo.name, "Halo");
    halo.setType(TOR);
    halo.e[0].set("base_d*0.45"); halo.e[1].set("1.5");
    halo.e[E_POS + 2].set("base_h+post_h+14");
    halo.color[0] = 0.55f; halo.color[1] = 0.85f; halo.color[2] = 0.65f;
    g_objs.push_back(halo);
    g_sel = 0;

    g_lights[0] = Light(); g_lights[0].on = true; g_lights[0].type = 0;
    g_lights[0].pos[0] = 1; g_lights[0].pos[1] = -1.2f; g_lights[0].pos[2] = 2; g_lights[0].inten = 0.95f;
    g_lights[1] = Light(); g_lights[1].on = true; g_lights[1].type = 0;
    g_lights[1].pos[0] = -1.5f; g_lights[1].pos[1] = 1; g_lights[1].pos[2] = 0.6f;
    g_lights[1].col[0] = 0.55f; g_lights[1].col[1] = 0.7f; g_lights[1].col[2] = 1; g_lights[1].inten = 0.5f;
    g_lights[2] = Light(); g_lights[2].on = false; g_lights[2].type = 1;
    g_lights[2].pos[0] = 40; g_lights[2].pos[1] = 40; g_lights[2].pos[2] = 70;
    g_lights[2].col[0] = 1; g_lights[2].col[1] = 0.85f; g_lights[2].col[2] = 0.6f;
    g_lights[3] = Light(); g_lights[3].type = 1;
    g_lights[3].pos[0] = -50; g_lights[3].pos[1] = -30; g_lights[3].pos[2] = 50;
}

static void addObject(int type) {
    Object o;
    o.setType(type);
    snprintf(o.name, sizeof o.name, "%s %d", kTypeNames[type], (int)g_objs.size() + 1);
    static const float pal[6][3] = {{0.6f, 0.7f, 0.9f}, {0.9f, 0.6f, 0.5f}, {0.6f, 0.85f, 0.6f},
                                    {0.9f, 0.8f, 0.45f}, {0.75f, 0.6f, 0.9f}, {0.6f, 0.85f, 0.85f}};
    memcpy(o.color, pal[g_objs.size() % 6], sizeof o.color);
    g_objs.push_back(std::move(o));
    g_sel = (int)g_objs.size() - 1;
}

// ----------------------------------------------------------------------------
// Camera + rendering
// ----------------------------------------------------------------------------
struct V3 {
    double x, y, z;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
};
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 norm(V3 a) { double l = std::sqrt(dot(a, a)); return l > 0 ? a * (1 / l) : a; }

struct Camera {
    float yaw = -55, pitch = 28, dist = 170, fov = 45;
    V3 target{0, 0, 18};
    V3 eye() const {
        double y = yaw * PI / 180, p = pitch * PI / 180;
        return target + V3{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)} * dist;
    }
};
static Camera g_cam;

static void loadView(int w, int h) {
    double near_ = std::max(0.05, g_cam.dist * 0.02), far_ = g_cam.dist * 60 + g_gridSize * 4;
    double top = near_ * std::tan(g_cam.fov * PI / 360.0), right = top * w / std::max(1, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, near_, far_);
    V3 e = g_cam.eye(), f = norm(g_cam.target - e), s = norm(cross(f, V3{0, 0, 1})), u = cross(s, f);
    double m[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -dot(s, e), -dot(u, e), dot(f, e), 1};
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixd(m);
}

static void drawMesh(const Mesh& m) {
    glVertexPointer(3, GL_FLOAT, 0, m.v.data());
    glNormalPointer(GL_FLOAT, 0, m.n.data());
    glDrawElements(GL_TRIANGLES, (GLsizei)m.i.size(), GL_UNSIGNED_INT, m.i.data());
}

static void applyInstance(const Object& o, int k) {
    double pos[3], rot[3];
    instanceXform(o, k, pos, rot);
    glPushMatrix();
    glTranslated(pos[0], pos[1], pos[2]);
    glRotated(rot[2], 0, 0, 1);
    glRotated(rot[1], 0, 1, 0);
    glRotated(rot[0], 1, 0, 0);
}

static void setupLights() {
    float amb[4] = {g_ambCol[0] * g_ambI, g_ambCol[1] * g_ambI, g_ambCol[2] * g_ambI, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
    for (int i = 0; i < 4; i++) {
        GLenum L = GL_LIGHT0 + i;
        const Light& l = g_lights[i];
        if (!l.on) { glDisable(L); continue; }
        glEnable(L);
        float p[4] = {l.pos[0], l.pos[1], l.pos[2], l.type == 1 ? 1.f : 0.f};
        float d[4] = {l.col[0] * l.inten, l.col[1] * l.inten, l.col[2] * l.inten, 1};
        float a0[4] = {0, 0, 0, 1};
        glLightfv(L, GL_POSITION, p);
        glLightfv(L, GL_DIFFUSE, d);
        glLightfv(L, GL_SPECULAR, d);
        glLightfv(L, GL_AMBIENT, a0);
        glLightf(L, GL_CONSTANT_ATTENUATION, 1.f);
        glLightf(L, GL_LINEAR_ATTENUATION, 0.f);
        glLightf(L, GL_QUADRATIC_ATTENUATION, l.type == 1 ? l.falloff : 0.f);
    }
}

static void drawGrid() {
    glDisable(GL_LIGHTING);
    glLineWidth(1);
    float half = g_gridSize / 2, step = std::max(1.f, g_gridSize / 20);
    glColor4f(1, 1, 1, 0.12f);
    glBegin(GL_LINES);
    for (float t = -half; t <= half + 0.001f; t += step) {
        glVertex3f(t, -half, 0); glVertex3f(t, half, 0);
        glVertex3f(-half, t, 0); glVertex3f(half, t, 0);
    }
    glEnd();
    glLineWidth(2);
    glBegin(GL_LINES);
    glColor3f(0.9f, 0.25f, 0.25f); glVertex3f(0, 0, 0); glVertex3f(half, 0, 0);
    glColor3f(0.3f, 0.85f, 0.3f);  glVertex3f(0, 0, 0); glVertex3f(0, half, 0);
    glColor3f(0.35f, 0.5f, 1.0f);  glVertex3f(0, 0, 0); glVertex3f(0, 0, half);
    glEnd();
    glLineWidth(1);
}

static void drawGizmos() {
    glDisable(GL_LIGHTING);
    float s = std::max(2.f, g_gridSize * 0.025f);
    glLineWidth(2);
    glBegin(GL_LINES);
    for (auto& l : g_lights) {
        if (!l.on) continue;
        glColor3f(l.col[0], l.col[1], l.col[2]);
        V3 p{l.pos[0], l.pos[1], l.pos[2]};
        if (l.type == 0) p = norm(p) * (g_gridSize * 0.45);
        glVertex3d(p.x - s, p.y, p.z); glVertex3d(p.x + s, p.y, p.z);
        glVertex3d(p.x, p.y - s, p.z); glVertex3d(p.x, p.y + s, p.z);
        glVertex3d(p.x, p.y, p.z - s); glVertex3d(p.x, p.y, p.z + s);
        glVertex3d(p.x, p.y, p.z); glVertex3d(g_cam.target.x, g_cam.target.y, g_cam.target.z);
    }
    glEnd();
    glLineWidth(1);
}

static void renderScene(int w, int h) {
    glViewport(0, 0, w, h);
    glClearColor(g_bg[0], g_bg[1], g_bg[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(0x809D);  // GL_MULTISAMPLE
    loadView(w, h);

    if (g_grid) drawGrid();

    setupLights();
    glEnable(GL_LIGHTING);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    float spec[4] = {g_spec, g_spec, g_spec, 1};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, g_shin);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);

    if (g_wire) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    for (auto& o : g_objs) {
        if (!o.visible) continue;
        glColor3f(o.color[0], o.color[1], o.color[2]);
        for (int k = 0; k < o.count(); k++) {
            applyInstance(o, k);
            drawMesh(o.mesh);
            glPopMatrix();
        }
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // selection outline
    if (g_sel >= 0 && g_sel < (int)g_objs.size() && g_objs[g_sel].visible) {
        const Object& o = g_objs[g_sel];
        glDisable(GL_LIGHTING);
        glColor4f(1.f, 0.78f, 0.2f, 0.55f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.f, -1.f);
        for (int k = 0; k < o.count(); k++) {
            applyInstance(o, k);
            drawMesh(o.mesh);
            glPopMatrix();
        }
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisable(GL_LIGHTING);
    if (g_gizmos) drawGizmos();
}

// ----------------------------------------------------------------------------
// UI helpers
// ----------------------------------------------------------------------------
static void exprBox(const char* id, Expr& e) {
    if (!e.ok) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.12f, 0.12f, 1));
    ImGui::InputText(id, e.buf, sizeof e.buf);
    if (!e.ok) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        if (e.ok) ImGui::SetTooltip("= %.6g", e.val);
        else ImGui::SetTooltip("Invalid expression (showing last good value %.6g)", e.val);
    }
}

static void exprField(const char* label, Expr& e) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-130);
    exprBox("##e", e);
    ImGui::SameLine();
    ImGui::Text("%s  [%.4g]", label, e.val);
    ImGui::PopID();
}

static void exprRow3(const char* label, const char* id, Expr* e) {
    ImGui::TextUnformatted(label);
    ImGui::PushID(id);
    float sp = ImGui::GetStyle().ItemSpacing.x, w = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, sp);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(w);
        exprBox("##e", e[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
}

static void uiModelWindow(float H) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, H - 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("Model");

    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("name | expression | drag to tweak");
        int del = -1;
        for (int i = 0; i < (int)g_params.size(); i++) {
            Param& p = g_params[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(85);
            ImGui::InputText("##n", p.name, sizeof p.name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(135);
            exprBox("##x", p.e);
            ImGui::SameLine();
            float v = (float)p.e.val;
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("##d", &v, std::max(0.01f, std::fabs(v) * 0.01f), 0, 0, "%.4g")) {
                char t[64]; snprintf(t, sizeof t, "%.6g", v); p.e.set(t);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) del = i;
            ImGui::PopID();
        }
        if (del >= 0) g_params.erase(g_params.begin() + del);
        if (ImGui::Button("+ Parameter")) {
            char n[32]; snprintf(n, sizeof n, "p%d", (int)g_params.size() + 1);
            g_params.emplace_back(n, "10");
        }
    }

    if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < (int)g_objs.size(); i++) {
            ImGui::PushID(i);
            ImGui::Checkbox("##vis", &g_objs[i].visible);
            ImGui::SameLine();
            if (ImGui::Selectable(g_objs[i].name, g_sel == i)) g_sel = i;
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add")) ImGui::OpenPopup("addobj");
        if (ImGui::BeginPopup("addobj")) {
            for (int t = 0; t < NTYPES; t++)
                if (ImGui::Selectable(kTypeNames[t])) addObject(t);
            ImGui::EndPopup();
        }
        bool has = g_sel >= 0 && g_sel < (int)g_objs.size();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate") && has) {
            Object c = g_objs[g_sel];
            size_t n = strlen(c.name);
            if (n + 6 < sizeof c.name) strcat(c.name, " copy");
            g_objs.insert(g_objs.begin() + g_sel + 1, c);
            g_sel++;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete") && has) {
            g_objs.erase(g_objs.begin() + g_sel);
            g_sel = std::min(g_sel, (int)g_objs.size() - 1);
        }
        has = g_sel >= 0 && g_sel < (int)g_objs.size();
        ImGui::SameLine();
        if (ImGui::Button("^") && has && g_sel > 0) { std::swap(g_objs[g_sel], g_objs[g_sel - 1]); g_sel--; }
        ImGui::SameLine();
        if (ImGui::Button("v") && has && g_sel + 1 < (int)g_objs.size()) { std::swap(g_objs[g_sel], g_objs[g_sel + 1]); g_sel++; }
    }
    ImGui::End();
}

static void uiInspector(float W, float H) {
    ImGui::SetNextWindowPos(ImVec2(W - 400, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(390, H - 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("Properties");
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Object")) {
            if (g_sel >= 0 && g_sel < (int)g_objs.size()) {
                Object& o = g_objs[g_sel];
                ImGui::InputText("Name", o.name, sizeof o.name);
                int t = o.type;
                if (ImGui::Combo("Type", &t, kTypeNames, NTYPES) && t != o.type) o.setType(t);
                ImGui::SeparatorText("Dimensions");
                for (int i = 0; i < kPCount[o.type]; i++) exprField(kPLabels[o.type][i], o.e[i]);
                ImGui::SliderInt("Resolution", &o.segs, 8, 128);
                ImGui::SeparatorText("Transform");
                exprRow3("Position", "pos", &o.e[E_POS]);
                exprRow3("Rotation (deg, applied X then Y then Z)", "rot", &o.e[E_ROT]);
                ImGui::SeparatorText("Linear array");
                exprField("Count", o.e[E_CNT]);
                exprRow3("Offset per copy", "aoff", &o.e[E_AOFF]);
                ImGui::SeparatorText("Appearance");
                ImGui::ColorEdit3("Color", o.color);
                ImGui::Checkbox("Visible", &o.visible);
                ImGui::TextDisabled("%zu verts, %zu tris per copy", o.mesh.v.size() / 3, o.mesh.i.size() / 3);
            } else {
                ImGui::TextDisabled("No object selected.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
            ImGui::SeparatorText("Global");
            ImGui::ColorEdit3("Ambient color", g_ambCol);
            ImGui::SliderFloat("Ambient", &g_ambI, 0, 1);
            ImGui::SliderFloat("Specular", &g_spec, 0, 1);
            ImGui::SliderFloat("Shininess", &g_shin, 1, 128);
            for (int i = 0; i < 4; i++) {
                Light& l = g_lights[i];
                char hdr[32];
                snprintf(hdr, sizeof hdr, "Light %d %s", i + 1, l.on ? "(on)" : "(off)");
                ImGui::PushID(i);
                if (ImGui::CollapsingHeader(hdr, i < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    ImGui::Checkbox("Enabled", &l.on);
                    const char* lt[] = {"Directional", "Point"};
                    ImGui::Combo("Kind", &l.type, lt, 2);
                    ImGui::DragFloat3(l.type == 0 ? "Direction (from)" : "Position", l.pos, l.type == 0 ? 0.02f : 1.f);
                    ImGui::ColorEdit3("Color", l.col);
                    ImGui::SliderFloat("Intensity", &l.inten, 0, 2);
                    if (l.type == 1) ImGui::SliderFloat("Falloff", &l.falloff, 0, 0.002f, "%.5f", ImGuiSliderFlags_Logarithmic);
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("View")) {
            ImGui::ColorEdit3("Background", g_bg);
            ImGui::Checkbox("Grid", &g_grid);
            ImGui::SameLine();
            ImGui::Checkbox("Light gizmos", &g_gizmos);
            ImGui::SameLine();
            ImGui::Checkbox("Wireframe", &g_wire);
            ImGui::SliderFloat("Grid size", &g_gridSize, 20, 1000);
            ImGui::SliderFloat("Field of view", &g_cam.fov, 15, 90);
            if (ImGui::Button("Reset camera")) g_cam = Camera();
            size_t tv = 0, tt = 0;
            for (auto& o : g_objs) if (o.visible) { tv += o.mesh.v.size() / 3 * o.count(); tt += o.mesh.i.size() / 3 * o.count(); }
            ImGui::Text("Scene: %zu verts, %zu tris", tv, tt);
            ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);
            ImGui::SeparatorText("Controls");
            ImGui::BulletText("Left drag: orbit");
            ImGui::BulletText("Right/middle drag or Shift+left: pan");
            ImGui::BulletText("Wheel: zoom");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("File")) {
            static char proj[128] = "model.pm", obj[128] = "model.obj";
            static bool yup = true;
            ImGui::SeparatorText("Project");
            ImGui::InputText("##proj", proj, sizeof proj);
            if (ImGui::Button("Save")) snprintf(g_status, sizeof g_status, saveProject(proj) ? "Saved %s" : "Could not write %s", proj);
            ImGui::SameLine();
            if (ImGui::Button("Load")) snprintf(g_status, sizeof g_status, loadProject(proj) ? "Loaded %s" : "Could not read %s", proj);
            ImGui::SameLine();
            if (ImGui::Button("New demo scene")) { resetScene(); snprintf(g_status, sizeof g_status, "Demo scene restored"); }
            ImGui::SeparatorText("Export mesh");
            ImGui::InputText("##obj", obj, sizeof obj);
            ImGui::Checkbox("Y-up (convert from Z-up)", &yup);
            if (ImGui::Button("Export OBJ")) snprintf(g_status, sizeof g_status, exportOBJ(obj, yup) ? "Exported %s" : "Could not write %s", obj);
            if (g_status[0]) ImGui::TextWrapped("%s", g_status);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ----------------------------------------------------------------------------
int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_SAMPLES, 4);
    GLFWwindow* win = glfwCreateWindow(1440, 860, "pmodel - parametric modeller", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4;
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL2_Init();

    resetScene();

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        updateScene();

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int w, h;
        glfwGetFramebufferSize(win, &w, &h);

        // camera interaction
        if (!io.WantCaptureMouse) {
            bool pan = io.MouseDown[1] || io.MouseDown[2] || (io.MouseDown[0] && io.KeyShift);
            if (pan) {
                V3 e = g_cam.eye(), f = norm(g_cam.target - e), s = norm(cross(f, V3{0, 0, 1})), u = cross(s, f);
                double k = g_cam.dist * 0.0014;
                g_cam.target = g_cam.target - s * (io.MouseDelta.x * k) + u * (io.MouseDelta.y * k);
            } else if (io.MouseDown[0]) {
                g_cam.yaw -= io.MouseDelta.x * 0.4f;
                g_cam.pitch = std::clamp(g_cam.pitch + io.MouseDelta.y * 0.4f, -89.f, 89.f);
            }
            if (io.MouseWheel != 0) g_cam.dist = std::clamp(g_cam.dist * std::pow(0.9f, io.MouseWheel), 1.f, 5000.f);
        }

        uiModelWindow((float)io.DisplaySize.y);
        uiInspector((float)io.DisplaySize.x, (float)io.DisplaySize.y);

        ImGui::Render();
        renderScene(w, h);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
