// SPDX-License-Identifier: GPL-3.0-only
// See game_assets.h.
#include "game_assets.h"
#include "ui_log.h"
#include "ui_text.h"
#include "../game/imgui_hook.h" // ImGuiHook_Device
#include "imgui_internal.h"     // ImTextCharFromUtf8
#include <d3d11.h>
#include <wincodec.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    ImVec2 size;
};

struct Glyph {
    float x, y, w, h;          // in the page, pixels
    float xoff, yoff, advance; // at the font's own size
    int page;
};

struct Font {
    bool tried = false, ok = false;
    float lineHeight = 0;      // the font's own pixel size
    std::vector<Texture> pages;
    std::unordered_map<unsigned, Glyph> glyphs;
    std::unordered_map<unsigned long long, float> kerning; // (first << 32 | second) -> amount
};

static std::unordered_map<std::string, Texture> g_textures;
static Font g_fonts[GAME_FONT_COUNT];
static const char* const kFontFiles[GAME_FONT_COUNT] = { "square", "buran", "arial" };

// Wide: the game may sit in a folder with a non-ASCII name.
static std::wstring GamePath(const char* rel)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    std::wstring r(rel, rel + strlen(rel)); // our relative paths are ASCII
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"media_soviet\\" + r;
}

static bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size : 0);
    bool ok = size > 0 && fread(out.data(), 1, size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static ID3D11ShaderResourceView* MakeSrv(DXGI_FORMAT format, UINT w, UINT h, const void* pixels, UINT pitch)
{
    ID3D11Device* dev = ImGuiHook_Device();
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { pixels, pitch, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, &data, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

// The first mip of a DDS: DXT1/3/5 or uncompressed 32-bit BGRA.
static bool LoadDds(const std::vector<unsigned char>& d, Texture& out)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    UINT h = *(const UINT*)(d.data() + 12), w = *(const UINT*)(d.data() + 16);
    UINT pfFlags = *(const UINT*)(d.data() + 80);
    const char* fourCC = (const char*)d.data() + 84;
    DXGI_FORMAT format;
    UINT pitch;
    size_t bytes;
    if (pfFlags & 0x4) { // DDPF_FOURCC
        UINT block;
        if (!memcmp(fourCC, "DXT1", 4)) format = DXGI_FORMAT_BC1_UNORM, block = 8;
        else if (!memcmp(fourCC, "DXT3", 4)) format = DXGI_FORMAT_BC2_UNORM, block = 16;
        else if (!memcmp(fourCC, "DXT5", 4)) format = DXGI_FORMAT_BC3_UNORM, block = 16;
        else return false;
        pitch = ((w + 3) / 4) * block;
        bytes = (size_t)pitch * ((h + 3) / 4);
    } else if (*(const UINT*)(d.data() + 88) == 32) { // RGB bit count
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        pitch = w * 4;
        bytes = (size_t)pitch * h;
    } else {
        return false;
    }
    if (d.size() < 128 + bytes) return false;
    out.srv = MakeSrv(format, w, h, d.data() + 128, pitch);
    out.size = ImVec2((float)w, (float)h);
    return out.srv != nullptr;
}

static bool LoadWic(const std::wstring& path, Texture& out)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the render thread may have COM already
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom));
    if (ok) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        std::vector<unsigned char> pixels((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()));
        if (ok) out.srv = MakeSrv(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, pixels.data(), w * 4);
        out.size = ImVec2((float)w, (float)h);
        ok = ok && out.srv;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

ImTextureID GameAssets_Texture(const char* path, ImVec2* size)
{
    auto it = g_textures.find(path);
    if (it == g_textures.end()) {
        Texture t;
        std::wstring full = GamePath(path);
        bool dds = full.size() > 4 && _wcsicmp(full.c_str() + full.size() - 4, L".dds") == 0;
        std::vector<unsigned char> bytes;
        bool ok = dds ? ReadFileBytes(full, bytes) && LoadDds(bytes, t) : LoadWic(full, t);
        if (!ok) {
            char line[MAX_PATH + 64];
            snprintf(line, sizeof(line), "The game's texture %s could not be loaded", path);
            UiLog_Add(line);
        }
        it = g_textures.emplace(path, t).first; // a failure is kept too: tried once
    }
    if (size) *size = it->second.size;
    return (ImTextureID)it->second.srv;
}

// BMFont binary v3: blocks of (type byte, uint32 size): 2 common, 3 pages,
// 4 chars (20 bytes each), 5 kerning pairs (10 bytes each).
static Font* GetFont(GameFont id)
{
    Font& f = g_fonts[id];
    if (f.tried) return f.ok ? &f : nullptr;
    f.tried = true;
    std::vector<unsigned char> d;
    std::string dir = "fontsbitmap\\data\\";
    // The menu's font follows the language, as the game's own menu does:
    // "Square" has no Cyrillic letters, "Buran" has them.
    const char* file = id == GAME_FONT_SQUARE && UiText_IsCyrillic() ? "buran" : kFontFiles[id];
    if (!ReadFileBytes(GamePath((dir + file + ".fnt").c_str()), d) || d.size() < 4 ||
        memcmp(d.data(), "BMF\x03", 4) != 0)
        return nullptr;
    for (size_t i = 4; i + 5 <= d.size();) {
        unsigned char type = d[i];
        UINT n = *(const UINT*)(d.data() + i + 1);
        const unsigned char* b = d.data() + i + 5;
        if (i + 5 + n > d.size()) break;
        if (type == 2 && n >= 2) {
            f.lineHeight = *(const unsigned short*)b;
        } else if (type == 3) {
            const char* end = (const char*)b + n;
            for (const char* p = (const char*)b; p < end && *p; p += strnlen(p, end - p) + 1) {
                std::string page(p, strnlen(p, end - p));
                page = page.substr(0, page.find_last_of('.')) + ".dds"; // the pages ship as DDS
                ImVec2 size;
                ImTextureID tex = GameAssets_Texture((dir + page).c_str(), &size);
                f.pages.push_back(Texture{ (ID3D11ShaderResourceView*)tex, size });
            }
        } else if (type == 4) {
            for (UINT k = 0; k + 20 <= n; k += 20) {
                const unsigned char* c = b + k;
                Glyph g;
                g.x = *(const unsigned short*)(c + 4);
                g.y = *(const unsigned short*)(c + 6);
                g.w = *(const unsigned short*)(c + 8);
                g.h = *(const unsigned short*)(c + 10);
                g.xoff = *(const short*)(c + 12);
                g.yoff = *(const short*)(c + 14);
                g.advance = *(const short*)(c + 16);
                g.page = c[18];
                f.glyphs[*(const UINT*)c] = g;
            }
        } else if (type == 5) {
            for (UINT k = 0; k + 10 <= n; k += 10)
                f.kerning[((unsigned long long)*(const UINT*)(b + k) << 32) | *(const UINT*)(b + k + 4)] =
                    *(const short*)(b + k + 8);
        }
        i += 5 + n;
    }
    f.ok = f.lineHeight > 0 && !f.pages.empty() && f.pages[0].srv && !f.glyphs.empty(); // else ImGui's font
    if (!f.ok) UiLog_Add("A game font could not be loaded - the page uses ImGui's font");
    return f.ok ? &f : nullptr;
}

// Walks the text at a pixel height, calling emit(font, glyph, x, scale) per
// glyph, x in pixels. A letter the font lacks comes from Arial (2229 glyphs:
// accents, Cyrillic) at the same height, so no letter of a language is lost.
template <typename Emit>
static float Walk(const Font& f, float height, const char* text, Emit emit)
{
    const Font* fallback = &f != &g_fonts[GAME_FONT_ARIAL] ? GetFont(GAME_FONT_ARIAL) : nullptr;
    float x = 0;
    unsigned prev = 0;
    for (const char* p = text; *p;) {
        unsigned c;
        int used = ImTextCharFromUtf8(&c, p, nullptr);
        p += used > 0 ? used : 1;
        const Font* use = &f;
        auto g = f.glyphs.find(c);
        const Glyph* glyph = g != f.glyphs.end() ? &g->second : nullptr;
        if (!glyph && fallback) {
            auto h = fallback->glyphs.find(c);
            if (h != fallback->glyphs.end()) {
                use = fallback;
                glyph = &h->second;
            }
        }
        if (!glyph) {
            g = f.glyphs.find('?');
            if (g == f.glyphs.end()) continue;
            glyph = &g->second;
        }
        float scale = height / use->lineHeight;
        auto k = use->kerning.find(((unsigned long long)prev << 32) | c);
        if (k != use->kerning.end()) x += k->second * scale;
        emit(*use, *glyph, x, scale);
        x += glyph->advance * scale;
        prev = c;
    }
    return x;
}

ImVec2 GameText_Size(GameFont font, float height, const char* text)
{
    Font* f = GetFont(font);
    if (!f) return ImGui::CalcTextSize(text);
    return ImVec2(Walk(*f, height, text, [](const Font&, const Glyph&, float, float) {}), height);
}

void GameText_Draw(ImDrawList* drawList, GameFont font, float height, ImVec2 pos, ImU32 color, const char* text)
{
    Font* f = GetFont(font);
    if (!f) {
        drawList->AddText(pos, color, text);
        return;
    }
    Walk(*f, height, text, [&](const Font& use, const Glyph& g, float x, float s) {
        if (g.page >= (int)use.pages.size() || !use.pages[g.page].srv || g.w <= 0 || g.h <= 0) return;
        const Texture& page = use.pages[g.page];
        ImVec2 a(pos.x + x + g.xoff * s, pos.y + g.yoff * s);
        ImVec2 b(a.x + g.w * s, a.y + g.h * s);
        drawList->AddImage((ImTextureID)page.srv, a, b, ImVec2(g.x / page.size.x, g.y / page.size.y),
                           ImVec2((g.x + g.w) / page.size.x, (g.y + g.h) / page.size.y), color);
    });
}
