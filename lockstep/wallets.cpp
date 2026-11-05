// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// See wallets.h.
#include "wallets.h"
#include "../save/mod_save.h"
#include "../session/save_sync.h"

#define WORLD_RVA 0x9D4F10
#define WORLD_DOLLARS 0x580
#define WORLD_RUBLES 0x588
#define WORLD_UNLIMITED 0x578 // byte: payers skip the charge, Late pins both balances to 1e8

#define UNOWNED_LOG_LINES 10

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

void Wallets_SimBegin()
{
    if (!g_on || g_use != USE_VIEW) return;
    g_simBase.rub = g_accounts[0].rub + g_accounts[1].rub;
    g_simBase.usd = g_accounts[0].usd + g_accounts[1].usd;
    Rubles() = g_simBase.rub;
    Dollars() = g_simBase.usd;
    g_use = USE_SIM;
}

void Wallets_SimEnd()
{
    if (g_use != USE_SIM) return; // not g_on: a session that ended under the tick still gets the balances back
    SettleSim();
    g_use = USE_VIEW;
    ShowLocal();
}

void Wallets_CommandBegin(int player)
{
    if (!g_on || g_use != USE_VIEW || player < 0 || player > 1) return;
    g_commandHalf = HalfOfPlayer(player);
    Rubles() = g_accounts[g_commandHalf].rub;
    Dollars() = g_accounts[g_commandHalf].usd;
    g_use = USE_COMMAND;
}

void Wallets_CommandEnd()
{
    if (g_use != USE_COMMAND) return;
    g_accounts[g_commandHalf] = { Rubles(), Dollars() };
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
    if (!session || SaveSync_GetGameMode() != SESSION_MODE_REPUBLICS) {
        g_on = false;
        g_kept = false;
        return;
    }
    if (found) {
        g_accounts[0] = g_loaded[0];
        g_accounts[1] = g_loaded[1];
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
    if (g_use == USE_SIM) SettleSim();
    // The host plays on alone: the balances are its account.
    if (!g_on) g_accounts[LocalHalf()] = { Rubles(), Dollars() };
    PutMoney("half0_rub", g_accounts[0].rub);
    PutMoney("half0_usd", g_accounts[0].usd);
    PutMoney("half1_rub", g_accounts[1].rub);
    PutMoney("half1_usd", g_accounts[1].usd);
}

static void ReadSection(bool found)
{
    g_loadedFound = found && GetMoney("half0_rub", &g_loaded[0].rub) && GetMoney("half0_usd", &g_loaded[0].usd) &&
                    GetMoney("half1_rub", &g_loaded[1].rub) && GetMoney("half1_usd", &g_loaded[1].usd);
}

void Wallets_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    ModSave_Add("wallets", WriteSection, ReadSection);
}
