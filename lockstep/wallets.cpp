// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// See wallets.h.
#include "wallets.h"
#include "commands.h"
#include "../game/world.h" // GAME_BUILDING_POS_OFFSET
#include "../save/mod_save.h"
#include "../session/save_sync.h"

#define WORLD_RVA 0x9D4F10
#define WORLD_DOLLARS 0x580
#define WORLD_RUBLES 0x588
#define WORLD_UNLIMITED 0x578 // byte: payers skip the charge, Late pins both balances to 1e8
// std::vector of the loans, 0x28 bytes each. +0x0C is $LoanSubType: the game's
// loan windows do not read it and Loan_Borrow leaves it 0, so it holds the
// owner's half + 1 (0: taken before the accounts, no owner).
#define WORLD_LOANS 0x10BD0
#define LOAN_SIZE 0x28
#define LOAN_OWNER 0x0C
#define MAX_SAVED_LOANS 512

#define UNOWNED_LOG_LINES 10
#define SCOPE_NONE (-1)
#define SCOPE_KEEP (-2) // Begin changed nothing, End has nothing to do

struct Account {
    double rub, usd;
};

enum UseMode { USE_VIEW, USE_COMMAND, USE_SIM };

static volatile bool g_on = false; // a Republics session is running
static bool g_kept = false;        // g_accounts hold this world's accounts (also after the session ended)
static Account g_accounts[2];
static UseMode g_use = USE_VIEW;
static int g_commandHalf = 0;
static Account g_simBase;    // what the balances held when the tick's code started
static Account g_unowned;    // the ticks' money split so far, this session
static int g_unownedLogged = 0;
static bool g_loadedFound = false; // the save being loaded had the accounts
static Account g_loaded[2];
static char g_loadedLoans[MAX_SAVED_LOANS + 1] = ""; // its loans' owners, one digit each
static int g_scope = SCOPE_NONE;   // USE_SIM: the half whose account the balances hold now
static size_t g_commandLoans = 0;  // the loans when the command started
static WalletsHalfRule g_halfRule = nullptr;

static double& Rubles() { return *(double*)(g_exeBase + WORLD_RVA + WORLD_RUBLES); }
static double& Dollars() { return *(double*)(g_exeBase + WORLD_RVA + WORLD_DOLLARS); }

static int HalfOfPlayer(int player)
{
    int hostHalf = SaveSync_GetHostSide();
    return player == 0 ? hostHalf : 1 - hostHalf;
}

static int LocalHalf()
{
    return HalfOfPlayer(SaveSync_IsHostRole() ? 0 : 1);
}

static void ShowLocal()
{
    const Account& a = g_accounts[LocalHalf()];
    Rubles() = a.rub;
    Dollars() = a.usd;
}

// What the tick's code changed since g_simBase goes to both, half each.
static void SettleSim()
{
    double dRub = Rubles() - g_simBase.rub, dUsd = Dollars() - g_simBase.usd;
    if (*(unsigned char*)(g_exeBase + WORLD_RVA + WORLD_UNLIMITED)) {
        g_accounts[0].rub = g_accounts[1].rub = Rubles();
        g_accounts[0].usd = g_accounts[1].usd = Dollars();
    } else if (dRub != 0.0 || dUsd != 0.0) {
        g_accounts[0].rub += dRub / 2;
        g_accounts[1].rub += dRub / 2;
        g_accounts[0].usd += dUsd / 2;
        g_accounts[1].usd += dUsd / 2;
        g_unowned.rub += dRub;
        g_unowned.usd += dUsd;
        if (g_unownedLogged < UNOWNED_LOG_LINES) {
            ++g_unownedLogged;
            Logf("republicmp  WALLETS: a tick's money without an owner: %+.4f rubles %+.4f dollars - split in two", dRub, dUsd);
        }
    }
    g_simBase = { Rubles(), Dollars() };
}

// What the balances hold now goes back where it belongs.
static void CommitScope()
{
    if (g_scope < 0) SettleSim();
    else g_accounts[g_scope] = { Rubles(), Dollars() };
}

static void LoadScope(int scope)
{
    g_scope = scope;
    if (scope < 0) {
        g_simBase.rub = g_accounts[0].rub + g_accounts[1].rub;
        g_simBase.usd = g_accounts[0].usd + g_accounts[1].usd;
        Rubles() = g_simBase.rub;
        Dollars() = g_simBase.usd;
    } else {
        Rubles() = g_accounts[scope].rub;
        Dollars() = g_accounts[scope].usd;
    }
}

void Wallets_SimBegin()
{
    if (!g_on || g_use != USE_VIEW) return;
    g_use = USE_SIM;
    LoadScope(SCOPE_NONE);
}

void Wallets_SimEnd()
{
    if (g_use != USE_SIM) return; // not g_on: a session that ended under the tick still gets the balances back
    CommitScope();
    g_scope = SCOPE_NONE;
    g_use = USE_VIEW;
    ShowLocal();
}

// half -1 (the owner is not known) leaves the money where it is. Outside a tick
// nothing changes either: in a command the money is the player's who made it.
static int ScopeBegin(int half)
{
    if (g_use != USE_SIM || half < 0 || half == g_scope) return SCOPE_KEEP;
    int previous = g_scope;
    CommitScope();
    LoadScope(half);
    return previous;
}

static void ScopeEnd(int previous)
{
    if (previous == SCOPE_KEEP || g_use != USE_SIM) return;
    CommitScope();
    LoadScope(previous);
}

void Wallets_SetHalfRule(WalletsHalfRule rule)
{
    g_halfRule = rule;
}

static int HalfOfBuilding(unsigned long long building)
{
    if (!g_halfRule || !building) return -1;
    float pos[3];
    __try {
        memcpy(pos, (const void*)(building + GAME_BUILDING_POS_OFFSET), sizeof(pos));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return g_halfRule(pos);
}

static int HalfOfVehicle(unsigned long long vehicle)
{
    if (!g_halfRule) return -1;
    unsigned long long building;
    float pos[3];
    if (!Commands_VehicleSide(vehicle, &building, pos)) return -1;
    return building ? HalfOfBuilding(building) : g_halfRule(pos);
}

int Wallets_BuildingBegin(unsigned long long building)
{
    return g_use == USE_SIM ? ScopeBegin(HalfOfBuilding(building)) : SCOPE_KEEP;
}

void Wallets_BuildingEnd(int previous)
{
    ScopeEnd(previous);
}

static size_t LoanCount()
{
    BYTE* vec = g_exeBase + WORLD_RVA + WORLD_LOANS;
    BYTE* begin = *(BYTE**)vec;
    BYTE* end = *(BYTE**)(vec + 8);
    return begin && end > begin ? (size_t)(end - begin) / LOAN_SIZE : 0;
}

static int* LoanOwner(size_t index)
{
    return (int*)(*(BYTE**)(g_exeBase + WORLD_RVA + WORLD_LOANS) + index * LOAN_SIZE + LOAN_OWNER);
}

void Wallets_CommandBegin(int player)
{
    if (!g_on || g_use != USE_VIEW || player < 0 || player > 1) return;
    g_commandHalf = HalfOfPlayer(player);
    g_commandLoans = LoanCount();
    Rubles() = g_accounts[g_commandHalf].rub;
    Dollars() = g_accounts[g_commandHalf].usd;
    g_use = USE_COMMAND;
}

void Wallets_CommandEnd()
{
    if (g_use != USE_COMMAND) return;
    g_accounts[g_commandHalf] = { Rubles(), Dollars() };
    // A loan this command took is this player's.
    for (size_t i = g_commandLoans, n = LoanCount(); i < n; i++) *LoanOwner(i) = g_commandHalf + 1;
    g_use = USE_VIEW;
    ShowLocal();
}

bool Wallets_On()
{
    return g_on;
}

bool Wallets_Read(double* rub0, double* usd0, double* rub1, double* usd1)
{
    if (!g_on) return false;
    *rub0 = g_accounts[0].rub;
    *usd0 = g_accounts[0].usd;
    *rub1 = g_accounts[1].rub;
    *usd1 = g_accounts[1].usd;
    return true;
}

void Wallets_LogState()
{
    if (!g_on) return;
    Logf("republicmp  WALLETS: half 0 %.2f rubles %.2f dollars, half 1 %.2f rubles %.2f dollars; ticks split %+.2f rubles %+.2f dollars",
         g_accounts[0].rub, g_accounts[0].usd, g_accounts[1].rub, g_accounts[1].usd, g_unowned.rub, g_unowned.usd);
}

void Wallets_AfterLoad(bool session)
{
    bool found = g_loadedFound;
    g_loadedFound = false;
    g_use = USE_VIEW;
    g_scope = SCOPE_NONE;
    if (!session || SaveSync_GetGameMode() != SESSION_MODE_REPUBLICS) {
        g_on = false;
        g_kept = false;
        return;
    }
    if (found) {
        g_accounts[0] = g_loaded[0];
        g_accounts[1] = g_loaded[1];
        size_t count = LoanCount();
        for (size_t i = 0; i < count && g_loadedLoans[i]; i++) *LoanOwner(i) = g_loadedLoans[i] - '0';
    } else {
        // Half 0 is the East. Both PCs loaded the same save: the same numbers.
        double rub = Rubles(), usd = Dollars();
        g_accounts[0] = { rub, usd };
        if (SaveSync_GetGameVariant() == SESSION_REPUBLICS_EQUAL) g_accounts[1] = { rub, usd };
        else g_accounts[1] = { usd, rub };
    }
    g_unowned = { 0.0, 0.0 };
    g_unownedLogged = 0;
    g_kept = true;
    g_on = true;
    ShowLocal();
    Logf("republicmp  WALLETS: %s", found ? "the accounts come from the save" : "the accounts start from the save's balances");
    Wallets_LogState();
}

void Wallets_SessionEnd()
{
    if (!g_on) return;
    Wallets_LogState();
    g_on = false;
}

// A double's exact bits: a re-sync must bring back the same number.
static void PutMoney(const char* key, double value)
{
    unsigned long long bits;
    memcpy(&bits, &value, sizeof(bits));
    char text[24];
    sprintf_s(text, "%016llX", bits);
    ModSave_Put(key, text);
}

static bool GetMoney(const char* key, double* value)
{
    char text[24];
    if (!ModSave_Get(key, text, sizeof(text))) return false;
    char* end = nullptr;
    unsigned long long bits = _strtoui64(text, &end, 16);
    if (end == text || *end) return false;
    memcpy(value, &bits, sizeof(bits));
    return true;
}

static void WriteSection()
{
    if (!g_kept) return;
    if (g_use == USE_SIM) {
        CommitScope();
        LoadScope(g_scope);
    }
    // The host plays on alone: the balances are its account.
    if (!g_on) g_accounts[LocalHalf()] = { Rubles(), Dollars() };
    PutMoney("half0_rub", g_accounts[0].rub);
    PutMoney("half0_usd", g_accounts[0].usd);
    PutMoney("half1_rub", g_accounts[1].rub);
    PutMoney("half1_usd", g_accounts[1].usd);
    char owners[MAX_SAVED_LOANS + 1];
    size_t count = LoanCount();
    if (count > MAX_SAVED_LOANS) count = MAX_SAVED_LOANS;
    for (size_t i = 0; i < count; i++) {
        int owner = *LoanOwner(i);
        owners[i] = owner == 1 || owner == 2 ? (char)('0' + owner) : '0';
    }
    owners[count] = 0;
    ModSave_Put("loans", owners);
}

static void ReadSection(bool found)
{
    g_loadedFound = found && GetMoney("half0_rub", &g_loaded[0].rub) && GetMoney("half0_usd", &g_loaded[0].usd) &&
                    GetMoney("half1_rub", &g_loaded[1].rub) && GetMoney("half1_usd", &g_loaded[1].usd);
    g_loadedLoans[0] = 0;
    if (g_loadedFound && !ModSave_Get("loans", g_loadedLoans, sizeof(g_loadedLoans))) g_loadedLoans[0] = 0;
}

void Wallets_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    ModSave_Add("wallets", WriteSection, ReadSection);
}

// ---------------------------------------------------------------------------
// The payers
// ---------------------------------------------------------------------------
//
// Every function of the tick that writes the balances (docs/findings.md,
// "Money"), by what it works for. Each gets the same arguments on: they are
// integers and pointers in the registers, a float only on the stack, so eight
// integer arguments carry them all (reading past a caller's last one is harmless).

typedef unsigned long long U64;
typedef U64 (*PayerFn)(U64, U64, U64, U64, U64, U64, U64, U64);

#define PAYER(name, half)                                                          \
    static PayerFn o_##name;                                                       \
    static U64 Det_##name(U64 a, U64 b, U64 c, U64 d, U64 e, U64 f, U64 g, U64 h)  \
    {                                                                              \
        int previous = g_use == USE_SIM ? ScopeBegin(half) : SCOPE_KEEP;           \
        U64 r = o_##name(a, b, c, d, e, f, g, h);                                  \
        ScopeEnd(previous);                                                        \
        return r;                                                                  \
    }

// (world, building): the building pass, by the building's kind.
PAYER(Collapse, HalfOfBuilding(b))     // 143710: a soft collapse
PAYER(Pass171E10, HalfOfBuilding(b))
PAYER(Pass173320, HalfOfBuilding(b))
PAYER(Pass183580, HalfOfBuilding(b))
PAYER(Pass183D30, HalfOfBuilding(b))
PAYER(Pass184930, HalfOfBuilding(b))
PAYER(Pass1856F0, HalfOfBuilding(b))
PAYER(Pass1B7D50, HalfOfBuilding(b))
PAYER(Pass1B9A40, HalfOfBuilding(b))
PAYER(Pass1D2DD0, HalfOfBuilding(b))
// (vehicle, ...): its update, with what its passengers pay inside.
PAYER(VehicleUpdate, HalfOfVehicle(a))
// (world, vehicle, ...): its removal and what that refunds.
PAYER(VehicleRemove, HalfOfVehicle(b))
// (queue, vehicle): the vehicle job queue's handlers (6141d0).
PAYER(Job60B8E0, HalfOfVehicle(b))
PAYER(Job60C450, HalfOfVehicle(b))
PAYER(Job60E4D0, HalfOfVehicle(b))
PAYER(Job60E940, HalfOfVehicle(b))
PAYER(Job60F700, HalfOfVehicle(b))

// 4b9490(world, loan), one loan's day. Its first instructions hold a short jump
// inside the 14 bytes a hook needs, so its one call (in 4b9660) comes here.
#define LOAN_DAY_RVA 0x4B9490
#define LOAN_DAY_CALL 0x4B96D3
typedef void (*LoanDayFn)(U64 world, U64 loan);

static void DetLoanDay(U64 world, U64 loan)
{
    int owner = *(int*)(loan + LOAN_OWNER);
    int previous = g_use == USE_SIM ? ScopeBegin(owner == 1 || owner == 2 ? owner - 1 : -1) : SCOPE_KEEP;
    ((LoanDayFn)(g_exeBase + LOAN_DAY_RVA))(world, loan);
    ScopeEnd(previous);
}

static bool RelayLoanDay()
{
    BYTE* call = g_exeBase + LOAN_DAY_CALL;
    int rel = 0;
    memcpy(&rel, call + 1, sizeof(rel));
    if (*call != 0xE8 || LOAN_DAY_CALL + 5 + rel != LOAN_DAY_RVA) return false;
    BYTE* stub = AllocNear(call, 14);
    if (!stub) return false;
    stub[0] = 0xFF;
    stub[1] = 0x25;
    memset(stub + 2, 0, 4);
    void* target = (void*)&DetLoanDay;
    memcpy(stub + 6, &target, sizeof(target));
    rel = (int)(stub - (call + 5));
    DWORD old;
    if (!VirtualProtect(call + 1, sizeof(rel), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(call + 1, &rel, sizeof(rel));
    VirtualProtect(call + 1, sizeof(rel), old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

static const BYTE kCollapse[] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
static const BYTE kPass171E10[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x68, 0x88 };
static const BYTE kArgs2[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57 };
static const BYTE kPass183580[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
static const BYTE kPass183D30[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xB8 };
static const BYTE kPass1856F0[] = { 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00 };
static const BYTE kPass1B7D50[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57 };
static const BYTE kPass1D2DD0[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
static const BYTE kVehicleUpdate[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10 };
static const BYTE kVehicleRemove[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08 };
static const BYTE kJob60E4D0[] = { 0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xC8 };

bool Wallets_Install()
{
    struct Site {
        unsigned rva;
        void* detour;
        PayerFn* original;
        const BYTE* expect;
        size_t stolen;
        const char* label;
    };
#define SITE(rva, name, expect) { rva, (void*)&Det_##name, &o_##name, expect, sizeof(expect), "republicmp wallets: " #name }
#define SITE_ARGS2(rva, name) { rva, (void*)&Det_##name, &o_##name, kArgs2, sizeof(kArgs2), "republicmp wallets: " #name }
    const Site sites[] = {
        SITE(0x143710, Collapse, kCollapse),
        SITE(0x171E10, Pass171E10, kPass171E10),
        SITE_ARGS2(0x173320, Pass173320),
        SITE(0x183580, Pass183580, kPass183580),
        SITE(0x183D30, Pass183D30, kPass183D30),
        SITE(0x184930, Pass184930, kPass183580), // the same first instructions
        SITE(0x1856F0, Pass1856F0, kPass1856F0),
        SITE(0x1B7D50, Pass1B7D50, kPass1B7D50),
        SITE_ARGS2(0x1B9A40, Pass1B9A40),
        SITE(0x1D2DD0, Pass1D2DD0, kPass1D2DD0),
        SITE(0x68B330, VehicleUpdate, kVehicleUpdate),
        SITE(0x3E0730, VehicleRemove, kVehicleRemove),
        SITE_ARGS2(0x60B8E0, Job60B8E0),
        SITE_ARGS2(0x60C450, Job60C450),
        SITE(0x60E4D0, Job60E4D0, kJob60E4D0),
        SITE_ARGS2(0x60E940, Job60E940),
        SITE_ARGS2(0x60F700, Job60F700),
    };
#undef SITE
#undef SITE_ARGS2
    bool ok = true;
    for (const Site& s : sites)
        ok &= InstallInlineHook(g_exeBase + s.rva, s.detour, (void**)s.original, s.expect, s.stolen, s.label);
    bool loans = RelayLoanDay();
    if (!loans) Logf("republicmp  WALLETS: the loans' daily payment differs - it stays split in two");
    return ok && loans;
}
