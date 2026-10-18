#include "SignText.h"

#include "config.h"
#include "decompressor.h"
#include "fileutils.h"
#include "include/stb_image.h"
#include "include/stb_image_write.h"
#include "JarReader.h"
#include "texture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numbers>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Vanilla sign text. Per-component colours and styles are ignored; the sign dye
// colour and the glowing flag paint the whole side. translate/score/selector
// components are not resolved — a "translate" key is copied literally when the
// component has no "text". unihex, ttf and legacy_unicode providers are skipped,
// so glyphs that exist only there are omitted.
//
// Hanging signs use the block entity's line height of 9 (standing and wall signs
// use 10). The bake hash includes that line height and the max line width so a
// hanging layout cannot reuse a standing texture.

namespace {

constexpr int kNbtTextDataVersion = 4325;
constexpr int kBlackGlowOutline = 0xF0EBCC;

enum class SignKind { None, Standing, Wall, Hanging, WallHanging };
enum class StringMode { Json, Literal, Heuristic };

struct FontImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

struct Glyph {
    int image = -1;
    int srcX = 0;
    int srcY = 0;
    int cellW = 0;
    int cellH = 0;
    float scale = 1.0f;
    int advance = 0;
    int ascent = 7;
};

struct FontLib {
    bool ok = false;
    std::vector<FontImage> images;
    std::unordered_map<uint32_t, Glyph> glyphs;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct PlacedGlyph {
    const Glyph* glyph = nullptr;
    float penX = 0.0f;
    float lineY = 0.0f;
};

FontLib gFont;
std::once_flag gFontOnce;
int gDataVersion = -1;
bool gDataVersionKnown = false;
std::once_flag gDataVersionOnce;
std::mutex gBakeMutex;
std::unordered_set<uint64_t> gBaked;

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<uint32_t> Codepoints(const std::string& text) {
    std::vector<uint32_t> out;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = bytes[i];
        uint32_t cp = c;
        size_t len = 1;
        if ((c & 0x80u) == 0) {
            cp = c;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < n) {
            cp = (static_cast<uint32_t>(c & 0x1Fu) << 6) | (bytes[i + 1] & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < n) {
            cp = (static_cast<uint32_t>(c & 0x0Fu) << 12) |
                 (static_cast<uint32_t>(bytes[i + 1] & 0x3Fu) << 6) |
                 (bytes[i + 2] & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < n) {
            cp = (static_cast<uint32_t>(c & 0x07u) << 18) |
                 (static_cast<uint32_t>(bytes[i + 1] & 0x3Fu) << 12) |
                 (static_cast<uint32_t>(bytes[i + 2] & 0x3Fu) << 6) |
                 (bytes[i + 3] & 0x3Fu);
            len = 4;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string TagString(const NbtTagPtr& tag) {
    if (!tag || tag->type != TagType::STRING) return {};
    return std::string(tag->payload.begin(), tag->payload.end());
}

bool TagFlag(const NbtTagPtr& tag) {
    if (!tag || tag->payload.empty()) return false;
    if (tag->type == TagType::BYTE) return static_cast<unsigned char>(tag->payload[0]) != 0;
    if (tag->type == TagType::SHORT && tag->payload.size() >= 2) return bytesToShort(tag->payload) != 0;
    if (tag->type == TagType::INT && tag->payload.size() >= 4) return bytesToInt(tag->payload) != 0;
    return false;
}

int DyeTextColor(const std::string& name) {
    const std::string key = ToLower(name);
    static const std::pair<const char*, int> kColors[] = {
        {"white", 16777215},
        {"orange", 16738335},
        {"magenta", 16711935},
        {"light_blue", 10141901},
        {"yellow", 16776960},
        {"lime", 12582656},
        {"pink", 16738740},
        {"gray", 8421504},
        {"light_gray", 13882323},
        {"cyan", 65535},
        {"purple", 10494192},
        {"blue", 255},
        {"brown", 9127187},
        {"green", 65280},
        {"red", 16711680},
        {"black", 0},
    };
    for (const auto& entry : kColors) {
        if (key == entry.first) return entry.second;
    }
    return 0;
}

int DarkChannel(int channel) {
    return static_cast<int>(static_cast<double>(channel) * 0.4);
}

int DarkenRgb(int rgb) {
    const int r = DarkChannel((rgb >> 16) & 255);
    const int g = DarkChannel((rgb >> 8) & 255);
    const int b = DarkChannel(rgb & 255);
    return (r << 16) | (g << 8) | b;
}

// SignRenderer.getDarkColor. Black glowing text uses the cream outline constant.
int DarkColor(int textRgb, bool glowing) {
    if (textRgb == 0 && glowing) return kBlackGlowOutline;
    return DarkenRgb(textRgb);
}

std::string FlattenJson(const nlohmann::json& node) {
    if (node.is_string()) return node.get<std::string>();
    if (node.is_array()) {
        std::string out;
        for (const auto& child : node) out += FlattenJson(child);
        return out;
    }
    if (node.is_object()) {
        std::string out;
        if (node.contains("text") && node["text"].is_string()) out += node["text"].get<std::string>();
        else if (node.contains("translate") && node["translate"].is_string())
            out += node["translate"].get<std::string>();
        if (node.contains("extra")) out += FlattenJson(node["extra"]);
        return out;
    }
    return {};
}

std::string ParseJsonText(const std::string& raw) {
    if (raw.empty()) return {};
    try {
        return FlattenJson(nlohmann::json::parse(raw));
    } catch (const std::exception&) {
        return raw;
    }
}

std::string DecodeString(const std::string& raw, StringMode mode) {
    if (mode == StringMode::Literal) return raw;
    if (mode == StringMode::Json) return ParseJsonText(raw);
    if (!raw.empty() && (raw[0] == '"' || raw[0] == '{' || raw[0] == '[')) return ParseJsonText(raw);
    return raw;
}

std::string FlattenNbtComponent(const NbtTagPtr& tag) {
    if (!tag) return {};
    if (tag->type == TagType::STRING) return TagString(tag);
    if (tag->type == TagType::LIST) {
        std::string out;
        for (const auto& child : tag->children) out += FlattenNbtComponent(child);
        return out;
    }
    if (tag->type != TagType::COMPOUND) return {};
    std::string out;
    const NbtTagPtr text = getChildByName(tag, "text");
    if (text && text->type == TagType::STRING) {
        out += TagString(text);
    } else {
        const NbtTagPtr translate = getChildByName(tag, "translate");
        if (translate && translate->type == TagType::STRING) out += TagString(translate);
    }
    const NbtTagPtr extra = getChildByName(tag, "extra");
    if (extra) out += FlattenNbtComponent(extra);
    return out;
}

std::string DecodeMessage(const NbtTagPtr& tag, StringMode mode) {
    if (!tag) return {};
    if (tag->type == TagType::STRING) return DecodeString(TagString(tag), mode);
    if (tag->type == TagType::COMPOUND || tag->type == TagType::LIST) return FlattenNbtComponent(tag);
    return {};
}

void ReadFourLines(const NbtTagPtr& messages, StringMode mode, std::array<std::string, 4>& lines) {
    lines = {"", "", "", ""};
    if (!messages || messages->type != TagType::LIST) return;
    const size_t count = std::min<size_t>(4, messages->children.size());
    for (size_t i = 0; i < count; ++i) lines[i] = DecodeMessage(messages->children[i], mode);
}

bool SideHasText(const std::array<std::string, 4>& lines) {
    for (const auto& line : lines) {
        if (!line.empty()) return true;
    }
    return false;
}

std::string ExeDir() {
    static std::string dir;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t buffer[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) return;
        const std::string exePath = wstring_to_string(std::wstring(buffer));
        const size_t pos = exePath.find_last_of("\\/");
        dir = (pos == std::string::npos) ? std::string() : exePath.substr(0, pos);
    });
    return dir;
}

void ReadDataVersion() {
    gDataVersion = -1;
    gDataVersionKnown = false;
    if (config.worldPath.empty()) return;
    const std::string path = JoinPath(config.worldPath, "level.dat");
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::vector<char> packed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (packed.empty()) return;
    std::vector<char> raw;
    if (!DecompressGzip(packed, raw) || raw.empty()) raw = std::move(packed);
    NbtTagPtr root;
    try {
        size_t index = 0;
        root = readTag(raw, index);
    } catch (const std::exception&) {
        return;
    }
    if (!root || root->type != TagType::COMPOUND) return;
    NbtTagPtr version = getChildByName(root, "DataVersion");
    if (!version) {
        const NbtTagPtr data = getChildByName(root, "Data");
        if (data && data->type == TagType::COMPOUND) version = getChildByName(data, "DataVersion");
    }
    if (!version || version->type != TagType::INT || version->payload.size() < 4) return;
    gDataVersion = bytesToInt(version->payload);
    gDataVersionKnown = true;
}

StringMode MessageStringMode() {
    std::call_once(gDataVersionOnce, ReadDataVersion);
    if (!gDataVersionKnown) return StringMode::Heuristic;
    if (gDataVersion >= kNbtTextDataVersion) return StringMode::Literal;
    return StringMode::Json;
}

std::string ProviderType(const nlohmann::json& provider) {
    if (!provider.contains("type") || !provider["type"].is_string()) return {};
    std::string type = provider["type"].get<std::string>();
    const size_t colon = type.rfind(':');
    if (colon != std::string::npos) type = type.substr(colon + 1);
    return type;
}

bool IncludeInDefaultFont(const nlohmann::json& provider) {
    if (!provider.contains("filter") || !provider["filter"].is_object()) return true;
    const auto& filter = provider["filter"];
    if (filter.contains("uniform") && filter["uniform"].is_boolean() && filter["uniform"].get<bool>()) return false;
    return true;
}

void SplitResource(const std::string& id, std::string& ns, std::string& path) {
    ns = "minecraft";
    path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    if (path.rfind("textures/", 0) == 0) path = path.substr(9);
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0) path.resize(path.size() - 4);
}

std::string FontJsonPath(const std::string& id) {
    std::string ns = "minecraft";
    std::string path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) path += ".json";
    return "assets/" + ns + "/font/" + path;
}

bool LoadFontImage(JarReader& jar, const std::string& ns, const std::string& texPath, FontImage& image) {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    if (LoadTexturePixels(ns, texPath, pixels, width, height) && width > 0 && height > 0) {
        image.width = width;
        image.height = height;
        image.pixels = std::move(pixels);
        return true;
    }
    const std::vector<unsigned char> file =
        jar.getBinaryFileContent("assets/" + ns + "/textures/" + texPath + ".png");
    if (file.empty()) return false;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) stbi_image_free(decoded);
        return false;
    }
    image.width = width;
    image.height = height;
    image.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(decoded);
    return true;
}

int ActualGlyphWidth(const FontImage& image, int srcX, int srcY, int cellW, int cellH) {
    for (int x = cellW - 1; x >= 0; --x) {
        for (int y = 0; y < cellH; ++y) {
            const int px = srcX + x;
            const int py = srcY + y;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const size_t index = (static_cast<size_t>(py) * image.width + px) * 4 + 3;
            if (image.pixels[index] != 0) return x + 1;
        }
    }
    return 0;
}

void AddBitmapProvider(FontLib& font, JarReader& jar, const nlohmann::json& provider) {
    if (!provider.contains("file") || !provider["file"].is_string()) return;
    if (!provider.contains("chars") || !provider["chars"].is_array() || provider["chars"].empty()) return;
    if (!provider.contains("ascent") || !provider["ascent"].is_number()) return;

    int height = 8;
    if (provider.contains("height") && provider["height"].is_number()) height = provider["height"].get<int>();
    const int ascent = provider["ascent"].get<int>();
    if (height <= 0) return;

    std::vector<std::vector<uint32_t>> rows;
    rows.reserve(provider["chars"].size());
    size_t columns = 0;
    for (const auto& row : provider["chars"]) {
        if (!row.is_string()) return;
        rows.push_back(Codepoints(row.get<std::string>()));
        if (columns == 0) columns = rows.back().size();
        if (rows.back().size() != columns || columns == 0) return;
    }
    if (rows.empty() || columns == 0) return;

    std::string ns;
    std::string texPath;
    SplitResource(provider["file"].get<std::string>(), ns, texPath);
    FontImage image;
    if (!LoadFontImage(jar, ns, texPath, image)) return;
    const int cellW = image.width / static_cast<int>(columns);
    const int cellH = image.height / static_cast<int>(rows.size());
    if (cellW <= 0 || cellH <= 0) return;
    const float scale = static_cast<float>(height) / static_cast<float>(cellH);

    const int imageIndex = static_cast<int>(font.images.size());
    font.images.push_back(std::move(image));
    const FontImage& stored = font.images.back();

    for (size_t row = 0; row < rows.size(); ++row) {
        for (size_t col = 0; col < columns; ++col) {
            const uint32_t cp = rows[row][col];
            if (cp == 0 || font.glyphs.find(cp) != font.glyphs.end()) continue;
            const int srcX = static_cast<int>(col) * cellW;
            const int srcY = static_cast<int>(row) * cellH;
            const int raw = ActualGlyphWidth(stored, srcX, srcY, cellW, cellH);
            if (raw <= 0) continue;
            const float product = static_cast<float>(raw) * scale;
            Glyph glyph;
            glyph.image = imageIndex;
            glyph.srcX = srcX;
            glyph.srcY = srcY;
            glyph.cellW = cellW;
            glyph.cellH = cellH;
            glyph.scale = scale;
            glyph.advance = static_cast<int>(0.5 + static_cast<double>(product)) + 1;
            glyph.ascent = ascent;
            font.glyphs.emplace(cp, glyph);
        }
    }
}

void AddSpaceProvider(FontLib& font, const nlohmann::json& provider) {
    if (!provider.contains("advances") || !provider["advances"].is_object()) return;
    for (auto it = provider["advances"].begin(); it != provider["advances"].end(); ++it) {
        if (!it.value().is_number()) continue;
        const std::vector<uint32_t> cps = Codepoints(it.key());
        if (cps.empty() || cps[0] == 0) continue;
        if (font.glyphs.find(cps[0]) != font.glyphs.end()) continue;
        Glyph glyph;
        glyph.advance = it.value().get<int>();
        font.glyphs.emplace(cps[0], glyph);
    }
}

void LoadFontProviders(FontLib& font, JarReader& jar, const nlohmann::json& root, std::unordered_set<std::string>& seen, int depth);

void LoadFontFile(FontLib& font, JarReader& jar, const std::string& jarPath, std::unordered_set<std::string>& seen, int depth) {
    if (depth > 8 || !seen.insert(jarPath).second) return;
    const std::string content = jar.getFileContent(jarPath);
    if (content.empty()) return;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(content);
    } catch (const std::exception&) {
        return;
    }
    LoadFontProviders(font, jar, root, seen, depth);
}

void LoadFontProviders(FontLib& font, JarReader& jar, const nlohmann::json& root, std::unordered_set<std::string>& seen, int depth) {
    if (root.is_object() && root.contains("providers") && root["providers"].is_array()) {
        for (const auto& provider : root["providers"]) {
            if (!provider.is_object() || !IncludeInDefaultFont(provider)) continue;
            const std::string type = ProviderType(provider);
            if (type == "reference") {
                if (provider.contains("id") && provider["id"].is_string())
                    LoadFontFile(font, jar, FontJsonPath(provider["id"].get<std::string>()), seen, depth + 1);
            } else if (type == "bitmap") {
                AddBitmapProvider(font, jar, provider);
            } else if (type == "space") {
                AddSpaceProvider(font, provider);
            }
        }
        return;
    }
    if (root.is_object() && root.contains("type")) {
        if (!IncludeInDefaultFont(root)) return;
        const std::string type = ProviderType(root);
        if (type == "bitmap") AddBitmapProvider(font, jar, root);
        else if (type == "space") AddSpaceProvider(font, root);
        else if (type == "reference" && root.contains("id") && root["id"].is_string())
            LoadFontFile(font, jar, FontJsonPath(root["id"].get<std::string>()), seen, depth + 1);
        return;
    }
    if (root.is_array()) {
        for (const auto& provider : root) {
            if (provider.is_object()) LoadFontProviders(font, jar, provider, seen, depth);
        }
    }
}

FontLib LoadFont() {
    FontLib font;
    if (config.jarPath.empty()) return font;
    JarReader jar(string_to_wstring(config.jarPath));
    if (!jar.open()) return font;
    std::unordered_set<std::string> seen;
    LoadFontFile(font, jar, "assets/minecraft/font/default.json", seen, 0);
    if (font.glyphs.find(32) == font.glyphs.end()) {
        Glyph space;
        space.advance = 4;
        font.glyphs.emplace(32, space);
    }
    font.ok = !font.glyphs.empty();
    if (!font.ok) std::cerr << "SignText: could not load assets/minecraft/font/default.json\n";
    return font;
}

const FontLib& Font() {
    std::call_once(gFontOnce, [] { gFont = LoadFont(); });
    return gFont;
}

uint64_t HashText(const std::array<std::string, 4>& lines, int rgb, bool glowing, int lineHeight, int maxWidth) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](unsigned char byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    auto mixString = [&mix](const std::string& text) {
        for (unsigned char c : text) mix(c);
        mix(0);
    };
    for (const auto& line : lines) mixString(line);
    mixString(std::to_string(rgb));
    mix(glowing ? 1 : 0);
    mixString(std::to_string(lineHeight));
    mixString(std::to_string(maxWidth));
    return hash;
}

std::string Hex64(uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016" PRIx64, value);
    return buffer;
}

std::vector<PlacedGlyph> LayoutLine(const std::string& text, float lineY, int maxWidth, const FontLib& font) {
    std::vector<const Glyph*> glyphs;
    float used = 0.0f;
    for (uint32_t cp : Codepoints(text)) {
        const auto it = font.glyphs.find(cp);
        if (it == font.glyphs.end()) continue;
        const float next = used + static_cast<float>(it->second.advance);
        if (!glyphs.empty() && next > static_cast<float>(maxWidth)) break;
        glyphs.push_back(&it->second);
        used = next;
    }
    const int width = static_cast<int>(std::ceil(static_cast<double>(used)));
    float pen = static_cast<float>(-(width / 2));
    std::vector<PlacedGlyph> placed;
    placed.reserve(glyphs.size());
    for (const Glyph* glyph : glyphs) {
        placed.push_back(PlacedGlyph{glyph, pen, lineY});
        pen += static_cast<float>(glyph->advance);
    }
    return placed;
}

void Plot(std::vector<unsigned char>& canvas, int width, int height, int x, int y,
    int srcR, int srcG, int srcB, int srcA, int rgb) {
    if (x < 0 || y < 0 || x >= width || y >= height || srcA <= 0) return;
    unsigned char* dest = canvas.data() + (static_cast<size_t>(y) * width + x) * 4;
    const int r = srcR * ((rgb >> 16) & 255) / 255;
    const int g = srcG * ((rgb >> 8) & 255) / 255;
    const int b = srcB * (rgb & 255) / 255;
    if (srcA >= 255 || dest[3] == 0) {
        dest[0] = static_cast<unsigned char>(r);
        dest[1] = static_cast<unsigned char>(g);
        dest[2] = static_cast<unsigned char>(b);
        dest[3] = static_cast<unsigned char>(srcA);
        return;
    }
    const int inv = 255 - srcA;
    dest[0] = static_cast<unsigned char>((r * srcA + dest[0] * inv) / 255);
    dest[1] = static_cast<unsigned char>((g * srcA + dest[1] * inv) / 255);
    dest[2] = static_cast<unsigned char>((b * srcA + dest[2] * inv) / 255);
    dest[3] = static_cast<unsigned char>(srcA + dest[3] * inv / 255);
}

void BlitGlyph(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY,
    const PlacedGlyph& placed, int dx, int dy, int rgb, const FontLib& font) {
    const Glyph& glyph = *placed.glyph;
    if (glyph.image < 0 || glyph.image >= static_cast<int>(font.images.size())) return;
    const FontImage& image = font.images[glyph.image];
    const float gw = static_cast<float>(glyph.cellW) * glyph.scale;
    const float gh = static_cast<float>(glyph.cellH) * glyph.scale;
    if (gw <= 0.0f || gh <= 0.0f) return;
    const float ox = placed.penX + static_cast<float>(dx);
    const float oy = placed.lineY + static_cast<float>(7 - glyph.ascent) + static_cast<float>(dy);
    const int x0 = static_cast<int>(std::floor(ox));
    const int y0 = static_cast<int>(std::floor(oy));
    const int x1 = static_cast<int>(std::ceil(ox + gw));
    const int y1 = static_cast<int>(std::ceil(oy + gh));
    const int canvasH = canvas.empty() || canvasW <= 0 ? 0 : static_cast<int>(canvas.size() / (static_cast<size_t>(canvasW) * 4));
    for (int fy = y0; fy < y1; ++fy) {
        for (int fx = x0; fx < x1; ++fx) {
            const float u = ((static_cast<float>(fx) + 0.5f) - ox) / gw;
            const float v = ((static_cast<float>(fy) + 0.5f) - oy) / gh;
            if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) continue;
            const int sx = std::min(glyph.cellW - 1, static_cast<int>(u * static_cast<float>(glyph.cellW)));
            const int sy = std::min(glyph.cellH - 1, static_cast<int>(v * static_cast<float>(glyph.cellH)));
            const int px = glyph.srcX + sx;
            const int py = glyph.srcY + sy;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const size_t index = (static_cast<size_t>(py) * image.width + px) * 4;
            const int a = image.pixels[index + 3];
            if (a == 0) continue;
            Plot(canvas, canvasW, canvasH, fx - originX, fy - originY,
                image.pixels[index], image.pixels[index + 1], image.pixels[index + 2], a, rgb);
        }
    }
}

bool BakeSide(const std::array<std::string, 4>& lines, int textRgb, bool glowing, int lineHeight, int maxWidth,
    std::string& hexOut, float& xMin, float& yMin, float& xMax, float& yMax) {
    const FontLib& font = Font();
    if (!font.ok) return false;

    std::vector<PlacedGlyph> placed;
    for (int i = 0; i < 4; ++i) {
        const float lineY = static_cast<float>(i * lineHeight - (4 * lineHeight / 2));
        auto line = LayoutLine(lines[i], lineY, maxWidth, font);
        placed.insert(placed.end(), line.begin(), line.end());
    }

    const int margin = glowing ? 1 : 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool any = false;
    for (const auto& stamp : placed) {
        if (stamp.glyph->image < 0) continue;
        const float gw = static_cast<float>(stamp.glyph->cellW) * stamp.glyph->scale;
        const float gh = static_cast<float>(stamp.glyph->cellH) * stamp.glyph->scale;
        const float top = stamp.lineY + static_cast<float>(7 - stamp.glyph->ascent);
        const int x0 = static_cast<int>(std::floor(stamp.penX)) - margin;
        const int y0 = static_cast<int>(std::floor(top)) - margin;
        const int x1 = static_cast<int>(std::ceil(stamp.penX + gw)) + margin;
        const int y1 = static_cast<int>(std::ceil(top + gh)) + margin;
        if (!any) {
            minX = x0;
            minY = y0;
            maxX = x1;
            maxY = y1;
            any = true;
        } else {
            minX = std::min(minX, x0);
            minY = std::min(minY, y0);
            maxX = std::max(maxX, x1);
            maxY = std::max(maxY, y1);
        }
    }
    if (!any || maxX <= minX || maxY <= minY) return false;

    const int canvasW = maxX - minX;
    const int canvasH = maxY - minY;
    if (canvasW > 4096 || canvasH > 4096) return false;
    std::vector<unsigned char> canvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);

    const int fill = glowing ? textRgb : DarkColor(textRgb, false);
    const int outline = DarkColor(textRgb, true);
    if (glowing) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                for (const auto& stamp : placed) {
                    BlitGlyph(canvas, canvasW, minX, minY, stamp, dx, dy, outline, font);
                }
            }
        }
    }
    for (const auto& stamp : placed) BlitGlyph(canvas, canvasW, minX, minY, stamp, 0, 0, fill, font);

    int cropX0 = canvasW;
    int cropY0 = canvasH;
    int cropX1 = 0;
    int cropY1 = 0;
    for (int y = 0; y < canvasH; ++y) {
        for (int x = 0; x < canvasW; ++x) {
            if (canvas[(static_cast<size_t>(y) * canvasW + x) * 4 + 3] == 0) continue;
            cropX0 = std::min(cropX0, x);
            cropY0 = std::min(cropY0, y);
            cropX1 = std::max(cropX1, x + 1);
            cropY1 = std::max(cropY1, y + 1);
        }
    }
    if (cropX1 <= cropX0 || cropY1 <= cropY0) return false;

    int outW = cropX1 - cropX0;
    int outH = cropY1 - cropY0;
    // objrender treats a PNG with height > width and height % width == 0 as an
    // animated strip and keeps only the top square. Pad the right edge so the
    // bake stays one frame. The quad grows by the same empty columns.
    if (outH > outW) outW = outH;
    std::vector<unsigned char> cropped(static_cast<size_t>(outW) * outH * 4, 0);
    for (int y = 0; y < cropY1 - cropY0; ++y) {
        const unsigned char* src = canvas.data() + (static_cast<size_t>(cropY0 + y) * canvasW + cropX0) * 4;
        unsigned char* dst = cropped.data() + static_cast<size_t>(y) * outW * 4;
        std::memcpy(dst, src, static_cast<size_t>(cropX1 - cropX0) * 4);
    }

    const uint64_t hash = HashText(lines, textRgb, glowing, lineHeight, maxWidth);
    const std::string hex = Hex64(hash);
    const std::string exe = ExeDir();
    if (exe.empty()) return false;
    const std::string relative = "textures/minecraft/sign_text/" + hex + ".png";
    const std::string full = JoinPath({exe, "textures", "minecraft", "sign_text", hex + ".png"});

    {
        std::lock_guard<std::mutex> lock(gBakeMutex);
        if (!gBaked.count(hash)) {
            try {
                std::filesystem::create_directories(std::filesystem::path(full).parent_path());
            } catch (const std::exception&) {
                return false;
            }
            if (stbi_write_png(full.c_str(), outW, outH, 4, cropped.data(), outW * 4) == 0) return false;
            RegisterTexture("minecraft", "sign_text/" + hex, relative);
            gBaked.insert(hash);
        }
    }

    hexOut = hex;
    xMin = static_cast<float>(minX + cropX0);
    yMin = static_cast<float>(minY + cropY0);
    xMax = xMin + static_cast<float>(outW);
    yMax = yMin + static_cast<float>(outH);
    return true;
}

Vec3 RotY(Vec3 p, float degrees) {
    const float rad = degrees * (std::numbers::pi_v<float> / 180.0f);
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return Vec3{c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
}

Vec3 TransformPoint(Vec3 p, float scale, const Vec3& offset, bool back, bool wall, bool hanging, float angle, float yBase) {
    p.x *= scale;
    p.y *= -scale;
    p.z *= scale;
    p.x += offset.x;
    p.y += offset.y;
    p.z += offset.z;
    if (back) p = RotY(p, 180.0f);
    // Hanging signs (ceiling and wall) share one local shift. Only a standing
    // wall sign uses the extra -Z that pulls the text onto the wall board.
    if (hanging) {
        p.y += -0.3125f;
    } else if (wall) {
        p.y += -0.3125f;
        p.z += -0.4375f;
    }
    p = RotY(p, -angle);
    p.x += 0.5f;
    p.y += yBase;
    p.z += 0.5f;
    return p;
}

void AddQuad(ModelData& out, const Vec3 corners[4], int materialIndex) {
    const int vbase = static_cast<int>(out.vertices.size() / 3);
    const int ubase = static_cast<int>(out.uvCoordinates.size() / 2);
    for (int i = 0; i < 4; ++i) {
        out.vertices.push_back(corners[i].x);
        out.vertices.push_back(corners[i].y);
        out.vertices.push_back(corners[i].z);
    }
    // v = 1 is the top of the PNG. objrender treats vt v = 0 as the bottom row.
    const float uvs[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (const auto& uv : uvs) {
        out.uvCoordinates.push_back(uv[0]);
        out.uvCoordinates.push_back(uv[1]);
    }
    Face face{};
    face.vertexIndices = {vbase, vbase + 1, vbase + 2, vbase + 3};
    face.uvIndices = {ubase, ubase + 1, ubase + 2, ubase + 3};
    face.materialIndex = materialIndex;
    face.faceDirection = FaceType::DO_NOT_CULL;
    face.tintIndex = -1;
    face.bothSided = false;
    face.thinEdge = false;
    out.faces.push_back(face);
}

int AddMaterial(ModelData& out, const std::string& name, const std::string& texturePath) {
    for (size_t i = 0; i < out.materials.size(); ++i) {
        if (out.materials[i].name == name) return static_cast<int>(i);
    }
    Material material;
    material.name = name;
    material.texturePath = texturePath;
    material.tintIndex = -1;
    material.type = NORMAL;
    material.aspectRatio = 1.0f;
    out.materials.push_back(material);
    return static_cast<int>(out.materials.size() - 1);
}

SignKind Classify(const std::string& base) {
    if (base.size() >= 18 && base.compare(base.size() - 18, 18, "_wall_hanging_sign") == 0) return SignKind::WallHanging;
    if (base.size() >= 13 && base.compare(base.size() - 13, 13, "_hanging_sign") == 0) return SignKind::Hanging;
    if (base.size() >= 10 && base.compare(base.size() - 10, 10, "_wall_sign") == 0) return SignKind::Wall;
    if (base.size() >= 5 && base.compare(base.size() - 5, 5, "_sign") == 0) return SignKind::Standing;
    return SignKind::None;
}

std::unordered_map<std::string, std::string> ParseState(const std::string& state) {
    std::unordered_map<std::string, std::string> props;
    size_t i = 0;
    while (i < state.size()) {
        const size_t comma = state.find(',', i);
        const std::string part = state.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) props[part.substr(0, eq)] = part.substr(eq + 1);
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return props;
}

bool ReadSide(const NbtTagPtr& side, StringMode mode, std::array<std::string, 4>& lines, int& rgb, bool& glowing) {
    if (!side || side->type != TagType::COMPOUND) return false;
    ReadFourLines(getChildByName(side, "messages"), mode, lines);
    rgb = 0;
    const NbtTagPtr color = getChildByName(side, "color");
    if (color && color->type == TagType::STRING) rgb = DyeTextColor(TagString(color));
    glowing = TagFlag(getChildByName(side, "has_glowing_text"));
    return SideHasText(lines);
}

void EmitSide(ModelData& out, const std::array<std::string, 4>& lines, int textRgb, bool glowing,
    int lineHeight, int maxWidth, float scale, const Vec3& offset, bool back, bool wall, bool hanging,
    float angle, float yBase, int x, int y, int z) {
    std::string hex;
    float xMin = 0.0f;
    float yMin = 0.0f;
    float xMax = 0.0f;
    float yMax = 0.0f;
    if (!BakeSide(lines, textRgb, glowing, lineHeight, maxWidth, hex, xMin, yMin, xMax, yMax)) return;
    const std::string name = "minecraft:sign_text#" + hex;
    const std::string texture = "textures/minecraft/sign_text/" + hex + ".png";
    const int material = AddMaterial(out, name, texture);
    const Vec3 local[4] = {
        {xMin, yMin, 0.0f},
        {xMax, yMin, 0.0f},
        {xMax, yMax, 0.0f},
        {xMin, yMax, 0.0f},
    };
    Vec3 world[4];
    for (int i = 0; i < 4; ++i) {
        world[i] = TransformPoint(local[i], scale, offset, back, wall, hanging, angle, yBase);
        world[i].x += static_cast<float>(x);
        world[i].y += static_cast<float>(y);
        world[i].z += static_cast<float>(z);
    }
    AddQuad(out, world, material);
}

}  // namespace

bool BuildSignTextModel(const std::string& ns, const std::string& blockNameWithState,
    const NbtTagPtr& blockEntityNbt, int x, int y, int z, ModelData& out) {
    if (ns != "minecraft" || !blockEntityNbt || blockEntityNbt->type != TagType::COMPOUND) return false;

    std::string base = blockNameWithState;
    std::string state;
    const size_t bracket = base.find('[');
    if (bracket != std::string::npos) {
        state = base.substr(bracket + 1);
        base = base.substr(0, bracket);
        if (!state.empty() && state.back() == ']') state.pop_back();
    }
    const SignKind kind = Classify(base);
    if (kind == SignKind::None) return false;

    const auto props = ParseState(state);
    const bool wall = kind == SignKind::Wall || kind == SignKind::WallHanging;
    const bool hanging = kind == SignKind::Hanging || kind == SignKind::WallHanging;
    float angle = 0.0f;
    if (wall) {
        const auto facing = props.find("facing");
        const std::string face = facing == props.end() ? "north" : facing->second;
        if (face == "south") angle = 0.0f;
        else if (face == "west") angle = 90.0f;
        else if (face == "east") angle = 270.0f;
        else angle = 180.0f;
    } else {
        int rotation = 0;
        const auto it = props.find("rotation");
        if (it != props.end()) {
            try {
                rotation = std::stoi(it->second);
            } catch (const std::exception&) {
                rotation = 0;
            }
        }
        angle = static_cast<float>(rotation) * 22.5f;
    }

    const int lineHeight = hanging ? 9 : 10;
    const int maxWidth = hanging ? 60 : 90;
    const float scale = hanging ? (0.015625f * 0.9f) : (0.015625f * 0.6666667f);
    const float yBase = hanging ? 0.9375f : (0.75f * 0.6666667f);
    const Vec3 offset = hanging ? Vec3{0.0f, -0.32f, 0.073f} : Vec3{0.0f, 0.33333334f, 0.046666667f};

    const size_t facesBefore = out.faces.size();
    const NbtTagPtr front = getChildByName(blockEntityNbt, "front_text");
    if (front && front->type == TagType::COMPOUND) {
        const StringMode mode = MessageStringMode();
        std::array<std::string, 4> lines;
        int rgb = 0;
        bool glowing = false;
        if (ReadSide(front, mode, lines, rgb, glowing)) {
            EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, false, wall, hanging, angle, yBase, x, y, z);
        }
        const NbtTagPtr back = getChildByName(blockEntityNbt, "back_text");
        if (back && back->type == TagType::COMPOUND && ReadSide(back, mode, lines, rgb, glowing)) {
            EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, true, wall, hanging, angle, yBase, x, y, z);
        }
    } else {
        std::array<std::string, 4> lines;
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            lines[i] = ParseJsonText(TagString(getChildByName(blockEntityNbt, "Text" + std::to_string(i + 1))));
            if (!lines[i].empty()) any = true;
        }
        if (!any) return false;
        int rgb = 0;
        const NbtTagPtr color = getChildByName(blockEntityNbt, "Color");
        if (color && color->type == TagType::STRING) rgb = DyeTextColor(TagString(color));
        const bool glowing = TagFlag(getChildByName(blockEntityNbt, "GlowingText"));
        EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, false, wall, hanging, angle, yBase, x, y, z);
    }
    return out.faces.size() > facesBefore;
}
