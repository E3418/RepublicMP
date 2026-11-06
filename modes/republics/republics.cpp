// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP by E3418 and Sohn (juniorsohn); all authors in README.md.
// See republics.h.
#include "republics.h"
#include "../../api/republicmp_api.h"
#include "../../lockstep/lockstep_net.h"
#include "../../world/buildings.h"
#include "imgui.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

// The live C3D_TERRAIN's size in metres, x and z: the map is [-x/2, x/2] by
// [-z/2, z/2] around the origin, y up (C3DDLL64's GetTerrainSize, DLL 0xF8DF0,
// returns these two floats; 20000 x 20000 on the stock maps, from $WORLDSIZE
// in the map's script.ini).
#define TERRAIN_SIZE_X_OFFSET 0x87C
#define TERRAIN_SIZE_Z_OFFSET 0x880
// bool 410d50(world, const float* xyz): the point is outside the playable
// border - the map's $BORDER_POLYGON, else its BOUNDARY rectangle (placement
// refuses with it at 2caa4c; findings.md "Terrain").
#define OUT_OF_PLAYABLE_RVA 0x410D50
typedef bool (*OutOfPlayableFn)(void* world, const float* xyz);

// Points of the border drawn along the cut, and how often their heights
// are read again (terrain edits along it show within that time).
#define BORDER_POINTS 512
#define BORDER_REFRESH_MS 5000
#define REFUSED_NOTICE_MS 3000
// A customs house on the line is both players': its position this near the cut
// counts as either half (AreaRule). Nothing else near the line is shared.
#define LINE_CUSTOMS_M 50.0f
#define LINE_CUSTOMS_SAME_M 1.0f // the point is that house's position
// The playable area is measured on a grid of CUT_GRID x CUT_GRID cells
// (100 m on a stock map).
#define CUT_GRID 200

// The cut: a broken line across the map. Along the direction (-nz, nx) a point
// has its u, across it (nx, nz) its v; the line is v as a function of u, straight
// between knots `step` metres apart, so it never turns back on itself. Below it
// is half 0, above it half 1; the host plays the one it chose
// (SaveSync_GetHostSide). MakeCut below picks all of it.
#define MAX_KNOTS 24
struct Cut {
    float nx, nz, startU, step;
    int knots;
    float off[MAX_KNOTS];
};

// The line's v at u; slope: its dv/du there. Past either end it goes on level.
static float CutAt(const Cut& c, float u, float* slope = nullptr)
{
    float s = (u - c.startU) / c.step;
    int i = 0;
    float t = 0;
    if (s >= (float)(c.knots - 1)) {
        i = c.knots - 2;
        t = 1;
    } else if (s > 0) {
        i = (int)s;
        t = s - (float)i;
    }
    float dv = c.off[i + 1] - c.off[i];
    if (slope) *slope = dv / c.step;
    return c.off[i] + dv * t;
}

// Metres from the line, negative in half 0: across its piece at this point.
static float CutDistance(const Cut& c, const float p[3])
{
    float u = p[2] * c.nx - p[0] * c.nz, v = p[0] * c.nx + p[2] * c.nz, slope;
    float f = CutAt(c, u, &slope);
    return (v - f) / sqrtf(1.0f + slope * slope);
}

static Cut g_cut;
static bool g_cutReady = false; // main thread
static unsigned g_cutLoads = 0; // Determinism_WorldLoads() when it was measured
static float g_border[BORDER_POINTS][3];
static volatile int g_borderCount = 0; // 0 until drawn points are ready
// The same cut inside the playable border, in pieces of at most STRIP_STEP_M
// as the game cuts its own border line (40f3f0), for the game's strip.
#define STRIP_STEP_M 500.0f
#define STRIP_STRETCH 1.3f
#define STRIP_POINTS 128
static float g_strip[STRIP_POINTS][3];
static volatile int g_stripCount = 0;
// The game's border signs on each half, as it sets its own (410080): one a
// piece, SIGN_INSET_M inside, SIGN_LIFT_M above the ground - without its
// random +-20 m, so every PC shows them alike. East vs West: half 0 (the
// East) has the red star, half 1 the blue one; on equal terms both
// alternate. A sign shrinks away as the camera comes near (from
// SIGN_HIDE_FAR_M to SIGN_HIDE_NEAR_M of view depth): it is 120 m wide and
// the shader has no partial transparency (BumpBorderPixel clips at alpha 0.5).
#define SIGN_INSET_M 350.0f
#define SIGN_LIFT_M 5.0f
#define SIGN_HIDE_FAR_M 1200.0f
#define SIGN_HIDE_NEAR_M 700.0f
static float g_signs[2][STRIP_POINTS][3];
static volatile int g_signCount[2] = { 0, 0 };
static BorderSign g_drawnSigns[2 * STRIP_POINTS]; // render thread
static bool g_showSigns = true; // profile "overlay" "border_signs"
static ULONGLONG g_borderAt = 0;
static unsigned g_borderLoads = 0; // the world the drawn points were read on
static volatile ULONGLONG g_refusedAt = 0; // this PC's player was refused (main thread), shown by the overlay
static volatile ULONGLONG g_hostOnlyAt = 0; // ... for a global setting, the host's alone
static float g_inside[CUT_GRID * CUT_GRID];

static bool MapSize(float* w, float* h)
{
    unsigned char* t = (unsigned char*)TerrainHook_GetTerrain();
    if (!t) return false;
    __try {
        *w = *(float*)(t + TERRAIN_SIZE_X_OFFSET);
        *h = *(float*)(t + TERRAIN_SIZE_Z_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *w > 0 && *h > 0;
}

static bool g_measureFault = false;

static bool OutOfPlayable(const float p[3])
{
    __try {
        return ((OutOfPlayableFn)(g_exeBase + OUT_OF_PLAYABLE_RVA))(World_Object(), p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_measureFault = true;
        return false;
    }
}

// ---------------------------------------------------------------------------
// Where the cut runs
// ---------------------------------------------------------------------------
//
// A broken line: a few general directions drawn from the world's seed, and for
// each, knots about KNOT_SPACING_M apart moved at random; the most even result
// wins. After every move the whole line shifts so the playable land stays half
// and half (water is not area: a half of open sea is no republic). No customs house
// may lie within CUSTOMS_CLEAR_M of the line. What is judged, in order: the border
// crossings (in East vs West only
// that each side of the world has at least one land customs house on the half of
// its currency, so each player can buy abroad with their own money; in Equal,
// customs houses and the other buildings on the border, each type shared evenly);
// each resource's
// amount, which counts only past RESOURCE_TOLERANCE from an even share; how many
// deposits (patches) each side has, whole ones where it can be, and a resource
// with only one deposit cut in two.
//
// Only exact arithmetic is used (no sine or exp: the CRT's differ by CPU), and all
// it reads is the same on every PC: the save everyone loaded. The cut is kept in
// the save, so what players build or mine later never moves it.

#define CUT_DIRECTIONS 12
#define CUT_ROUNDS 1500       // moves of the knots, per direction
#define CUT_HEAT 0.05         // how readily a worse line is taken at the start; none at the end
#define KNOT_SPACING_M 2500.0f
#define MIN_KNOTS 4
#define MAX_SLOPE 1.2f        // of a piece: about 50 degrees off the general direction
#define MAX_BEND 1.5f         // the change of slope at a knot
#define MAX_SWING 0.3f        // how far a knot may go, of the map's smaller side
#define DEPOSIT_MIN_SHARE 0.05 // of its resource: smaller deposits are not counted as patches
#define DEPOSIT_REACH 2       // cells: deposits this close are one
#define PATCH_WEIGHT 0.25     // one patch beyond an even count
#define CUT_WEIGHT 0.08       // a deposit cut in halves, while the resource has others
#define LONE_WEIGHT 0.5       // the only deposit of a resource not cut in halves
// That deposit wholly on one side: how far the line is from it, per km. Without a
// slope to follow, moves that only bring the line nearer change nothing and are lost,
// and on some seeds one player got all of it.
#define LONE_MISS_SHARE 0.02
#define LONE_DISTANCE_WEIGHT 0.3
#define RESOURCE_CHANNELS 5 // coal, iron, oil, bauxite, uranium: resourcemap RGB, resourcemap2 RG
#define RESOURCE_CELL 8     // texels added into one point
#define MAX_CROSSINGS 1024
#define CUT_CELL_STRIDE 3 // of the playable grid's rows and columns, one in this many while the knots move
#define RESOURCE_TOLERANCE 0.10 // |a - b| / (a + b): 45% to 55% is even enough
// A building's type: its category, and $BORDER_BUILDING (BuildingType_LoadIni,
// 10fefe: the record starts at rbp+0x1ab0, as its category at rbp+0x1e10 shows).
#define BUILDING_TYPE 0x318
#define TYPE_CATEGORY 0x360
#define TYPE_BORDER_FLAG 0xAE0
#define CATEGORY_CUSTOMS 0x14
#define CUSTOMS_CLEAR_M 500.0f // so its yard and roads stay on one side
#define CLEAR_WEIGHT 3.0 // a customs house too close counts more than a whole crossing
#define SEA_LEVEL 0.0f // the terrain's height at the water (the game's border signs stop there too)
// A customs house's side of the world: 3 the West, dollars; 4 the Soviets, rubles
// (the load marks its own border buildings so, 43768d and 4376cd). Half 0 is the East.
#define BUILDING_BLOC 0x9EC
#define BLOC_WEST 3
#define BLOC_SOVIET 4
#define CROSSING_GROUPS 64 // one per building type, and the last for all the customs houses
#define CROSSING_ALL_CUSTOMS (CROSSING_GROUPS - 1)

struct ResourcePoint {
    float x, z, amount;
    int channel, deposit;
};
struct Deposit {
    int channel;
    double amount;
};
struct Crossing {
    float x, z;
    int group;
    int bloc; // East vs West: BLOC_* of a customs house that has a side to be on, else 0
};

static std::vector<ResourcePoint> g_resources;
static std::vector<Deposit> g_deposits;
static std::vector<double> g_depositLow; // ScoreCut: each deposit's amount below the line
static double g_resourceTotal[RESOURCE_CHANNELS];
static std::vector<Crossing> g_crossings;
static std::vector<Crossing> g_customs; // every customs house, for the distance from the line
static int g_cells = 0; // playable cells in g_cellX / g_cellZ
static float g_cellX[CUT_GRID * CUT_GRID], g_cellZ[CUT_GRID * CUT_GRID];
static Cut g_savedCut;
static bool g_cutSaved = false; // g_savedCut is this world's cut

static unsigned long long CutMix(unsigned long long x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// One 32-bit-per-texel resource map of the save. Column x, row z, from the
// map's corner, as the game's own resource brush reads them (238b70); inferred.
static void ReadResourceMap(const char* folder, const char* name, int firstChannel, int channels, float w, float h)
{
    char path[MAX_PATH + 32];
    if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", folder, name) < 0) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return;
    unsigned char head[128];
    std::vector<unsigned char> row;
    if (fread(head, 1, sizeof(head), f) == sizeof(head)) {
        unsigned height, width, bits;
        memcpy(&height, head + 12, 4);
        memcpy(&width, head + 16, 4);
        memcpy(&bits, head + 88, 4);
        if (bits == 32 && width && height && width <= 8192 && height <= 8192) {
            unsigned cols = width / RESOURCE_CELL + 1, rows = height / RESOURCE_CELL + 1;
            std::vector<double> sums(cols * rows * channels, 0.0);
            row.resize(width * 4);
            for (unsigned z = 0; z < height && fread(row.data(), 1, row.size(), f) == row.size(); z++)
                for (unsigned x = 0; x < width; x++)
                    for (int c = 0; c < channels; c++)
                        sums[(z / RESOURCE_CELL * cols + x / RESOURCE_CELL) * channels + c] += row[x * 4 + c];
            // Each channel's cells, joined into its deposits.
            std::vector<int> ids(cols * rows), stack;
            for (int c = 0; c < channels; c++) {
                std::fill(ids.begin(), ids.end(), -1);
                for (unsigned start = 0; start < cols * rows; start++) {
                    double first = sums[start * channels + c];
                    if (first <= 0 || ids[start] >= 0) continue;
                    int id = (int)g_deposits.size();
                    double amount = 0;
                    ids[start] = id;
                    stack.push_back((int)start);
                    while (!stack.empty()) {
                        unsigned at = (unsigned)stack.back();
                        stack.pop_back();
                        double here = sums[at * channels + c];
                        amount += here;
                        unsigned r = at / cols, q = at % cols;
                        float px = ((q + 0.5f) * RESOURCE_CELL / width - 0.5f) * w;
                        float pz = ((r + 0.5f) * RESOURCE_CELL / height - 0.5f) * h;
                        g_resources.push_back({ px, pz, (float)here, firstChannel + c, id });
                        for (int dr = -DEPOSIT_REACH; dr <= DEPOSIT_REACH; dr++)
                            for (int dq = -DEPOSIT_REACH; dq <= DEPOSIT_REACH; dq++) {
                                int nr = (int)r + dr, nq = (int)q + dq;
                                if (nr < 0 || nq < 0 || nr >= (int)rows || nq >= (int)cols) continue;
                                unsigned next = (unsigned)nr * cols + (unsigned)nq;
                                if (ids[next] >= 0 || sums[next * channels + c] <= 0) continue;
                                ids[next] = id;
                                stack.push_back((int)next);
                            }
                    }
                    g_deposits.push_back({ firstChannel + c, amount });
                    g_resourceTotal[firstChannel + c] += amount;
                }
            }
        }
    }
    fclose(f);
}

// The buildings that lead abroad: the customs houses (the air ones have no
// border flag) and what else the game marks as on the border. Each type is its
// own group, so three of one kind and one of another do not count as even.
static void FindCrossings()
{
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!World_Buildings(&begin, &end)) return;
    const BYTE* types[CROSSING_ALL_CUSTOMS] = {};
    bool eastWest = SaveSync_GetGameVariant() == SESSION_REPUBLICS_EAST_WEST;
    __try {
        for (unsigned char** b = begin; b < end && g_crossings.size() < MAX_CROSSINGS; ++b) {
            if (!*b) continue;
            const BYTE* type = *(const BYTE**)(*b + BUILDING_TYPE);
            if (!type) continue;
            bool customs = *(const int*)(type + TYPE_CATEGORY) == CATEGORY_CUSTOMS;
            if (!customs && !type[TYPE_BORDER_FLAG]) continue;
            int group = 0;
            while (group < CROSSING_ALL_CUSTOMS && types[group] && types[group] != type) group++;
            if (group == CROSSING_ALL_CUSTOMS) continue;
            types[group] = type;
            float pos[3];
            if (!World_BuildingPos(*b, pos)) continue;
            if (customs) g_customs.push_back({ pos[0], pos[2], 0, 0 });
            if (eastWest) {
                // Only land customs houses with a side count here; the air ones have no border flag.
                int bloc = customs && type[TYPE_BORDER_FLAG] ? *(const int*)(*b + BUILDING_BLOC) : 0;
                if (bloc == BLOC_WEST || bloc == BLOC_SOVIET) g_crossings.push_back({ pos[0], pos[2], 0, bloc });
                continue;
            }
            g_crossings.push_back({ pos[0], pos[2], group, 0 });
            if (customs) g_crossings.push_back({ pos[0], pos[2], CROSSING_ALL_CUSTOMS, 0 });
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_measureFault = true;
    }
}

struct CutScore {
    int crossingsOff;    // crossings beyond an even share (an odd count leaves one over)
    double resourcesOff; // mean of each resource's |a - b| / (a + b) past RESOURCE_TOLERANCE
    double worstOff;     // the most unevenly shared one, past it too
    int patchesOff;      // deposits beyond an even count, each to the side with more of it
    double cuts;         // deposits cut, by how evenly; a lone one by how unevenly
    double tooClose;     // customs houses within CUSTOMS_CLEAR_M, more the closer
    double Total() const
    {
        return crossingsOff + tooClose * CLEAR_WEIGHT + resourcesOff * 2 + worstOff * 4 + patchesOff * PATCH_WEIGHT + cuts;
    }
};

// The measured things in the direction's own frame.
struct FramePoint {
    float u, v, amount;
    int kind, deposit;
};
static std::vector<FramePoint> g_frameResources, g_frameCrossings, g_frameCells, g_frameCustoms;
static std::vector<double> g_frameDepositU, g_frameDepositV; // each deposit's middle, by its amount
static std::vector<int> g_coarseCells; // an even grid of the playable cells

static void MakeFrame(float nx, float nz)
{
    g_frameResources.clear();
    g_frameCrossings.clear();
    g_frameCells.clear();
    g_frameCustoms.clear();
    for (const Crossing& p : g_customs)
        g_frameCustoms.push_back({ p.z * nx - p.x * nz, p.x * nx + p.z * nz, 1.0f, 0, 0 });
    g_frameDepositU.assign(g_deposits.size(), 0.0);
    g_frameDepositV.assign(g_deposits.size(), 0.0);
    for (const ResourcePoint& p : g_resources) {
        g_frameResources.push_back({ p.z * nx - p.x * nz, p.x * nx + p.z * nz, p.amount, p.channel, p.deposit });
        const FramePoint& f = g_frameResources.back();
        g_frameDepositU[p.deposit] += (double)f.u * f.amount / g_deposits[p.deposit].amount;
        g_frameDepositV[p.deposit] += (double)f.v * f.amount / g_deposits[p.deposit].amount;
    }
    for (const Crossing& p : g_crossings)
        g_frameCrossings.push_back({ p.z * nx - p.x * nz, p.x * nx + p.z * nz, 1.0f, p.group, p.bloc });
    for (int k : g_coarseCells)
        g_frameCells.push_back({ g_cellZ[k] * nx - g_cellX[k] * nz, g_cellX[k] * nx + g_cellZ[k] * nz, 1.0f, 0, 0 });
}

// What moves the line, at the same u everywhere, so the cells lie half on each side.
static float AreaShift(const Cut& c)
{
    int n = 0;
    for (const FramePoint& p : g_frameCells) g_inside[n++] = p.v - CutAt(c, p.u);
    if (!n) return 0;
    std::nth_element(g_inside, g_inside + n / 2, g_inside + n);
    return g_inside[n / 2];
}

// The line as it would stand with the areas equal.
static CutScore ScoreCut(const Cut& c)
{
    CutScore s = { 0, 0.0, 0.0, 0, 0.0, 0.0 };
    float shift = AreaShift(c);
    for (const FramePoint& p : g_frameCustoms) {
        float slope;
        float f = CutAt(c, p.u, &slope);
        float d = fabsf(p.v - shift - f) / sqrtf(1.0f + slope * slope);
        if (d < CUSTOMS_CLEAR_M) s.tooClose += 1.0 + (CUSTOMS_CLEAR_M - d) / CUSTOMS_CLEAR_M;
    }
    double side[RESOURCE_CHANNELS] = {};
    g_depositLow.assign(g_deposits.size(), 0.0);
    for (const FramePoint& p : g_frameResources)
        if (p.v - shift < CutAt(c, p.u)) {
            side[p.kind] += p.amount;
            g_depositLow[p.deposit] += p.amount;
        }
    int used = 0;
    for (int i = 0; i < RESOURCE_CHANNELS; i++) {
        if (g_resourceTotal[i] <= 0) continue;
        double off = fabs(2 * side[i] - g_resourceTotal[i]) / g_resourceTotal[i] - RESOURCE_TOLERANCE;
        if (off < 0) off = 0;
        s.resourcesOff += off;
        if (off > s.worstOff) s.worstOff = off;
        used++;
    }
    if (used) s.resourcesOff /= used;
    int patches[RESOURCE_CHANNELS] = {}, lows[RESOURCE_CHANNELS] = {};
    for (const Deposit& d : g_deposits)
        if (d.amount >= g_resourceTotal[d.channel] * DEPOSIT_MIN_SHARE) patches[d.channel]++;
    for (size_t i = 0; i < g_deposits.size(); i++) {
        const Deposit& d = g_deposits[i];
        if (d.amount < g_resourceTotal[d.channel] * DEPOSIT_MIN_SHARE) continue;
        double low = g_depositLow[i] / d.amount;
        if (low >= 0.5) lows[d.channel]++;
        if (patches[d.channel] == 1) {
            s.cuts += fabs(2 * low - 1) * LONE_WEIGHT;
            if (low <= LONE_MISS_SHARE || low >= 1 - LONE_MISS_SHARE)
                s.cuts += fabs(g_frameDepositV[i] - shift - CutAt(c, (float)g_frameDepositU[i])) / 1000.0 * LONE_DISTANCE_WEIGHT;
        } else s.cuts += (low < 0.5 ? low : 1 - low) * 2 * CUT_WEIGHT;
    }
    for (int i = 0; i < RESOURCE_CHANNELS; i++) {
        int off = abs(2 * lows[i] - patches[i]);
        if (patches[i] > 1 && off > patches[i] % 2) s.patchesOff += off - patches[i] % 2;
    }
    int low[CROSSING_GROUPS] = {}, all[CROSSING_GROUPS] = {};
    bool hasBloc[2] = {}, blocHome[2] = {}; // the Soviets', the West's
    for (const FramePoint& p : g_frameCrossings) {
        bool isLow = p.v - shift < CutAt(c, p.u);
        if (p.deposit) { // here: the customs house's side of the world
            int west = p.deposit == BLOC_WEST ? 1 : 0;
            hasBloc[west] = true;
            if (isLow != (west == 1)) blocHome[west] = true;
            continue;
        }
        all[p.kind]++;
        if (isLow) low[p.kind]++;
    }
    for (int b = 0; b < 2; b++)
        if (hasBloc[b] && !blocHome[b]) s.crossingsOff++;
    for (int t = 0; t < CROSSING_GROUPS; t++) {
        int off = abs(2 * low[t] - all[t]);
        if (off > all[t] % 2) s.crossingsOff += off - all[t] % 2;
    }
    return s;
}

// A number in [0, 1) from the next step of the seed.
static double CutRandom(unsigned long long* seed)
{
    *seed = CutMix(*seed);
    return (double)(*seed >> 11) / (double)(1ull << 53);
}

static bool CutAllowed(const Cut& c, float swing)
{
    float slopeMax = c.step * MAX_SLOPE, bendMax = c.step * MAX_BEND;
    for (int i = 0; i < c.knots; i++) {
        if (fabsf(c.off[i]) > swing) return false;
        if (i + 1 < c.knots && fabsf(c.off[i + 1] - c.off[i]) > slopeMax) return false;
        if (i > 0 && i + 1 < c.knots && fabsf(c.off[i - 1] - 2 * c.off[i] + c.off[i + 1]) > bendMax) return false;
    }
    return true;
}

// Main thread. A fault leaves no cut: it is measured again next time, as a
// half-measured one would differ between the PCs.
static void MakeCut(float w, float h)
{
    g_measureFault = false;
    // The land of the playable area; a map that is nearly all water counts as a whole.
    for (int landOnly = 1; landOnly >= 0; landOnly--) {
        g_cells = 0;
        g_coarseCells.clear();
        for (int i = 0; i < CUT_GRID; i++)
            for (int j = 0; j < CUT_GRID; j++) {
                float p[3] = { ((i + 0.5f) / CUT_GRID - 0.5f) * w, 0, ((j + 0.5f) / CUT_GRID - 0.5f) * h };
                if (OutOfPlayable(p)) continue;
                float y;
                if (landOnly && TerrainHook_HeightAt(p, &y) && y <= SEA_LEVEL) continue;
                if (i % CUT_CELL_STRIDE == 0 && j % CUT_CELL_STRIDE == 0) g_coarseCells.push_back(g_cells);
                g_cellX[g_cells] = p[0];
                g_cellZ[g_cells++] = p[2];
            }
        if (g_cells * 20 >= CUT_GRID * CUT_GRID) break;
    }
    g_resources.clear();
    g_deposits.clear();
    g_crossings.clear();
    g_customs.clear();
    memset(g_resourceTotal, 0, sizeof(g_resourceTotal));
    char folder[MAX_PATH];
    if (ModSave_LoadedFolder(folder, sizeof(folder))) {
        ReadResourceMap(folder, "resourcemap.dds", 0, 3, w, h);
        ReadResourceMap(folder, "resourcemap2.dds", 3, 2, w, h);
    }
    FindCrossings();
    if (g_measureFault || !g_cells) return;

    // The knots cover the map's diagonal, whatever the direction.
    float len = sqrtf(w * w + h * h);
    int knots = (int)ceilf(len / KNOT_SPACING_M) + 1;
    knots = knots < MIN_KNOTS ? MIN_KNOTS : knots > MAX_KNOTS ? MAX_KNOTS : knots;
    float swing = (w < h ? w : h) * MAX_SWING;

    unsigned long long seed = ModSave_WorldSeed();
    Cut line, bestLine;
    memset(&line, 0, sizeof(line));
    line.startU = -len * 0.5f;
    line.step = len / (float)(knots - 1);
    line.knots = knots;
    bestLine = line;
    CutScore score = { 1000000, 2.0, 2.0, 0, 0.0, 0.0 };
    for (int direction = 0; direction < CUT_DIRECTIONS; direction++) {
        // A point on the circle by exact arithmetic: t in [-1, 1) covers every line's direction.
        double t = CutRandom(&seed) * 2.0 - 1.0;
        double den = 1.0 + t * t;
        line.nx = (float)((1.0 - t * t) / den);
        line.nz = (float)(2.0 * t / den);
        for (int k = 0; k < knots; k++) line.off[k] = 0;
        MakeFrame(line.nx, line.nz);
        CutScore now = ScoreCut(line);
        // The same line with its halves the other way round, if that puts the customs houses better.
        MakeFrame(-line.nx, -line.nz);
        Cut turned = line;
        turned.nx = -line.nx;
        turned.nz = -line.nz;
        CutScore other = ScoreCut(turned);
        if (other.crossingsOff < now.crossingsOff) {
            line = turned;
            now = other;
        } else {
            MakeFrame(line.nx, line.nz);
        }
        double total = now.Total();
        if (total < score.Total()) {
            score = now;
            bestLine = line;
        }
        for (int round = 0; round < CUT_ROUNDS; round++) {
            float done = (float)round / CUT_ROUNDS;
            float reach = line.step * (0.8f - 0.7f * done);
            double heat = CUT_HEAT * (1.0f - done) * (1.0f - done);
            int knot = (int)(CutRandom(&seed) * knots);
            float move = (float)(CutRandom(&seed) * 2.0 - 1.0) * reach;
            double dice = CutRandom(&seed);
            float was = line.off[knot];
            line.off[knot] = was + move;
            if (CutAllowed(line, swing)) {
                CutScore s = ScoreCut(line);
                double worse = s.Total() - total;
                // A worse line is taken now and then, less as it cools, to get out of a dead end.
                if (worse < 0 || (heat > 0 && dice < heat / (heat + worse * 4.0))) {
                    total = s.Total();
                    if (total < score.Total()) {
                        score = s;
                        bestLine = line;
                    }
                    continue;
                }
            }
            line.off[knot] = was;
        }
    }
    // The line as it was judged, shifted exactly as it was: the deposits lie close
    // to it, so any other shift would undo what was found.
    line = bestLine;
    MakeFrame(line.nx, line.nz);
    float shift = AreaShift(line);
    for (int i = 0; i < line.knots; i++) line.off[i] += shift;
    score = ScoreCut(line);
    g_cut = line;
    g_cutReady = true;
    g_savedCut = line;
    g_cutSaved = true;

    int lowCells = 0;
    for (int k = 0; k < g_cells; k++) {
        float u = g_cellZ[k] * line.nx - g_cellX[k] * line.nz, v = g_cellX[k] * line.nx + g_cellZ[k] * line.nz;
        if (v < CutAt(line, u)) lowCells++;
    }
    float nearest = -1;
    for (const FramePoint& p : g_frameCustoms) {
        float slope;
        float f = CutAt(line, p.u, &slope);
        float d = fabsf(p.v - f) / sqrtf(1.0f + slope * slope);
        if (nearest < 0 || d < nearest) nearest = d;
    }
    int customs = 0, customsLow = 0, border = 0, borderLow = 0;
    int bloc[2] = {}, blocRight[2] = {};
    for (const FramePoint& p : g_frameCrossings) {
        bool isLow = p.v < CutAt(line, p.u);
        if (p.deposit) {
            int west = p.deposit == BLOC_WEST ? 1 : 0;
            bloc[west]++;
            if (isLow != (west == 1)) blocRight[west]++;
        } else if (p.kind == CROSSING_ALL_CUSTOMS) {
            customs++;
            if (isLow) customsLow++;
        } else {
            border++;
            if (isLow) borderLow++;
        }
    }
    Logf("republicmp  REPUBLICS: the cut - direction (%.3f, %.3f), %d knots %.0f m apart; half 0 has %.1f%% of the land (%d cells); the nearest customs house %.0f m from the line; "
         "western land customs %d of %d on the West's half, Soviet %d of %d on the East's; other customs %d of %d, "
         "border buildings %d of %d; crossings %d off; patches %d off",
         line.nx, line.nz, knots, line.step, 100.0 * lowCells / g_cells, g_cells, nearest, blocRight[1], bloc[1], blocRight[0], bloc[0], customsLow, customs,
         borderLow, border, score.crossingsOff, score.patchesOff);
    for (int i = 0; i < RESOURCE_CHANNELS; i++) {
        if (g_resourceTotal[i] <= 0) continue;
        double low = 0;
        int lows = 0, patches = 0;
        for (size_t d = 0; d < g_deposits.size(); d++) {
            if (g_deposits[d].channel != i) continue;
            low += g_depositLow[d];
            if (g_deposits[d].amount < g_resourceTotal[i] * DEPOSIT_MIN_SHARE) continue;
            patches++;
            if (g_depositLow[d] * 2 >= g_deposits[d].amount) lows++;
        }
        Logf("republicmp  REPUBLICS:   resource %d: half 0 has %.1f%%, patches %d of %d", i, 100.0 * low / g_resourceTotal[i], lows, patches);
    }
    std::vector<Crossing>().swap(g_crossings);
    std::vector<Crossing>().swap(g_customs);
    std::vector<FramePoint>().swap(g_frameCustoms);
    std::vector<ResourcePoint>().swap(g_resources);
    std::vector<Deposit>().swap(g_deposits);
    std::vector<double>().swap(g_depositLow);
    std::vector<double>().swap(g_frameDepositU);
    std::vector<double>().swap(g_frameDepositV);
    std::vector<FramePoint>().swap(g_frameResources);
    std::vector<FramePoint>().swap(g_frameCrossings);
    std::vector<FramePoint>().swap(g_frameCells);
    std::vector<int>().swap(g_coarseCells);
}

// ---------------------------------------------------------------------------
// The customs houses on the line
// ---------------------------------------------------------------------------
//
// Every SPAWN_EVERY_M along the line a customs house, spawned free of charge, its
// middle between its ports exactly on the line and its ports across it, so each
// player reaches it from their own side; the world's seed picks which are small,
// medium and big. A place: land all around within SPAWN_CLEAR_M and inside the playable
// border, level to SPAWN_LEVEL_M within SPAWN_LEVEL_RADIUS_M, no building near. One
// that does not fit moves along the line up to SPAWN_SLIDE_M; a place with none is
// left out. Fewer than SPAWN_MIN_HOUSES on the whole line: a second pass between the
// places, then a last with easier ground. The host picks: alone it places them at
// once and the save carries them to who joins, else each is a command. A save keeps
// how far it got.

// A type's ports (its ini): normal ones at one end of an axis, border ones at the
// other, and where the middle between them lies on that axis.
struct SpawnType {
    const char* name;
    bool portsOnX;   // along the building's x axis, else its z axis
    float middle;
};
static const SpawnType kSpawnSmall = { "zoll_sahy", false, 0.758f };   // a road: z 17.53 and -16.01
static const SpawnType kSpawnMedium = { "zoll_siatre", false, -1.564f }; // a road and rails: z 23.48 and -26.61
static const SpawnType kSpawnBig = { "zoll_milhost", true, 4.524f };   // roads and rails: x 64.30 and -55.26
#define SPAWN_EVERY_M 4000.0f
#define SPAWN_MIN_HOUSES 2
#define SPAWN_PASSES 3
#define SPAWN_EASY_CLEAR_M 50.0f  // the last pass
#define SPAWN_EASY_LEVEL_M 20.0f
#define SPAWN_SLIDE_M 800.0f
#define SPAWN_SLIDE_STEP_M 100.0f
#define SPAWN_CLEAR_M 100.0f
#define SPAWN_LEVEL_M 10.0f
#define SPAWN_LEVEL_RADIUS_M 200.0f
#define SPAWN_NO_BUILDING_M 150.0f
#define SPAWN_CLEAR_STEP_M 25.0f
#define SPAWN_LEVEL_STEP_M 50.0f

static bool g_spawnDone = false;    // this world has them all (from the save, or made)
static bool g_spawnWaiting = false; // the host's command is on its way
static int g_spawnPass = 0;         // 0 the places, 1 between them, 2 between again on easier ground
static int g_spawnPlace = 0;        // the place along the line it has reached
static int g_spawnSlide = 0;        // spots of this place the engine refused
static int g_spawnMade = 0;

static bool SpawnSpotFits(float x, float z, float clear, float level)
{
    float at[3] = { x, 0, z }, y0;
    if (!TerrainHook_HeightAt(at, &y0)) return false;
    for (float dx = -clear; dx <= clear; dx += SPAWN_CLEAR_STEP_M)
        for (float dz = -clear; dz <= clear; dz += SPAWN_CLEAR_STEP_M) {
            if (dx * dx + dz * dz > clear * clear) continue;
            float p[3] = { x + dx, 0, z + dz }, y;
            if (OutOfPlayable(p)) return false;
            if (!TerrainHook_HeightAt(p, &y) || y <= SEA_LEVEL) return false;
        }
    for (float dx = -SPAWN_LEVEL_RADIUS_M; dx <= SPAWN_LEVEL_RADIUS_M; dx += SPAWN_LEVEL_STEP_M)
        for (float dz = -SPAWN_LEVEL_RADIUS_M; dz <= SPAWN_LEVEL_RADIUS_M; dz += SPAWN_LEVEL_STEP_M) {
            if (dx * dx + dz * dz > SPAWN_LEVEL_RADIUS_M * SPAWN_LEVEL_RADIUS_M) continue;
            float p[3] = { x + dx, 0, z + dz }, y;
            if (!TerrainHook_HeightAt(p, &y) || fabsf(y - y0) > level) return false;
        }
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (World_Buildings(&begin, &end))
        for (unsigned char** b = begin; b < end; ++b) {
            float pos[3];
            if (!*b || !World_BuildingPos(*b, pos)) continue;
            float dx = pos[0] - x, dz = pos[2] - z;
            if (dx * dx + dz * dz < SPAWN_NO_BUILDING_M * SPAWN_NO_BUILDING_M) return false;
        }
    return true;
}

// Spots of one place in the order they are tried: on it, then ever further to each side.
static float SpawnSlide(int index)
{
    int step = (index + 1) / 2;
    return (index % 2 ? 1.0f : -1.0f) * step * SPAWN_SLIDE_STEP_M;
}

#define SPAWN_SLIDES ((int)(SPAWN_SLIDE_M / SPAWN_SLIDE_STEP_M) * 2 + 1)

// Main thread, a frame observer, the host. One customs house at a time; in a
// session the command waits for the player, so it is made on every PC.
static void TrySpawnCustoms(const Cut& c)
{
    if (g_spawnDone || g_spawnWaiting || !SaveSync_IsHostRole()) return;
    bool lockstep = Commands_Capturing();
    if (lockstep && !LockstepNet_CommandsReady()) return;
    unsigned long long seed = CutMix(ModSave_WorldSeed() ^ 0xC057u);
    float length = c.step * (c.knots - 1);
    float phase = (float)(seed % 1000) / 1000.0f * SPAWN_EVERY_M;
    g_measureFault = false;
    for (;; g_spawnPlace++, g_spawnSlide = 0) {
        // Each pass's places lie between the last one's.
        float along = phase + SPAWN_EVERY_M * g_spawnPlace + SPAWN_EVERY_M * 0.5f * (g_spawnPass > 0) + SPAWN_EVERY_M * 0.25f * (g_spawnPass > 1);
        if (g_measureFault) return;
        if (along >= length) {
            if (g_spawnMade < SPAWN_MIN_HOUSES && g_spawnPass + 1 < SPAWN_PASSES) {
                g_spawnPass++;
                g_spawnPlace = -1; // the loop's step makes it 0
                continue;
            }
            g_spawnDone = true;
            double rub = 0, usd = 0;
            World_Money(&rub, &usd);
            Logf("republicmp  REPUBLICS: %d customs houses on the line (pass %d); the balances now %.2f rubles %.2f dollars", g_spawnMade,
                 g_spawnPass, rub, usd);
            return;
        }
        float clear = g_spawnPass < 2 ? SPAWN_CLEAR_M : SPAWN_EASY_CLEAR_M;
        float level = g_spawnPass < 2 ? SPAWN_LEVEL_M : SPAWN_EASY_LEVEL_M;
        for (; g_spawnSlide < SPAWN_SLIDES; g_spawnSlide++) {
            float u = c.startU + along + SpawnSlide(g_spawnSlide), slope;
            float v = CutAt(c, u, &slope);
            float x = c.nx * v - c.nz * u, z = c.nz * v + c.nx * u;
            if (!SpawnSpotFits(x, z, clear, level)) continue;
            // The line's normal here: (n - slope * dir), its length sqrt(1 + slope^2).
            float across[2] = { c.nx + slope * c.nz, c.nz - slope * c.nx };
            float size = sqrtf(across[0] * across[0] + across[1] * across[1]);
            across[0] /= size;
            across[1] /= size;
            // The first two are a medium and a big one, the seed says which comes first; the rest as it falls.
            static const SpawnType* const kSpawnTypes[3] = { &kSpawnSmall, &kSpawnMedium, &kSpawnBig };
            bool firstBig = (CutMix(seed) & 1) != 0;
            const SpawnType& type = g_spawnMade == 0   ? (firstBig ? kSpawnBig : kSpawnMedium)
                                    : g_spawnMade == 1 ? (firstBig ? kSpawnMedium : kSpawnBig)
                                                       : *kSpawnTypes[CutMix(seed + (unsigned)g_spawnMade) % 3];
            // The yaw turns the axis with the ports onto the normal: x lies a quarter turn from z. It
            // only goes in the command, which carries the number: atan2 differing by CPU cannot split the games.
            float yaw = atan2f(across[0], across[1]);
            if (type.portsOnX) yaw -= 1.5707964f;
            // The middle between the ports on the line.
            x -= across[0] * type.middle;
            z -= across[1] * type.middle;
            float at[3] = { x, 0, z }, y = 0;
            TerrainHook_HeightAt(at, &y);
            BuildingMsg m = {};
            m.kind = BUILDING_PLACE;
            m.mode.rub = 1;
            m.x = m.cursorX = x;
            m.y = m.cursorY = y;
            m.z = m.cursorZ = z;
            m.cursorRot = yaw;
            m.pickModes = BUILDING_PICK_NO_LINKS | BUILDING_PICK_FINISHED;
            strncpy_s(m.typeName, type.name, _TRUNCATE);
            g_spawnWaiting = true;
            double rub = 0, usd = 0;
            World_Money(&rub, &usd);
            Logf("republicmp  REPUBLICS: pass %d place %d: %s at (%.0f, %.0f), height %.1f; the balances before it %.2f rubles %.2f dollars",
                 g_spawnPass, g_spawnPlace, m.typeName, x, z, y, rub, usd);
            if (lockstep) Commands_Submit(LS_CMD_SPAWN_BUILDING, &m, sizeof(m));
            else Commands_SpawnBuildingNow(m);
            return;
        }
    }
}

// On every PC, in the order the host made them.
static void SpawnedCustoms(bool placed)
{
    g_spawnWaiting = false;
    if (placed) {
        g_spawnMade++;
        g_spawnPlace++;
        g_spawnSlide = 0;
    } else {
        g_spawnSlide++;
    }
    Logf("republicmp  REPUBLICS: the customs house %s", placed ? "stands" : "was refused by the engine");
}

// g_savedCut holds this world's cut from its first measure until the next load: a
// host who plays on alone and takes a new player saves the same line.
static void WriteCutSection()
{
    if (g_cutReady) {
        g_savedCut = g_cut;
        g_cutSaved = true;
    }
    if (!g_cutSaved) return;
    float v[4 + MAX_KNOTS] = { g_savedCut.nx, g_savedCut.nz, g_savedCut.startU, g_savedCut.step };
    memcpy(v + 4, g_savedCut.off, sizeof(float) * g_savedCut.knots);
    ModSave_PutInt("customs", g_spawnDone ? 1 : 0);
    ModSave_PutInt("customs_pass", g_spawnPass);
    ModSave_PutInt("customs_place", g_spawnPlace);
    ModSave_PutInt("customs_made", g_spawnMade);
    ModSave_PutInt("knots", g_savedCut.knots);
    ModSave_PutFloats("line", v, 4 + g_savedCut.knots);
}

static void ReadCutSection(bool found)
{
    g_cutSaved = false;
    long long knots = 0, spawned = 0, place = 0, made = 0, pass = 0;
    g_spawnWaiting = false;
    g_spawnSlide = 0;
    g_spawnDone = found && ModSave_GetInt("customs", &spawned) && spawned == 1;
    g_spawnPass = found && ModSave_GetInt("customs_pass", &pass) ? (int)pass : 0;
    g_spawnPlace = found && ModSave_GetInt("customs_place", &place) ? (int)place : 0;
    g_spawnMade = found && ModSave_GetInt("customs_made", &made) ? (int)made : 0;
    if (!found || !ModSave_GetInt("knots", &knots) || knots < 2 || knots > MAX_KNOTS) return;
    float v[4 + MAX_KNOTS];
    if (!ModSave_GetFloats("line", v, 4 + (int)knots) || v[3] <= 0) return;
    memset(&g_savedCut, 0, sizeof(g_savedCut));
    g_savedCut.nx = v[0];
    g_savedCut.nz = v[1];
    g_savedCut.startU = v[2];
    g_savedCut.step = v[3];
    g_savedCut.knots = (int)knots;
    memcpy(g_savedCut.off, v + 4, sizeof(float) * (size_t)knots);
    g_cutSaved = true;
}

// A Republics session is running here (the host's choice, on both PCs).
static bool Active()
{
    return SaveSync_IsBusy() && SaveSync_GetGameMode() == SESSION_MODE_REPUBLICS;
}

// The cut of the world loaded now, if it was measured; any thread.
static const Cut* ReadyCut()
{
    return Active() && g_cutReady && g_cutLoads == Determinism_WorldLoads() ? &g_cut : nullptr;
}

// Main thread, outside the game's own calls (the observers, a command being
// made): the cut, measured on first use after each load - a player joining
// from a city of its own measures the host's world once it is loaded.
static const Cut* CurrentCut()
{
    if (!Active()) {
        g_cutReady = false;
        return nullptr;
    }
    unsigned loads = Determinism_WorldLoads();
    if (g_cutLoads != loads) g_cutReady = false;
    float w, h;
    if (!g_cutReady && World_IsReady() && MapSize(&w, &h)) {
        g_cutLoads = loads;
        if (g_cutSaved) {
            g_cut = g_savedCut;
            g_cutReady = true;
            Logf("republicmp  REPUBLICS: the cut comes from the save - direction (%.3f, %.3f), %d knots", g_cut.nx, g_cut.nz, g_cut.knots);
        } else {
            MakeCut(w, h);
        }
    }
    return g_cutReady ? &g_cut : nullptr;
}

// shortcut: 2 players, so the player is always 1; with more, the host has to
// tell each player its number.
static int LocalPlayer()
{
    return SaveSync_IsHostRole() ? 0 : 1;
}

// 0 the first half, 1 the second.
static int HalfOf(const float p[3], const Cut& c)
{
    return CutDistance(c, p) < 0 ? 0 : 1;
}

// The point is the position of a customs house standing on the line. Its settings
// and purchases name it by that position, whichever half its middle fell on.
static bool LineCustomsAt(const float p[3], const Cut& c)
{
    if (fabsf(CutDistance(c, p)) > LINE_CUSTOMS_M) return false;
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!World_Buildings(&begin, &end)) return false;
    __try {
        for (unsigned char** b = begin; b < end; ++b) {
            if (!*b) continue;
            const BYTE* type = *(const BYTE**)(*b + BUILDING_TYPE);
            if (!type || *(const int*)(type + TYPE_CATEGORY) != CATEGORY_CUSTOMS) continue;
            float pos[3];
            if (!World_BuildingPos(*b, pos)) continue;
            if (fabsf(pos[0] - p[0]) < LINE_CUSTOMS_SAME_M && fabsf(pos[2] - p[2]) < LINE_CUSTOMS_SAME_M) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Commands_SetAreaRule: a player builds and demolishes on their own half only,
// the host also while alone (COMMANDS_LOCAL_PLAYER). The game's own border
// still refuses what is out of the map.
static bool AreaRule(int player, const float pos[3], bool notify)
{
    // A local check runs inside the game's own call (and the bulldozer's on
    // any thread): it only uses a cut already measured.
    bool local = player == COMMANDS_LOCAL_PLAYER;
    const Cut* c = local ? ReadyCut() : CurrentCut();
    if (!pos) return c != nullptr;
    if (!c) return true;
    if (local) player = LocalPlayer();
    if (!isfinite(pos[0]) || !isfinite(pos[2])) return false;
    int hostHalf = SaveSync_GetHostSide(); // the host's choice: the player has the other half
    bool ok = HalfOf(pos, *c) == (player == 0 ? hostHalf : 1 - hostHalf) || LineCustomsAt(pos, *c);
    if (!ok && notify && player == LocalPlayer()) g_refusedAt = GetTickCount64();
    return ok;
}

// Wallets_SetHalfRule: whose money a building or a vehicle spends: everything
// belongs to one of the two. Inside the game's own calls.
static int OwnerHalf(const float pos[3])
{
    const Cut* c = ReadyCut();
    if (!c) return -1;
    return HalfOf(pos, *c);
}

// Commands_SetBorderRule: the line between the republics is a border for a foreign
// pipeline and a power connection, as the game's own border is: the model has to
// cross it. Customs houses come with the mode (TrySpawnCustoms), a player builds none.
// Only the geometry of what is being placed, so every PC answers the same; a
// command's area rule has measured the cut by then. Inside the game's own call.
static bool BorderRule(unsigned long long type, const float corners[4][3])
{
    const Cut* c = ReadyCut();
    if (!c) return false;
    const BYTE* t = (const BYTE*)type;
    if (!t[TYPE_BORDER_FLAG] || *(const int*)(t + TYPE_CATEGORY) == CATEGORY_CUSTOMS) return false;
    int half = HalfOf(corners[0], *c);
    for (int i = 1; i < 4; i++)
        if (HalfOf(corners[i], *c) != half) return true;
    return false;
}

// Commands_SetGlobalRule: the game's rules (the rules page) are the host's.
static bool GlobalRule(int player)
{
    if (!Active()) return true;
    bool ok = player == 0;
    if (!ok && player == LocalPlayer()) g_hostOnlyAt = GetTickCount64();
    return ok;
}

// Main thread: the cut's points on the terrain, for the overlay. Every frame
// of a lockstep session (Determinism_AddFrameObserver), and every step while
// the host waits alone, before the lockstep starts (a step observer). Only
// reads the world.
static void SampleBorder()
{
    const Cut* c = CurrentCut();
    if (!c) {
        g_borderCount = 0;
        g_stripCount = g_signCount[0] = g_signCount[1] = 0;
        return;
    }
    TrySpawnCustoms(*c);
    ULONGLONG now = GetTickCount64();
    if (g_borderCount && now - g_borderAt < BORDER_REFRESH_MS && g_borderLoads == g_cutLoads) return;
    g_borderLoads = g_cutLoads;
    float w, h;
    if (!MapSize(&w, &h)) return;
    // Along the line, end to end, inside the map.
    float len = sqrtf(w * w + h * h);
    float dir[2] = { -c->nz, c->nx };
    int n = 0;
    for (int i = 0; i < BORDER_POINTS; i++) {
        float t = ((float)i / (BORDER_POINTS - 1) - 0.5f) * len;
        float v = CutAt(*c, t);
        float* p = g_border[n];
        p[0] = c->nx * v + dir[0] * t;
        p[1] = 0;
        p[2] = c->nz * v + dir[1] * t;
        if (fabsf(p[0]) > w * 0.5f || fabsf(p[2]) > h * 0.5f) continue;
        float y;
        if (TerrainHook_HeightAt(p, &y)) p[1] = y + 2.0f; // just above the ground
        n++;
    }
    g_borderAt = now;
    g_borderCount = n;
    // The strip: from the first point inside the playable border to the last.
    int first = -1, last = -1;
    g_measureFault = false;
    for (int i = 0; i < n; i++)
        if (!OutOfPlayable(g_border[i])) {
            if (first < 0) first = i;
            last = i;
        }
    if (first < 0 || last <= first || g_measureFault) {
        g_stripCount = g_signCount[0] = g_signCount[1] = 0;
        return;
    }
    float ua = g_border[first][2] * c->nx - g_border[first][0] * c->nz;
    float ub = g_border[last][2] * c->nx - g_border[last][0] * c->nz;
    // Its points: every knot, so the pieces do not cut the corners, and between them
    // pieces of at most STRIP_STEP_M (their own length is more than along u on a slope).
    float stripU[STRIP_POINTS];
    int pieces = 0;
    stripU[0] = ua;
    for (int k = 0; k <= c->knots && pieces < STRIP_POINTS - 1; k++) {
        float end = k < c->knots ? c->startU + c->step * k : ub;
        if (end > ub) end = ub;
        float from = stripU[pieces];
        if (end <= from) continue;
        int steps = (int)ceilf((end - from) * STRIP_STRETCH / STRIP_STEP_M);
        if (steps < 1) steps = 1;
        for (int i = 1; i <= steps && pieces < STRIP_POINTS - 1; i++) stripU[++pieces] = from + (end - from) * i / steps;
    }
    if (pieces < 1) {
        g_stripCount = g_signCount[0] = g_signCount[1] = 0;
        return;
    }
    g_stripCount = 0; // the render thread draws none while they change
    for (int i = 0; i <= pieces; i++) {
        float t = stripU[i];
        float v = CutAt(*c, t);
        float* p = g_strip[i];
        p[0] = c->nx * v + dir[0] * t;
        p[1] = 0;
        p[2] = c->nz * v + dir[1] * t;
        float y;
        if (TerrainHook_HeightAt(p, &y)) p[1] = y; // as the game's: on the ground, drawn over it
    }
    g_stripCount = pieces + 1;
    // The signs: across from each piece's middle, on both sides, inside the map.
    for (int half = 0; half < 2; half++) {
        float k = half == 0 ? -SIGN_INSET_M : SIGN_INSET_M; // half 0 lies below the line
        int m = 0;
        g_signCount[half] = 0;
        for (int i = 0; i < pieces; i++) {
            float t = (stripU[i] + stripU[i + 1]) * 0.5f, slope;
            float v = CutAt(*c, t, &slope);
            float across = k / sqrtf(1.0f + slope * slope); // the normal of this piece: (n - slope * dir), normalized
            float* p = g_signs[half][m];
            p[0] = c->nx * v + dir[0] * t + (c->nx - slope * dir[0]) * across;
            p[1] = 0;
            p[2] = c->nz * v + dir[1] * t + (c->nz - slope * dir[1]) * across;
            if (OutOfPlayable(p)) continue;
            float d = CutDistance(*c, p);
            if (half == 0 ? d >= 0 : d <= 0) continue; // past a bend, on the other's side
            float y;
            p[1] = (TerrainHook_HeightAt(p, &y) ? fmaxf(y, 0.0f) : 0.0f) + SIGN_LIFT_M;
            m++;
        }
        g_signCount[half] = g_measureFault ? 0 : m;
    }
}

// On the terrain the line is dimmer, and fades out with its distance from the
// camera (metres of view depth): full up to NEAR, gone from FAR on. It is
// drawn in FADE_STEPS batches of one alpha each (the panel takes one colour
// per call).
static const float kTerrainRed[4] = { 0.72f, 0.16f, 0.14f, 0.55f };
#define BORDER_FADE_NEAR_M 300.0f
#define BORDER_FADE_FAR_M 1500.0f
#define FADE_STEPS 8
static float g_segments[FADE_STEPS][BORDER_POINTS * 4]; // render thread
static int g_segmentCount[FADE_STEPS];

// Render thread, right after the game's own border line (BorderDraw_SetAfter):
// the cut drawn the same way, the game's dash-and-dot strip on the ground.
static void DrawAfterBorder()
{
    int count = g_stripCount;
    if (!Active() || count < 2) return;
    BorderDraw_Strip(g_strip, count);
    if (!g_showSigns) return;
    bool equal = SaveSync_GetGameVariant() == SESSION_REPUBLICS_EQUAL;
    int n = 0;
    for (int half = 0; half < 2; half++)
        for (int i = 0, m = g_signCount[half]; i < m; i++) {
            BorderSign& s = g_drawnSigns[n++];
            memcpy(s.pos, g_signs[half][i], sizeof(s.pos));
            s.sign = (equal ? (i + half) % 2 : half) == 0 ? BORDER_SIGN_EAST : BORDER_SIGN_WEST;
            float clip[4];
            float depth = Camera_Clip(s.pos, clip) ? clip[3] : SIGN_HIDE_FAR_M;
            float t = (depth - SIGN_HIDE_NEAR_M) / (SIGN_HIDE_FAR_M - SIGN_HIDE_NEAR_M);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            s.size = t * t * (3 - 2 * t); // smooth
        }
    BorderDraw_Signs(g_drawnSigns, n);
}

bool Republics_GetShowSigns() { return g_showSigns; }

void Republics_SetShowSigns(bool on)
{
    g_showSigns = on;
    Profile_SetInt("overlay", "border_signs", on ? 1 : 0);
}

// Render thread, right before the interface draws (SimPatches_SetUnderUi):
// where the game's strip cannot be drawn, a line with the game's 2D panel,
// under the HUD.
static void DrawUnderUi()
{
    int count = g_borderCount;
    float w, h;
    if (BorderDraw_Ready() || !Active() || count < 2 || !Panel2D_ScreenSize(&w, &h)) return;
    memset(g_segmentCount, 0, sizeof(g_segmentCount));
    float prev[2], prevDepth = 0;
    bool prevInFront = false;
    for (int i = 0; i < count; i++) {
        float clip[4];
        bool inFront = Camera_Clip(g_border[i], clip) && clip[3] > 1e-4f;
        float q[2] = { 0, 0 };
        if (inFront) {
            q[0] = (clip[0] / clip[3] + 1) * 0.5f * w;
            q[1] = (1 - clip[1] / clip[3]) * 0.5f * h;
        }
        if (inFront && prevInFront) {
            float depth = (clip[3] + prevDepth) * 0.5f;
            float fade = (BORDER_FADE_FAR_M - depth) / (BORDER_FADE_FAR_M - BORDER_FADE_NEAR_M);
            int step = (int)(fminf(fade, 1.0f) * FADE_STEPS + 0.5f); // 0: not drawn
            if (step > 0) {
                float* s = g_segments[step - 1] + g_segmentCount[step - 1]++ * 4;
                s[0] = prev[0];
                s[1] = prev[1];
                s[2] = q[0];
                s[3] = q[1];
            }
        }
        prev[0] = q[0];
        prev[1] = q[1];
        prevDepth = clip[3];
        prevInFront = inFront;
    }
    for (int k = 0; k < FADE_STEPS; k++) {
        float rgba[4] = { kTerrainRed[0], kTerrainRed[1], kTerrainRed[2], kTerrainRed[3] * (k + 1) / FADE_STEPS };
        Panel2D_ScreenSegments(g_segments[k], g_segmentCount[k], 2.5f, rgba);
    }
}

// A muted, dusty red on the minimap (the menu strip's #953C37): it stands out
// on the map without glaring, and is darker than the game's bright red border.
static const float kMinimapRed[4] = { 0.584f, 0.235f, 0.216f, 0.85f };

// Render thread, right after the minimap's border (Minimap_SetDraw): the cut
// across the playable area (the strip's ends; the map's when there is no
// strip), as wide as the game's border (S * 0.004).
static void DrawOnMinimap()
{
    int strip = g_stripCount, count = g_borderCount;
    float cx, cy, k;
    if (!Active() || (strip < 2 && count < 2) || !Minimap_Frame(&cx, &cy, &k)) return;
    const float (*points)[3] = strip >= 2 ? g_strip : g_border;
    int n = strip >= 2 ? strip : count;
    // Every piece of the broken line.
    static float s_seg[BORDER_POINTS * 4]; // render thread
    for (int i = 0; i + 1 < n; i++) {
        float* seg = s_seg + i * 4;
        seg[0] = cx + points[i][0] * k;
        seg[1] = cy - points[i][2] * k;
        seg[2] = cx + points[i + 1][0] * k;
        seg[3] = cy - points[i + 1][2] * k;
    }
    Minimap_DrawSegments(s_seg, n - 1, fmaxf(2.0f, k * 80.0f), kMinimapRed);
}

static bool WantsOverlay()
{
    if (!Active()) g_borderCount = g_stripCount = g_signCount[0] = g_signCount[1] = 0; // the next session's map is read again
    ULONGLONG area = g_refusedAt, host = g_hostOnlyAt, refused = area > host ? area : host;
    return refused && GetTickCount64() - refused < REFUSED_NOTICE_MS;
}

// Render thread, in the ImGui frame: the notice when this PC's player was
// refused for building on the other half.
static void DrawOverlay()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ULONGLONG area = g_refusedAt, host = g_hostOnlyAt, refused = area > host ? area : host;
    if (refused && GetTickCount64() - refused < REFUSED_NOTICE_MS) {
        const char* text = host > area ? "Only the host changes the game's settings." : "That is the other republic's side of the border.";
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImVec2 at(size.x * 0.5f - ts.x * 0.5f, size.y * 0.12f);
        dl->AddRectFilled(ImVec2(at.x - 10, at.y - 6), ImVec2(at.x + ts.x + 10, at.y + ts.y + 6), IM_COL32(0, 0, 0, 0xB0));
        dl->AddText(at, IM_COL32(0xFF, 0xE0, 0x80, 0xFF), text);
    }
}

static void DrawPage()
{
    ImGui::TextWrapped("Each player builds their own republic on one map, split in two along a line the world's seed "
                       "picks, with bends every few kilometres: both halves get the same playable area, and as even a share of the border crossings and "
                       "resources as the map allows. A red line on the map and the minimap shows it.");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1.0f),
                       "In development: buildings and networks stay on your half; each republic has its own money.");
    double rub0, usd0, rub1, usd1;
    if (Wallets_Read(&rub0, &usd0, &rub1, &usd1)) {
        ImGui::Spacing();
        ImGui::Text("East: %.0f rubles, %.0f dollars", rub0, usd0);
        ImGui::Text("West: %.0f rubles, %.0f dollars", rub1, usd1);
    }
}

static const RmpMode kRepublics = {
    "Republics",
    "Each player their own republic, 2 players",
    "One map split in two - in development",
    DrawPage,
    nullptr, // frame
    WantsOverlay,
    DrawOverlay,
};

void Republics_Bind(const ModHost* host)
{
    ModHost_Bind(host);
    g_showSigns = Profile_GetInt("overlay", "border_signs", 1) != 0;
    Commands_SetAreaRule(AreaRule);
    Commands_SetGlobalRule(GlobalRule);
    Commands_SetBorderRule(BorderRule);
    Wallets_SetHalfRule(OwnerHalf);
    CitizenStats_SetHalfRule(OwnerHalf);
    Commands_SetSpawned(SpawnedCustoms);
    ModSave_Add("republics", WriteCutSection, ReadCutSection);
    Determinism_AddFrameObserver(SampleBorder);
    Determinism_AddStepObserver(SampleBorder);
    SimPatches_SetUnderUi(DrawUnderUi);
    BorderDraw_SetAfter(DrawAfterBorder);
    Minimap_SetDraw(DrawOnMinimap);
    Modes_SetRepublics(&kRepublics);
}
