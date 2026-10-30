// SPDX-License-Identifier: GPL-3.0-only
// See rename.h. Follows SoylentGreed's rename route (RepublicLAN, read against
// the disassembly of 1.1.1.9).
#include "rename.h"
#include "commands.h"
#include <string.h>
#include <vector>

typedef unsigned __int64 U64;

#define WORLD_RVA 0x9D4F10
#define CITY_LIST 0x10348
#define CITY_NAME 0x10
#define CITY_POSITION 4
#define CITY_CHARS 0x20
#define MAX_CITIES 4096
#define ROAD_NETWORK 0xF7A0
#define NETWORK_NODES 0x298
#define JUNCTION_OF_NODE 0xC0
#define JUNCTION_NODES 0x88
#define JUNCTION_NAME 4
#define JUNCTION_NAMED 0xD0
#define JUNCTION_CHARS 0x40
#define MAX_NODES 16
#define MAX_CHARS 0x40
#define WINDOW_LIST 0xCAC0    // world: the open windows (begin, end)
#define MAX_WINDOWS 1024
#define WINDOW_TARGET 0x240   // window: the object it shows
#define WINDOW_DRAWN 0x258    // window: 0 = draw afresh
#define NAMES_SHOWN 0xFC8     // world: float, -0.5 after a rename
#define LABELS_RVA 0x32FA50       // the city labels, printed in the interface routine
#define LABELS_CALL 0x318E7F      // its call there (world in rcx)
#define FONT_MANAGER_RVA 0x996FB0 // the C3D_FONTMANAGER the labels print with
#define REALLY_RENDER_IMPORT 0x86D680 // C3D_FONTMANAGER::SetReallyRender(bool)
#define LABEL_FRAMES 4

// The city labels (32fa50) print each name through the font manager, which
// keeps what it drew while really-render is off; the windows switch it on for
// some frames (6ea2cb, 6eb92b), so a renamed city's label changed only when
// its card was opened. After a shared city rename it is on for a few frames
// around the labels.
static volatile LONG g_labelFrames = 0;
static BYTE* g_labelRelay = nullptr;

typedef void (*LabelsFn)(U64 world);
typedef void (*ReallyRenderFn)(void* manager, bool on);

static void RelayedLabels(U64 world)
{
    if (InterlockedCompareExchange(&g_labelFrames, 0, 0) > 0) {
        InterlockedDecrement(&g_labelFrames);
        __try {
            (*(ReallyRenderFn*)(g_exeBase + REALLY_RENDER_IMPORT))(g_exeBase + FONT_MANAGER_RVA, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    ((LabelsFn)(g_exeBase + LABELS_RVA))(world);
}

bool Rename_InstallLabelRefresh()
{
    BYTE* call = g_exeBase + LABELS_CALL;
    int rel = 0;
    memcpy(&rel, call + 1, 4);
    if (call[0] != 0xE8 || LABELS_CALL + 5 + rel != LABELS_RVA) {
        Logf("republicmp  RENAME: the city labels' call differs - a shared city name shows when its card is opened");
        return false;
    }
    BYTE* relay = AllocNear(call, 4096);
    if (!relay) return false;
    long long to = (long long)relay - (long long)(call + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    DWORD old;
    if (!VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    static const unsigned char kJump[6] = { 0xFF, 0x25, 0, 0, 0, 0 }; // jmp [rip]
    U64 target = (U64)&RelayedLabels;
    memcpy(relay, kJump, sizeof(kJump));
    memcpy(relay + sizeof(kJump), &target, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    int rel32 = (int)to;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(call + 1, &rel32, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    g_labelRelay = relay;
    return true;
}
#define MAGIC 0x314D414E

enum { KIND_CITY = 3, KIND_CROSSROAD = 0xA };

#pragma pack(push, 1)
struct RenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[MAX_CHARS];
};
#pragma pack(pop)

void Rename_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned Fnv(const BYTE* b, size_t n, unsigned h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h | 1u;
}

static unsigned CityCheck(U64 city) { return Fnv((const BYTE*)(city + CITY_POSITION), 12); }

static unsigned NodeCheck(U64 node)
{
    unsigned w[3] = { 0x4e4f4445u, *(unsigned*)(node + 4), *(unsigned*)(node + 0xc) };
    return Fnv((const BYTE*)w, sizeof(w));
}

// A vector of pointers: begin and count (false: not one, or longer than max).
static bool List(U64 at, unsigned max, U64& begin, unsigned& count)
{
    U64 b = Q(at), e = Q(at + 8);
    begin = b;
    count = 0;
    if (!b && !e) return true;
    if (!b || e < b || (e - b) % 8 || (e - b) / 8 > max) return false;
    count = (unsigned)((e - b) / 8);
    return true;
}

// The field's owner: a listed city (its index) or a junction (its first node's index).
static bool Identify(U64 field, RenameMsg& m)
{
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (List(world + CITY_LIST, MAX_CITIES, begin, n))
        for (unsigned i = 0; i < n; i++) {
            U64 city = Q(begin + (U64)i * 8);
            if (city && city + CITY_NAME == field) {
                m.kind = KIND_CITY;
                m.index = i;
                m.check = CityCheck(city);
                return true;
            }
        }
    // A junction: the field is +4 of an object whose first node's +c0 is that object.
    U64 junction = field - JUNCTION_NAME, nodes = 0, roads = Q(world + ROAD_NETWORK);
    unsigned count = 0;
    if (!roads || !List(junction + JUNCTION_NODES, MAX_NODES, nodes, count) || !count) return false;
    U64 first = Q(nodes);
    if (!first || Q(first + JUNCTION_OF_NODE) != junction) return false;
    U64 all = 0;
    unsigned total = 0;
    if (!List(roads + NETWORK_NODES, 10000000, all, total)) return false;
    for (unsigned i = 0; i < total; i++)
        if (Q(all + (U64)i * 8) == first) {
            m.kind = KIND_CROSSROAD;
            m.index = i;
            m.check = NodeCheck(first);
            return true;
        }
    return false;
}

static bool GuardedIdentify(U64 field, RenameMsg& m)
{
    __try {
        return Identify(field, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CopyText(U64 text, unsigned length, RenameMsg& m)
{
    __try {
        const unsigned short* t = (const unsigned short*)text;
        unsigned n = 0;
        while (n < length && n < MAX_CHARS && t[n]) n++;
        if (n >= length || n >= MAX_CHARS) n = (length < MAX_CHARS ? length : MAX_CHARS) - 1; // as swprintf truncates
        memcpy(m.text, t, n * 2);
        m.chars = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Rename_Capture(U64 field, U64 length, U64 text)
{
    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    if (!field || !text || !GuardedIdentify(field, m)) return false;
    unsigned want = m.kind == KIND_CITY ? CITY_CHARS : JUNCTION_CHARS;
    if ((unsigned)length != want || !CopyText(text, want, m)) {
        Logf("republicmp  RENAME: a %s name not shared - unexpected length %u; written on this PC only",
             m.kind == KIND_CITY ? "city" : "crossroad", (unsigned)length);
        return false;
    }
    Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
    return true;
}

static const char* Apply(const RenameMsg& m)
{
    U64 world = (U64)World(), begin = 0, field = 0, flag = 0;
    unsigned n = 0;
    if (m.kind == KIND_CITY) {
        if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || m.index >= n) return "the city is not here";
        U64 city = Q(begin + (U64)m.index * 8);
        if (!city || CityCheck(city) != m.check) return "the city is not here";
        field = city + CITY_NAME;
    } else {
        U64 roads = Q(world + ROAD_NETWORK);
        if (!roads || !List(roads + NETWORK_NODES, 10000000, begin, n) || m.index >= n) return "the crossroad is not here";
        U64 node = Q(begin + (U64)m.index * 8);
        if (!node || NodeCheck(node) != m.check) return "the crossroad is not here";
        U64 junction = Q(node + JUNCTION_OF_NODE);
        if (!junction) return "the node has no crossroad here";
        field = junction + JUNCTION_NAME;
        flag = junction + JUNCTION_NAMED;
    }
    unsigned short t[MAX_CHARS + 1] = {};
    memcpy(t, m.text, m.chars * 2);
    memcpy((void*)field, t, (m.chars + 1) * 2);
    if (flag) *(BYTE*)flag = 1;
    // What the rename window does after its write (8019ce..801a63): the open
    // windows of that city or crossroad are drawn afresh, and the world's
    // +fc8 is set to -0.5 so the names shown are made again. Without it the
    // name changed and this PC kept showing the old one.
    U64 target = m.kind == KIND_CITY ? field - CITY_NAME : field - JUNCTION_NAME;
    U64 windows = 0;
    unsigned count = 0;
    if (List(world + WINDOW_LIST, MAX_WINDOWS, windows, count))
        for (unsigned i = 0; i < count; i++) {
            U64 window = Q(windows + (U64)i * 8);
            if (window && Q(window + WINDOW_TARGET) == target) *(int*)(window + WINDOW_DRAWN) = 0;
        }
    *(float*)(world + NAMES_SHOWN) = -0.5f;
    if (m.kind == KIND_CITY) InterlockedExchange(&g_labelFrames, LABEL_FRAMES);
    return nullptr;
}

// A city the game made by itself (a building placed away from every city,
// 330690 -> 32f450) is named with C3D_LANGUAGE::GetString of a random entry:
// the same entry is another name in another language. The host names the
// cities that appeared since the last boundary for every PC.
static int g_knownCities = -1;
// Every PC, at each boundary: a city that appeared or whose name changed is
// logged with its place, name and position, so the two PCs' logs show where
// their cities differ. A hash of each city's position and name, by index.
static std::vector<unsigned> g_cityNames;

void Rename_ResetCities()
{
    g_knownCities = -1;
    g_cityNames.clear();
}

static void ShareNewCities()
{
    U64 begin = 0;
    unsigned n = 0;
    if (!List((U64)World() + CITY_LIST, MAX_CITIES, begin, n)) return;
    if (g_knownCities < 0 || n < (unsigned)g_knownCities) {
        g_knownCities = (int)n;
        return;
    }
    for (unsigned i = (unsigned)g_knownCities; i < n; i++) {
        U64 city = Q(begin + (U64)i * 8);
        if (!city) continue;
        RenameMsg m;
        memset(&m, 0, sizeof(m));
        m.magic = MAGIC;
        m.kind = KIND_CITY;
        m.index = i;
        m.check = CityCheck(city);
        if (!CopyText(city + CITY_NAME, CITY_CHARS, m)) continue;
        Logf("republicmp  RENAME: city #%u appeared - its name here goes to every PC", i);
        Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
    }
    g_knownCities = (int)n;
}

static void LogCityChanges()
{
    U64 begin = 0;
    unsigned n = 0;
    if (!List((U64)World() + CITY_LIST, MAX_CITIES, begin, n)) return;
    if (g_cityNames.size() > n) g_cityNames.resize(n);
    bool first = g_cityNames.empty() && n;
    for (unsigned i = 0; i < n; i++) {
        U64 city = Q(begin + (U64)i * 8);
        if (!city) continue;
        unsigned h = Fnv((const BYTE*)(city + CITY_NAME), CITY_CHARS * 2, CityCheck(city));
        if (i < g_cityNames.size() && g_cityNames[i] == h) continue;
        bool appeared = i >= g_cityNames.size();
        if (appeared) g_cityNames.push_back(h);
        else g_cityNames[i] = h;
        if (first) continue;
        char name[CITY_CHARS * 3 + 1] = {};
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)(city + CITY_NAME), -1, name, sizeof(name) - 1, nullptr, nullptr);
        const float* p = (const float*)(city + CITY_POSITION);
        Logf("republicmp  CITIES: #%u of %u %s - [%s] at (%.1f,%.1f)", i, n, appeared ? "appeared" : "renamed", name, p[0], p[2]);
    }
    if (first) Logf("republicmp  CITIES: %u at the start", n);
}

void Rename_LogCities()
{
    __try {
        LogCityChanges();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Rename_ShareNewCities()
{
    __try {
        ShareNewCities();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool Rename_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    unsigned limit = m.kind == KIND_CITY ? CITY_CHARS : m.kind == KIND_CROSSROAD ? JUNCTION_CHARS : 0;
    if (m.magic != MAGIC || !limit || m.chars >= limit) return false;
    const char* why;
    __try {
        why = Apply(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    Logf("republicmp  RENAME: %s name (%u characters): %s", m.kind == KIND_CITY ? "city" : "crossroad", m.chars, why ? why : "made");
    return why == nullptr;
}
