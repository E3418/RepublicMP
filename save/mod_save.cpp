// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// See mod_save.h.
#include "mod_save.h"
#include "../core/version.h"
#include <intrin.h> // __rdtsc
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define SAVE_RVA 0x42CC70       // the save routine (world)
#define SAVE_NAME_OFFSET 0x9F8  // world: char[]: "save/<name>"
#define LOAD_NAME_RVA 0x9962B0  // char[]: "save\<name>", the save loaded last
#define TERRAIN_FLAG_RVA 0x995298 // byte: != 0, the load makes a built-in terrain instead
#define NAME_BYTES 120
#define MAX_SECTIONS 16

// mov r11,rsp / push rdi / push r12..r15 / sub rsp,0A40h
static const unsigned char kSaveExpect[19] = {
    0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x0A, 0x00, 0x00,
};

struct Section {
    const char* name;
    ModSaveWriteFn write;
    ModSaveReadFn read;
};
struct Entry {
    std::string section, key, value;
};

typedef void (*SaveFn)(unsigned char* world);
static SaveFn o_save = nullptr;
static Section g_sections[MAX_SECTIONS];
static int g_sectionCount = 0;
static std::string* g_out = nullptr;             // the text being written
static const std::vector<Entry>* g_in = nullptr; // the file being read
static const char* g_inSection = nullptr;
static char g_loadName[NAME_BYTES];             // the save the load in progress reads ("": none)
static char g_lastLoadName[NAME_BYTES];         // the same, kept after the load
static unsigned long long g_worldSeed = 0;       // 0: no world loaded yet
static unsigned long long g_loadSeed = 0;        // the world seed in the save being loaded (0: none)

void ModSave_Add(const char* section, ModSaveWriteFn write, ModSaveReadFn read)
{
    if (g_sectionCount < MAX_SECTIONS) g_sections[g_sectionCount++] = { section, write, read };
}

// "<game folder>\media_soviet\<name>" with the name's '/' turned into '\'.
static bool SaveFolder(const char* name, char* out, size_t outSize)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(exe, '\\') : nullptr;
    if (!slash || !name[0]) return false;
    *slash = 0;
    if (_snprintf_s(out, outSize, _TRUNCATE, "%s\\media_soviet\\%s", exe, name) < 0) return false;
    for (char* c = out; *c; ++c)
        if (*c == '/') *c = '\\';
    return true;
}

static bool CopyName(const char* from, char* name)
{
    __try {
        memcpy(name, from, NAME_BYTES - 1);
        name[NAME_BYTES - 1] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return name[0] != 0;
}

void ModSave_Put(const char* key, const char* value)
{
    if (!g_out || !key[0] || strpbrk(key, "=[\r\n")) return;
    std::string v = value;
    for (char& ch : v)
        if (ch == '\r' || ch == '\n') ch = ' ';
    *g_out += key;
    *g_out += '=';
    *g_out += v;
    *g_out += "\r\n";
}

void ModSave_PutInt(const char* key, long long value)
{
    char text[32];
    sprintf_s(text, "%lld", value);
    ModSave_Put(key, text);
}

void ModSave_PutFloats(const char* key, const float* values, int count)
{
    std::string text;
    char one[32];
    for (int i = 0; i < count; i++) {
        sprintf_s(one, "%s%.9g", i ? "," : "", values[i]);
        text += one;
    }
    ModSave_Put(key, text.c_str());
}

// After the game's files: the save's folder exists and holds this save. The
// text goes to a .new file first, so a failed write leaves the old one.
static void WriteFile(const char* name)
{
    char folder[MAX_PATH], path[MAX_PATH], temp[MAX_PATH];
    if (!SaveFolder(name, folder, sizeof(folder))) return;
    // A save into a folder that is not there wrote nothing either (the
    // game's exit save "saved_last" without its folder).
    DWORD attributes = GetFileAttributesA(folder);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\" MOD_SAVE_FILE, folder);
    _snprintf_s(temp, sizeof(temp), _TRUNCATE, "%s\\" MOD_SAVE_FILE ".new", folder);
    std::string text = "; RepublicMP's data for this save (save/mod_save.h)\r\n[republicmp]\r\n";
    g_out = &text;
    ModSave_Put("version", REPUBLICMP_VERSION);
    ModSave_PutInt("protocol", REPUBLICMP_PROTOCOL);
    if (g_worldSeed) {
        char seed[24];
        sprintf_s(seed, "%016llx", g_worldSeed);
        ModSave_Put("world_seed", seed);
    }
    for (int i = 0; i < g_sectionCount; i++) {
        if (!g_sections[i].write) continue;
        text += "[";
        text += g_sections[i].name;
        text += "]\r\n";
        g_sections[i].write();
    }
    g_out = nullptr;
    FILE* f = nullptr;
    bool ok = fopen_s(&f, temp, "wb") == 0 && f;
    if (ok) {
        ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = fclose(f) == 0 && ok;
    }
    if (!ok || !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(temp);
        Logf("republicmp  MOD SAVE: [%s] - %s could not be written", name, MOD_SAVE_FILE);
        return;
    }
    Logf("republicmp  MOD SAVE: [%s] %s written (%zu bytes)", name, MOD_SAVE_FILE, text.size());
}

static void HookedSave(unsigned char* world)
{
    o_save(world);
    char name[NAME_BYTES];
    if (CopyName((const char*)world + SAVE_NAME_OFFSET, name)) WriteFile(name);
}

static void Trim(std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool ReadFile(const char* name, std::vector<Entry>& out)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!SaveFolder(name, folder, sizeof(folder))) return false;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\" MOD_SAVE_FILE, folder);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    // Whole, so a line has no length limit (a module's history is one line a record).
    std::string all;
    char chunk[4096];
    for (size_t got; (got = fread(chunk, 1, sizeof(chunk), f)) > 0;) all.append(chunk, got);
    fclose(f);
    std::string section;
    for (size_t at = 0, next; at < all.size(); at = next) {
        size_t newline = all.find('\n', at);
        next = newline == std::string::npos ? all.size() : newline + 1;
        std::string l = all.substr(at, next - at);
        Trim(l);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            size_t end = l.find(']');
            section = end == std::string::npos ? std::string() : l.substr(1, end - 1);
            continue;
        }
        size_t eq = l.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        Entry e = { section, l.substr(0, eq), l.substr(eq + 1) };
        Trim(e.key);
        Trim(e.value);
        if (!e.key.empty()) out.push_back(e);
    }
    return true;
}

bool ModSave_Get(const char* key, char* out, size_t outSize)
{
    if (!g_in || !g_inSection) return false;
    for (const Entry& e : *g_in)
        if (e.section == g_inSection && e.key == key) {
            strncpy_s(out, outSize, e.value.c_str(), _TRUNCATE);
            return true;
        }
    return false;
}

bool ModSave_GetInt(const char* key, long long* out)
{
    char text[32], *end = nullptr;
    if (!ModSave_Get(key, text, sizeof(text))) return false;
    long long v = strtoll(text, &end, 10);
    if (end == text) return false;
    *out = v;
    return true;
}

bool ModSave_GetFloats(const char* key, float* out, int count)
{
    char text[512];
    if (!ModSave_Get(key, text, sizeof(text))) return false;
    const char* p = text;
    for (int i = 0; i < count; i++) {
        char* end = nullptr;
        float v = strtof(p, &end);
        if (end == p || !isfinite(v)) return false;
        out[i] = v;
        p = end;
        if (i + 1 < count) {
            if (*p != ',') return false;
            ++p;
        }
    }
    return true;
}

void ModSave_ForEach(void (*fn)(const char* key, const char* value, void* ctx), void* ctx)
{
    if (!g_in || !g_inSection) return;
    for (const Entry& e : *g_in)
        if (e.section == g_inSection) fn(e.key.c_str(), e.value.c_str(), ctx);
}

static bool TerrainLoad()
{
    __try {
        return *(const char*)(g_exeBase + TERRAIN_FLAG_RVA) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void ModSave_BeforeLoad()
{
    // A new game on a built-in terrain leaves the last save's name in place.
    if (TerrainLoad() || !CopyName((const char*)(g_exeBase + LOAD_NAME_RVA), g_loadName)) g_loadName[0] = 0;
    memcpy(g_lastLoadName, g_loadName, sizeof(g_lastLoadName));
    // Read now as well: the load itself draws random numbers.
    g_loadSeed = 0;
    std::vector<Entry> entries;
    if (g_loadName[0] && ReadFile(g_loadName, entries))
        for (const Entry& e : entries)
            if (e.section == "republicmp" && e.key == "world_seed") g_loadSeed = strtoull(e.value.c_str(), nullptr, 16);
}

unsigned long long ModSave_LoadSeed() { return g_loadSeed; }

unsigned long long ModSave_WorldSeed() { return g_worldSeed; }

// A world without one gets a new seed: not from rand or C3DRandom, which the
// determinism layer serves from the session's draws.
static unsigned long long NewSeed()
{
    unsigned long long x = __rdtsc() ^ (GetTickCount64() << 20) ^ ((unsigned long long)GetCurrentProcessId() << 40);
    do { // splitmix64
        x += 0x9E3779B97F4A7C15ull;
        unsigned long long z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        x = z ^ (z >> 31);
    } while (!x);
    return x;
}

void ModSave_AfterLoad()
{
    const char* name = g_loadName;
    std::vector<Entry> entries;
    bool file = name[0] && ReadFile(name, entries);
    g_in = &entries;
    if (file) {
        char version[32] = "?";
        g_inSection = "republicmp";
        ModSave_Get("version", version, sizeof(version));
        Logf("republicmp  MOD SAVE: [%s] %s read (written by %s, %zu keys)", name, MOD_SAVE_FILE, version,
             entries.size());
    }
    // Before the sections: a mode's read callback may build on it.
    char seed[24] = "";
    g_inSection = "republicmp";
    unsigned long long saved = ModSave_Get("world_seed", seed, sizeof(seed)) ? strtoull(seed, nullptr, 16) : 0;
    g_worldSeed = saved ? saved : NewSeed();
    Logf("republicmp  MOD SAVE: world seed %016llx (%s)", g_worldSeed, saved ? "from the save" : "new");
    for (int i = 0; i < g_sectionCount; i++) {
        if (!g_sections[i].read) continue;
        bool found = false;
        for (const Entry& e : entries)
            if (e.section == g_sections[i].name) found = true;
        g_inSection = g_sections[i].name;
        g_sections[i].read(found);
    }
    g_in = nullptr;
    g_inSection = nullptr;
    g_loadName[0] = 0;
    g_loadSeed = 0;
}

void ModSave_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

bool ModSave_LoadedFolder(char* out, size_t outSize)
{
    return g_lastLoadName[0] && SaveFolder(g_lastLoadName, out, outSize);
}

bool ModSave_Install()
{
    return InstallInlineHook(g_exeBase + SAVE_RVA, (void*)&HookedSave, (void**)&o_save, kSaveExpect,
                             sizeof(kSaveExpect), "republicmp save");
}
