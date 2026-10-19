// SPDX-License-Identifier: GPL-3.0-only
// See immigrant_invite.h. Follows SoylentGreed's invite replay (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "immigrant_invite.h"
#include "commands.h"
#include "world_refs.h"
#include "../determinism/determinism.h"
#include <emmintrin.h>
#include <limits.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define PRESS_OFF_SLOT 0x86C8B0   // C3D_INPUT::GetMouseLeftPressAOff
#define CREATE_CITIZEN_RVA 0x823470 // (world, building, partner, bloc, a5, a6)
#define EXPERT_FACTOR_RVA 0x90A364  // 2.5
#define EDUCATION_LOW_RVA 0x90A2A8  // 2.05
#define EDUCATION_HIGH_RVA 0x90A310 // 2.30

struct InviteSite {
    unsigned site, variant;
};
static const InviteSite kSites[3] = { { 0x6FEDBA, 0 }, { 0x6FF2A9, 1 }, { 0x6FF9EA, 2 } };
static const unsigned char kCreateEntry[16] = { 0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55,
                                                0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xd0 };
// The replay's own draws (the partner rolls, the experts' education): fixed
// keys of the shared random stream, the same on every PC.
#define PARTNER_DRAW_SITE 0x6FEDC0
#define EDUCATION_DRAW_SITE 0x6FF9F0

// World words.
#define DOLLARS 0x580
#define RUBLES 0x588
#define PRICE_A 0xE7C      // third world
#define PRICE_B 0xE80      // Soviet bloc, experts (x2.5)
#define FACTOR_A 0x1369C
#define FACTOR_B 0x13694
#define ALLOW_A 0x13698
#define ALLOW_B 0x13690
#define CAMPAIGN 0x13178   // != -1: a campaign step (its own branch, not carried)
#define COUNT_A 0x11580    // month totals
#define COST_A 0x11568
#define UNIT_A 0x11560
#define COUNT_B 0x1157C
#define COST_B 0x1156C
#define UNIT_B 0x11564
#define CITIZENS 0x126A8
#define RESIDENTIAL 2      // type +0x360
#define MAX_RESIDENTS 100000

typedef unsigned __int64 U64;
typedef bool (*GetterFn)(void* input);
typedef U64 (*CreateFn)(U64 world, U64 building, U64 partner, U64 bloc, U64 a5, U64 a6);

#pragma pack(push, 1)
struct InviteMsg {
    unsigned variant; // 0 third world (dollars), 1 Soviet bloc (rubles), 2 experts (rubles)
    int heads;        // the clicking PC's count, for the log
    RouteRef building;
};
#pragma pack(pop)

static GetterFn g_getter = nullptr;
static bool g_ready = false;

void ImmigrantInvite_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

static bool Residential(U64 b)
{
    U64 type = Q(b + 0x318);
    return type && *(unsigned*)(type + 0x360) == RESIDENTIAL;
}

// Residents without a partner link, as the window counts them.
static bool Heads(U64 b, int& heads)
{
    heads = 0;
    U64 begin = Q(b + 0xBC0), end = Q(b + 0xBC8);
    if (end < begin || (end - begin) % 8 || (end - begin) / 8 > MAX_RESIDENTS) return false;
    for (U64 at = begin; at < end; at += 8) {
        U64 c = Q(at);
        if (!c) return false;
        if (!Q(c + 0xC8)) ++heads;
    }
    return true;
}

static bool Capacity(U64 b, int& cap)
{
    U64 type = Q(b + 0x318), rec = type ? Q(type + 0x3A0) : 0;
    if (!rec) return false;
    cap = _mm_cvttss_si32(_mm_set_ss(*(float*)(rec + 0x8C)));
    return true;
}

// ---------------------------------------------------------------------------
// The press (the clicking PC's interface frame)
// ---------------------------------------------------------------------------

// The building and the heads at the press: one frame up, the window routine
// keeps them in r12 (building), r13 (world), r14d (heads), r15 (the window).
static bool Capture(unsigned variant, const CONTEXT& c, InviteMsg& m, const char** why)
{
    U64 world = (U64)(g_exeBase + WORLD_RVA);
    U64 b = c.R12, window = c.R15;
    if (c.R13 != world || !b || !window) { *why = "registers"; return false; }
    if (!*(BYTE*)(window + 0x20)) { *why = "inactive"; return false; } // the window ignores the press itself
    if (Q(window + 0x240) != b) { *why = "window building"; return false; }
    if (!Residential(b)) { *why = "not residential"; return false; }
    int heads = 0;
    if (!Heads(b, heads) || heads != (int)(unsigned)c.R14) { *why = "residents"; return false; }
    if (variant == 1 && *(int*)(world + CAMPAIGN) != -1) { *why = "campaign"; return false; }
    if (!Ref_Building(b, m.building)) { *why = "building reference"; return false; }
    m.variant = variant;
    m.heads = heads;
    return true;
}

static __declspec(noinline) bool RelayPress(void* input)
{
    CONTEXT c = {};
    RtlCaptureContext(&c);
    bool press = g_getter ? g_getter(input) : false;
    if (!press || !g_ready || !Commands_TakeUi()) return press;
    // One frame up: the window routine at the press site.
    DWORD64 base = 0;
    RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (!f) return false;
    void* data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
    const InviteSite* site = nullptr;
    for (const InviteSite& s : kSites)
        if (c.Rip == (DWORD64)(g_exeBase + s.site + 5)) site = &s;
    if (!site) return false;
    InviteMsg m = {};
    const char* why = "";
    bool ok = false;
    __try {
        ok = Capture(site->variant, c, m, &why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (!ok) {
        if (!strcmp(why, "inactive")) return true;
        Logf("republicmp  INVITE: the press is not carried (%s) - nothing is done", why);
        return false;
    }
    Commands_Submit(LS_CMD_INVITE, &m, sizeof(m));
    return false;
}

bool ImmigrantInvite_Install()
{
    for (const InviteSite& s : kSites) {
        BYTE* at = g_exeBase + s.site;
        int rel = 0;
        memcpy(&rel, at + 2, 4);
        if (at[0] != 0xFF || at[1] != 0x15 || s.site + 6 + rel != PRESS_OFF_SLOT) {
            Logf("republicmp  INVITE: the invite press at 0x%X differs - invitations stay as before", s.site);
            return false;
        }
    }
    float k[3];
    memcpy(&k[0], g_exeBase + EXPERT_FACTOR_RVA, 4);
    memcpy(&k[1], g_exeBase + EDUCATION_LOW_RVA, 4);
    memcpy(&k[2], g_exeBase + EDUCATION_HIGH_RVA, 4);
    if (memcmp(g_exeBase + CREATE_CITIZEN_RVA, kCreateEntry, 16) || k[0] != 2.5f || k[1] <= 0 || k[2] <= k[1]) {
        Logf("republicmp  INVITE: the citizen routine or the invite constants differ - invitations stay as before");
        return false;
    }
    g_getter = *(GetterFn*)(g_exeBase + PRESS_OFF_SLOT);
    if (!g_getter) return false;
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    void* target = (void*)&RelayPress;
    DWORD old;
    VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old);
    relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
    memcpy(relay + 6, &target, 8);
    VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, 16);
    for (const InviteSite& s : kSites) {
        BYTE* at = g_exeBase + s.site;
        long long to = (long long)relay - (long long)(at + 5);
        if (to < INT_MIN || to > INT_MAX) return false;
        unsigned char code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        int rel32 = (int)to;
        memcpy(code + 1, &rel32, 4);
        DWORD prot;
        VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &prot);
        memcpy(at, code, 6);
        VirtualProtect(at, 6, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), at, 6);
    }
    g_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// The replay (every PC, at the boundary)
// ---------------------------------------------------------------------------

static const char* Replay(U64 world, U64 b, unsigned variant, int* adults, int* partners, double* paid)
{
    auto d = [](U64 a) -> int& { return *(int*)a; };
    auto f = [](U64 a) -> float& { return *(float*)a; };
    auto money = [](U64 a) -> double& { return *(double*)a; };
    CreateFn create = (CreateFn)(g_exeBase + CREATE_CITIZEN_RVA);
    if (!Residential(b)) return "not residential";
    int heads = 0, cap = 0;
    if (!Heads(b, heads) || !Capacity(b, cap)) return "the building's residents";
    U64 city = Q(b + 0x108), stats = city ? Q(city + 0x1B8) : 0;
    if (!stats) return "no city statistics";
    // The button is active only with room and money above the base price (6fecd1, 6ff1c5, 6ff904).
    double base = variant == 0 ? (double)f(world + PRICE_A) : (double)f(world + PRICE_B);
    if (!(heads < cap) || !(money(world + (variant == 0 ? DOLLARS : RUBLES)) > base)) return "no room or money";
    if (variant == 1 && d(world + CAMPAIGN) != -1) return "a campaign step";
    if (!*(BYTE*)(world + (variant == 0 ? ALLOW_A : ALLOW_B))) return "not allowed";
    bool linked = true;
    // A partner is linked to the adult before it: the last citizen's +c8, the previous one's +b0.
    auto link = [&] {
        U64 vb = Q(world + CITIZENS), ve = Q(world + CITIZENS + 8);
        if (ve < vb + 16 || (ve - vb) % 8) { linked = false; return; }
        U64 last = Q(ve - 8), prev = Q(ve - 16);
        *(U64*)(last + 0xC8) = prev;
        *(U64*)(prev + 0xB0) = last;
    };
    auto room = [&] { int c = 0; return Capacity(b, c) && heads < c; };
    float k = *(float*)(g_exeBase + EXPERT_FACTOR_RVA);
    if (variant == 0) {
        for (int i = 0; i < 10; ++i) {
            float price = f(world + PRICE_A) * f(world + FACTOR_A);
            if (!(money(world + DOLLARS) > (double)price) || !room()) continue;
            ++heads;
            ++d(world + COUNT_A);
            f(world + COST_A) = f(world + UNIT_A) * f(world + FACTOR_A) + f(world + COST_A);
            ++d(stats + 0x258);
            f(stats + 0x240) = f(world + UNIT_A) * f(world + FACTOR_A) + f(stats + 0x240);
            float cost = f(world + PRICE_A) * f(world + FACTOR_A);
            money(world + DOLLARS) = money(world + DOLLARS) - (double)cost;
            *paid += (double)cost;
            create(world, b, 0, 0, 1, 0);
            ++*adults;
            if (Determinism_CommandRand(PARTNER_DRAW_SITE) % 3 == 0) {
                create(world, b, 1, 0, 1, 0);
                link();
                ++*partners;
            }
            if (!linked) break;
        }
    } else {
        int rounds = variant == 1 ? 10 : 5;
        float lo = *(float*)(g_exeBase + EDUCATION_LOW_RVA), hi = *(float*)(g_exeBase + EDUCATION_HIGH_RVA);
        for (int i = 0; i < rounds; ++i) {
            double m = money(world + RUBLES);
            float price = variant == 1 ? f(world + FACTOR_B) * f(world + PRICE_B) : (f(world + PRICE_B) * k) * f(world + FACTOR_B);
            if (!(m > (double)price) || !room()) continue;
            money(world + RUBLES) = m - (double)price;
            *paid += (double)price;
            create(world, b, 0, 1, 0, 0);
            ++*adults;
            if (variant == 2) {
                U64 vb = Q(world + CITIZENS), ve = Q(world + CITIZENS + 8);
                if (ve < vb + 8 || (ve - vb) % 8) { linked = false; break; }
                f(Q(ve - 8) + 0xA8) = Determinism_CommandRandFloat(EDUCATION_DRAW_SITE, lo, hi);
            }
            ++d(world + COUNT_B);
            if (variant == 1) {
                f(world + COST_B) = f(world + FACTOR_B) * f(world + UNIT_B) + f(world + COST_B);
                ++d(stats + 0x254);
                f(stats + 0x244) = f(world + FACTOR_B) * f(world + UNIT_B) + f(stats + 0x244);
            } else {
                f(world + COST_B) = (f(world + UNIT_B) * k) * f(world + FACTOR_B) + f(world + COST_B);
                ++d(stats + 0x254);
                f(stats + 0x244) = (f(world + UNIT_B) * k) * f(world + FACTOR_B) + f(stats + 0x244);
            }
            ++heads;
            if (Determinism_CommandRand(PARTNER_DRAW_SITE) % 2 == 0) {
                create(world, b, 1, 1, 0, 0);
                link();
                ++*partners;
            }
            if (!linked) break;
        }
    }
    if (!linked) return "the citizen list";
    return nullptr;
}

bool ImmigrantInvite_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(InviteMsg)) return false;
    InviteMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.variant > 2) return false;
    static const char* const kKinds[3] = { "third world", "Soviet bloc", "experts" };
    U64 world = (U64)(g_exeBase + WORLD_RVA);
    int adults = 0, partners = 0;
    double paid = 0;
    const char* why = nullptr;
    __try {
        U64 b = Ref_BuildingAt(m.building);
        why = b ? Replay(world, b, m.variant, &adults, &partners, &paid) : "the building is not here";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) {
        Logf("republicmp  INVITE: %s immigrants to building #%d: %s - %d invited", kKinds[m.variant], m.building.place, why, adults);
        return adults > 0;
    }
    Logf("republicmp  INVITE: %s immigrants to building #%d: %d adult(s), %d partner(s), %.2f paid (heads at the press %d)",
         kKinds[m.variant], m.building.place, adults, partners, paid, m.heads);
    return true;
}
