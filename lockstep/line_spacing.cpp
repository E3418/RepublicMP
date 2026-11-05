// SPDX-License-Identifier: GPL-3.0-only
// See line_spacing.h. Follows SoylentGreed's line spacing route (RepublicLAN,
// read against the disassembly of 1.1.1.9).
#include "line_spacing.h"
#include "commands.h"
#include "world_refs.h"
#include <initializer_list>
#include <limits.h>
#include <string.h>

#define PRESS_RVA 0x7D3C71   // the button's press test, start of the checked block
#define BLOCK_END 0x7D3D06
#define BLOCK_FNV 0x2c8eb31afc780704ull
#define PATCH_RVA 0x7D3C8C   // lea rdx,[click sound] (7 bytes)
#define BACK_RVA 0x7D3C93
#define SKIP_RVA 0x7D3D06
#define SOUND_RVA 0x8F2EF0   // the click sound's name
#define GETTER_RVA 0x491500  // 491500(line): spacing on (caches the length +25c)
#define LINE_VEHICLES 0x240
#define VEHICLE_SPACING 0x658
#define MAX_VEHICLES 4096
#define MAGIC 0x3150534C

static const unsigned char kGetterEntry[16] = { 0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x89, 0x48, 0x02, 0x00, 0x00, 0x49, 0x2b, 0x88, 0x40, 0x02, 0x00 };

#pragma pack(push, 1)
struct SpacingMsg {
    unsigned magic;
    RouteRef line;
};
#pragma pack(pop)

typedef BYTE (*GetterFn)(U64 line);

void LineSpacing_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }

// The relay's question (the interface thread, the button pressed and hovered): 1 taken.
static BYTE __cdecl Capture(U64 line)
{
    if (!Commands_TakeUi()) return 0;
    SpacingMsg m = {};
    m.magic = MAGIC;
    bool ok = false;
    __try {
        ok = line && Ref_Line(line, m.line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (ok) Commands_Submit(LS_CMD_LINE_SPACING, &m, sizeof(m));
    else Logf("republicmp  LINE SPACING: the line is not in the lines manager - nothing is done");
    return 1;
}

bool LineSpacing_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SpacingMsg)) return false;
    SpacingMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC) return false;
    const char* why = nullptr;
    int now = -1;
    unsigned vehicles = 0;
    __try {
        U64 line = Ref_LineAt(m.line);
        if (!line) {
            why = "the line is gone";
        } else if (!Commands_LineInArea(line)) {
            why = "the line is outside the player's area";
        } else {
            // 7d3c9e..7d3d03 on this PC's line.
            BYTE on = ((GetterFn)(g_exeBase + GETTER_RVA))(line);
            now = on ? 0 : 1;
            U64 begin = Q(line + LINE_VEHICLES), end = Q(line + LINE_VEHICLES + 8);
            for (U64 at = begin; at < end && vehicles < MAX_VEHICLES; at += 8, ++vehicles) *(unsigned*)(Q(at) + VEHICLE_SPACING) = (unsigned)now;
            *(BYTE*)line = (BYTE)now;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault";
    }
    if (why) Logf("republicmp  LINE SPACING: %s", why);
    else Logf("republicmp  LINE SPACING: %s on a line (%u vehicle(s))", now ? "on" : "off", vehicles);
    return why == nullptr;
}

bool LineSpacing_Install()
{
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned a = PRESS_RVA; a < BLOCK_END; a++) h = (h ^ g_exeBase[a]) * 0x100000001b3ull;
    if (h != BLOCK_FNV || memcmp(g_exeBase + GETTER_RVA, kGetterEntry, 16)) {
        Logf("republicmp  LINE SPACING: the line window's button differs - it acts on the clicking PC only");
        return false;
    }
    BYTE* relay = AllocNear(g_exeBase, 4096);
    if (!relay) return false;
    // Entered by jmp with rsp 16-aligned (the window routine's body); the line in rsi.
    unsigned char code[128];
    size_t n = 0;
    auto put = [&](std::initializer_list<unsigned char> b) {
        for (unsigned char x : b) code[n++] = x;
    };
    auto abs = [&](U64 v) {
        memcpy(code + n, &v, 8);
        n += 8;
    };
    put({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // push rcx,rdx,r8..r11
    put({ 0x48, 0x83, 0xec, 0x20 });                                     // sub rsp,20
    put({ 0x48, 0x89, 0xf1 });                                           // mov rcx,rsi
    put({ 0x48, 0xb8 });                                                 // mov rax,Capture
    abs((U64)&Capture);
    put({ 0xff, 0xd0 });                                                 // call rax
    put({ 0x48, 0x83, 0xc4, 0x20, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59 }); // add rsp,20; pops
    put({ 0x84, 0xc0 });                                                 // test al,al
    size_t jnz = n;
    put({ 0x75, 0x00 });                                                 // jnz taken
    put({ 0x48, 0xba });                                                 // mov rdx,sound (the patched lea)
    abs((U64)(g_exeBase + SOUND_RVA));
    put({ 0xff, 0x25, 0, 0, 0, 0 });                                     // jmp [7d3c93]
    abs((U64)(g_exeBase + BACK_RVA));
    size_t taken = n;
    put({ 0xff, 0x25, 0, 0, 0, 0 });                                     // taken: jmp [7d3d06]
    abs((U64)(g_exeBase + SKIP_RVA));
    code[jnz + 1] = (unsigned char)(taken - (jnz + 2));
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(relay, code, n);
    VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), relay, n);
    BYTE* at = g_exeBase + PATCH_RVA;
    long long to = (long long)relay - (long long)(at + 5);
    if (to < INT_MIN || to > INT_MAX) return false;
    unsigned char jmp[7] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90 };
    int rel32 = (int)to;
    memcpy(jmp + 1, &rel32, 4);
    if (!VirtualProtect(at, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, jmp, 7);
    VirtualProtect(at, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 7);
    return true;
}
