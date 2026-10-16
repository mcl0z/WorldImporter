#include "objExporter.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <sstream>
#include <locale>
#include <limits>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
// 仅在Windows平台需要这些函数声明
extern "C" {
    __declspec(dllimport) unsigned long __stdcall GetLastError();
    __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
}
#define CP_UTF8 65001
#define MAX_PATH 260
// Windows平台使用sprintf_s
#define safe_sprintf sprintf_s
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
// 非Windows平台使用snprintf
#define safe_sprintf(dst, size, ...) snprintf(dst, size, __VA_ARGS__)
#endif

using namespace std::chrono;


// 跨平台获取可执行文件目录的实现
std::string getExecutableDir() {
    namespace fs = std::filesystem;
    fs::path exePath;
    
    try {
        // 获取可执行文件路径
#ifdef _WIN32
        // Windows平台
        wchar_t buffer[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        }
        std::wstring ws(buffer);
        size_t pos = ws.find_last_of(L"\\/");
        if (pos != std::wstring::npos) ws.resize(pos + 1);
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string ret(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &ret[0], len, nullptr, nullptr);
            return ret.substr(0, len - 1);
        }
        return ".";
#elif defined(__APPLE__)
        // macOS平台
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        exePath = fs::canonical(fs::path(buffer));
#else
        // Linux平台
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        buffer[len] = '\0';
        exePath = fs::path(buffer);
#endif
        // 获取父目录
        return exePath.parent_path().string() + "/";
    }
    catch (const std::exception& e) {
        std::cerr << "获取可执行文件目录失败: " << e.what() << std::endl;
        return fs::current_path().string() + "/"; // 失败时返回当前目录
    }
}

// 快速整数转字符串(正数版)
inline char* fast_itoa_positive(uint32_t value, char* ptr) {
    char* start = ptr;
    do {
        *ptr++ = '0' + (value % 10);
        value /= 10;
    } while (value > 0);
    std::reverse(start, ptr);
    return ptr;
}

// 快速整数转字符串(带符号)
inline char* fast_itoa(int value, char* ptr) {
    if (value == 0) {
        *ptr++ = '0';
        return ptr;
    }

    bool negative = value < 0;
    if (negative) {
        *ptr++ = '-';
        value = -value;
    }

    // 调用无符号整数的辅助函数
    ptr = fast_itoa_positive(static_cast<uint32_t>(value), ptr);

    return ptr;
}

// 快速浮点转字符串(固定6位小数)
inline char* fast_ftoa(float value, char* ptr) {
    // 非有限值(NaN/Inf):统一写 0,避开 floor()/整数转换的未定义行为
    if (!std::isfinite(value)) {
        *ptr++ = '0';
        return ptr;
    }
    // 超出定点缩放安全范围:走 snprintf 兜底,避免 int64 溢出
    if (std::fabs(value) >= 1.0e12f) {
        int written = snprintf(ptr, 64, "%.6f", static_cast<double>(value));
        return ptr + (written > 0 ? written : 0);
    }
    if (value == floor(value)) {  // 检查是否是整数
        return fast_itoa(static_cast<int>(value), ptr);
    }
    else {
        // 原有浮点数处理逻辑
        const int64_t scale = 1000000;
        bool negative = value < 0;
        if (negative) {
            *ptr++ = '-';
            value = -value;
        }

        int64_t scaled = static_cast<int64_t>(std::round(value * scale));
        int64_t integer_part = scaled / scale;
        int64_t fractional_part = scaled % scale;

        ptr = fast_itoa(static_cast<int>(integer_part), ptr);
        *ptr++ = '.';

        for (int i = 5; i >= 0; --i) {
            ptr[i] = '0' + (fractional_part % 10);
            fractional_part /= 10;
        }
        ptr += 6;

        return ptr;
    }
}

//——————————————导出.obj/.mtl方法—————————————

void createObjFileViaMemoryMapped(const ModelData& data, const std::string& objName, const std::string& mtlFileName = "") {
    std::string exeDir = getExecutableDir();
    std::string objFilePath = exeDir + objName + ".obj";
    std::string mtlFilePath = mtlFileName.empty() ? (objName + ".mtl") : (mtlFileName + ".mtl");

    // 提取模型名称(取路径最后一段;兼容 / 与 \ 分隔符)
    std::string modelName = objName;
    size_t sepPos = objName.find_last_of("/\\");
    if (sepPos != std::string::npos) {
        modelName = objName.substr(sepPos + 1);
    }

    // 面数据分组计算
    std::vector<std::vector<size_t>> materialGroups(data.materials.size());
    const size_t totalFaces = data.faces.size();
    for (size_t faceIdx = 0; faceIdx < totalFaces; ++faceIdx) {
        const int matIndex = data.faces[faceIdx].materialIndex;
        if (matIndex != -1 && matIndex < materialGroups.size()) {
            materialGroups[matIndex].push_back(faceIdx);
        }
    }

    // Grow on demand instead of reserving worst-case space for every model line.
    // This keeps large models proportional to their actual formatted size and
    // avoids formatting floats and counting face-index digits in a sizing pass.
    std::vector<char> buffer(64 * 1024);
    char* ptr = buffer.data();
    const auto ensureSpace = [&](size_t extra) {
        const size_t used = static_cast<size_t>(ptr - buffer.data());
        if (extra <= buffer.size() - used) return;
        if (extra > buffer.max_size() - used) {
            throw std::length_error("OBJ buffer exceeds maximum size");
        }
        const size_t grownSize = buffer.size() <= buffer.max_size() / 2
            ? buffer.size() * 2 : buffer.max_size();
        buffer.resize((std::max)(grownSize, used + extra));
        ptr = buffer.data() + used;
    };
    const auto appendFormatted = [&](const char* format, const auto&... args) {
        const int length = snprintf(nullptr, 0, format, args...);
        if (length < 0) {
            throw std::runtime_error("Unable to format OBJ header");
        }
        ensureSpace(static_cast<size_t>(length) + 1); // snprintf's terminating NUL
        ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), format, args...);
    };

    // A finite float has at most 39 integer digits; sign, decimal point, six
    // decimals and snprintf's NUL fit in 48 bytes. 64 also covers fast_ftoa's
    // integer/nonfinite branches and its 64-byte snprintf fallback unchanged.
    constexpr size_t floatSpace = 64;
    constexpr size_t indexDigits = std::numeric_limits<int>::digits10 + 1;
    constexpr size_t faceSpace = 2 + 4 * (2 * indexDigits + 2) + 1;

    // 开始填充缓冲区
    appendFormatted("mtllib %s\n", mtlFilePath.c_str());
    appendFormatted("o %s\n\n", modelName.c_str());
    appendFormatted("# Vertices (%zu)\n", data.vertices.size() / 3);
    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        ensureSpace(3 * floatSpace + 5); // "v " + x + ' ' + y + ' ' + z + '\n'
        memcpy(ptr, "v ", 2);
        ptr += 2;
        ptr = fast_ftoa(data.vertices[i], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.vertices[i + 1], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.vertices[i + 2], ptr);
        *ptr++ = '\n';
    }

    appendFormatted("\n# UVs (%zu)\n", data.uvCoordinates.size() / 2);
    for (size_t i = 0; i < data.uvCoordinates.size(); i += 2) {
        ensureSpace(2 * floatSpace + 5); // "vt " + u + ' ' + v + '\n'
        memcpy(ptr, "vt ", 3);
        ptr += 3;
        ptr = fast_ftoa(data.uvCoordinates[i], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.uvCoordinates[i + 1], ptr);
        *ptr++ = '\n';
    }

    appendFormatted("\n# Faces (%zu)\n", totalFaces);
    for (size_t matIndex = 0; matIndex < materialGroups.size(); ++matIndex) {
        const auto& faces = materialGroups[matIndex];
        if (faces.empty()) continue;
        appendFormatted("usemtl %s\n", data.materials[matIndex].name.c_str());
        for (const size_t faceIdx : faces) {
            ensureSpace(faceSpace); // "f " + four positive int/int pairs with spaces + '\n'
            memcpy(ptr, "f ", 2);
            ptr += 2;
            for (int i = 0; i < 4; ++i) {
                const int vIdx = data.faces[faceIdx].vertexIndices[i] + 1;
                const int uvIdx = data.faces[faceIdx].uvIndices[i] + 1;
                if (vIdx <= 0 || uvIdx <= 0) {
                    throw std::invalid_argument("Invalid vertex or UV index");
                }
                ptr = fast_itoa(vIdx, ptr);
                *ptr++ = '/';
                ptr = fast_itoa(uvIdx, ptr);
                *ptr++ = ' ';
            }
            *ptr++ = '\n';
        }
    }

    const size_t writtenSize = static_cast<size_t>(ptr - buffer.data());
    assert(writtenSize <= buffer.size());

    // 使用跨平台方式写入文件
    try {
#ifdef _WIN32
        // Windows平台使用标准文件IO
        std::ofstream file(objFilePath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("无法创建文件: " + objFilePath);
        }

        // 一次性写入全部数据
        file.write(buffer.data(), writtenSize);
        file.close();
#elif defined(__unix__) || defined(__APPLE__)
        // Unix/Linux/macOS平台
        int fd = open(objFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            throw std::runtime_error("无法创建文件: " + objFilePath + ", 错误: " + std::to_string(errno));
        }
        
        // Normally one write; retry interrupted/partial writes and cap requests
        // at ssize_t's limit for very large models. No mmap copy or sync flush.
        size_t offset = 0;
        while (offset < writtenSize) {
            const size_t count = (std::min)(writtenSize - offset,
                static_cast<size_t>((std::numeric_limits<ssize_t>::max)()));
            const ssize_t result = write(fd, buffer.data() + offset, count);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                const int error = result < 0 ? errno : EIO;
                close(fd);
                throw std::system_error(error, std::generic_category(), "无法写入文件: " + objFilePath);
            }
            offset += static_cast<size_t>(result);
        }
        if (close(fd) == -1) {
            throw std::system_error(errno, std::generic_category(), "无法关闭文件: " + objFilePath);
        }
#else
        // 其他平台回退到标准文件IO
        std::ofstream file(objFilePath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("无法创建文件: " + objFilePath);
        }
        
        // 一次性写入全部数据
        file.write(buffer.data(), writtenSize);
        file.close();
#endif
    }
    catch (const std::exception& e) {
        std::cerr << "写入OBJ文件时出错: " << e.what() << std::endl;
    }
}

// 创建 .obj 文件并写入内容
void createObjFile(const ModelData& data, const std::string& objName, const std::string& mtlFileName = "") {
    std::string exeDir = getExecutableDir();

    std::string objFilePath = exeDir + objName + ".obj";
    std::string mtlFilePath = mtlFileName.empty() ? (objName + ".mtl") : (mtlFileName + ".mtl");

    // 提取模型名称(取路径最后一段;兼容 / 与 \ 分隔符)
    std::string name = objName;
    size_t sepPos = objName.find_last_of("/\\");
    if (sepPos != std::string::npos) {
        name = objName.substr(sepPos + 1);
    }

    // 使用流缓冲区进行拼接,减少IO操作次数
    std::ostringstream oss;
    oss.imbue(std::locale::classic());

    // 写入文件头
    oss << "mtllib " << mtlFilePath << "\n";
    oss << "o " << name << "\n\n";
    // 写入顶点数据(每3个元素一个顶点)
    oss << "# Vertices (" << data.vertices.size() / 3 << ")\n";
    // 固定 6 位小数: 默认精度只有 6 位有效数字, 坐标上千时会把几何双面的微小偏移抹掉
    oss << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        oss << "v " << data.vertices[i] << " "
            << data.vertices[i + 1] << " "
            << data.vertices[i + 2] << "\n";
    }
    oss << std::defaultfloat;
    oss << "\n";

    // 写入UV坐标(每2个元素一个UV)
    oss << "# UVs (" << data.uvCoordinates.size() / 2 << ")\n";
    for (size_t i = 0; i < data.uvCoordinates.size(); i += 2) {
        oss << "vt " << data.uvCoordinates[i] << " "
            << data.uvCoordinates[i + 1] << "\n";
    }
    oss << "\n";

    // 按材质分组面(优化分组算法)
    std::vector<std::vector<size_t>> materialGroups(data.materials.size());
    const size_t totalFaces = data.faces.size();
    for (size_t faceIdx = 0; faceIdx < totalFaces; ++faceIdx) {
        const int matIndex = data.faces[faceIdx].materialIndex;
        if (matIndex != -1 && matIndex < materialGroups.size()) {
            materialGroups[matIndex].push_back(faceIdx);
        }
    }

    // 写入面数据(优化内存访问模式)
    oss << "# Faces (" << totalFaces << ")\n";
    for (size_t matIndex = 0; matIndex < materialGroups.size(); ++matIndex) {
        const auto& faces = materialGroups[matIndex];
        if (faces.empty()) continue;

        oss << "usemtl " << data.materials[matIndex].name << "\n";
        for (const size_t faceIdx : faces) {
            oss << "f ";
            for (int i = 0; i < 4; ++i) {
                const int vIdx = data.faces[faceIdx].vertexIndices[i] + 1;
                const int uvIdx = data.faces[faceIdx].uvIndices[i] + 1;
                oss << vIdx << "/" << uvIdx << " ";
            }
            oss << "\n";
        }
    }

    // 将缓冲区写入文件
    std::ofstream objFile(objFilePath, std::ios::binary); // 使用二进制模式提高写入速度
    if (objFile.is_open()) {
        objFile << oss.str();
        objFile.close();
    }
    else {
        std::cerr << "Error: Failed to create " << objFilePath << "\n";
    }
}

void CreateSharedMtlFile(std::unordered_map<std::string, std::string> uniqueMaterials, const std::string& mtlFileName, const std::unordered_map<std::string, TintResult>& uniqueTints) {
    std::string exeDir = getExecutableDir();
    std::string fullMtlPath = exeDir + mtlFileName + ".mtl";

    std::ofstream mtlFile(fullMtlPath);
    if (mtlFile.is_open()) {
        for (const auto& uM : uniqueMaterials) {
            const std::string& textureName = uM.first;
            std::string texturePath = uM.second;

            mtlFile << "newmtl " << textureName << "\n";

            // 处理材质类型
            if (texturePath == "None") {
                // LIGHT材质处理
                mtlFile << "Ns 200.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ka 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.900000 0.900000 0.900000\n";
                mtlFile << "Ke 0.900000 0.900000 0.900000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 2\n";
            }
            // 缺失纹理不写虚假的 .png 引用，避免 Blender 显示紫色材质。
            else if (texturePath.empty()) {
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            // 处理纯颜色材质(支持流体格式:color#r g b-流体名 和普通格式:color#r g b=)
            else if (texturePath.find("color#") != std::string::npos) {
                std::string colorStr;
                size_t pos = texturePath.find("-");
                size_t pos_deng = texturePath.find("=");
                if (pos != std::string::npos) {
                    // 流体材质格式,提取"-"前面的颜色部分
                    colorStr = texturePath.substr(std::string("color#").size(), pos - std::string("color#").size());
                }
                else if (pos_deng != std::string::npos) {
                    colorStr = texturePath.substr(std::string("color#").size(), pos_deng - std::string("color#").size());
                }
                else {
                    colorStr = "";
                }
                std::istringstream iss(colorStr);
                float r, g, b;
                if (iss >> r >> g >> b) {

                    mtlFile << "Kd " << std::fixed << std::setprecision(config.decimalPlaces)
                        << r << " " << g << " " << b << "\n";
                }
                else {
                    mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                    std::cerr << "Error: Invalid color format in '" << texturePath << "'\n";
                }
                // 共用普通材质参数
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            else {
                // 普通纹理材质处理
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";

                // 处理纹理路径
                if (mtlFileName.find("//") != std::string::npos) {
                    texturePath = "../" + texturePath;
                }
                if (texturePath.find(".png") == std::string::npos) {
                    texturePath += ".png";
                }
                mtlFile << "map_Kd " << texturePath << "\n";
                // Only emit map_d for a distinct opacity texture; Crafter uses map_Kd's alpha.
            }

            mtlFile << "\n";
        }
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFileName << std::endl;
    }

    CreateTintJsonFile(uniqueTints);
}

namespace {
    float SrgbChannelToLinear(uint32_t channel) {
        float s = static_cast<float>(channel & 0xFF) / 255.0f;
        return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
    }
}

// 输出 tint.json（材质名 -> {kind, color?}；无 kind 表示不上色），供 Blender 插件按接口接线
void CreateTintJsonFile(const std::unordered_map<std::string, TintResult>& uniqueTints) {
    if (uniqueTints.empty()) return;

    nlohmann::json root = nlohmann::json::object();
    for (const auto& entry : uniqueTints) {
        const TintResult& tint = entry.second;
        nlohmann::json value = nlohmann::json::object();
        if (tint.on()) {
            value["kind"] = TintKindName(tint.kind);
            if (tint.kind == TintKind::Fixed) {
                nlohmann::json color = nlohmann::json::array();
                color.push_back(SrgbChannelToLinear((tint.color >> 16) & 0xFF));
                color.push_back(SrgbChannelToLinear((tint.color >> 8) & 0xFF));
                color.push_back(SrgbChannelToLinear(tint.color & 0xFF));
                value["color"] = std::move(color);
            }
        }
        root[entry.first] = std::move(value);
    }

    std::string path = getExecutableDir() + "tint.json";
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file << root.dump(2);
        file.close();
    }
    else {
        std::cerr << "Failed to create tint.json: " << path << std::endl;
    }
}

// ---------- overlay.json：base 材质 -> 叠加层序列 ----------
namespace {
    std::mutex g_overlayMutex;
    std::unordered_map<std::string, std::vector<OverlayLayerInfo>> g_overlaySequences;
    std::unordered_set<std::string> g_overlayWarned;
}

void RegisterOverlaySequence(const std::string& baseName,
    const std::vector<OverlayLayerInfo>& overlays) {
    if (baseName.empty() || overlays.empty()) return;

    // 同一序列内按首次出现去重，保持绘制顺序
    std::vector<OverlayLayerInfo> deduped;
    for (const auto& layer : overlays) {
        bool exists = false;
        for (const auto& existing : deduped) {
            if (existing.name == layer.name) { exists = true; break; }
        }
        if (!exists) deduped.push_back(layer);
    }
    if (deduped.empty()) return;

    std::lock_guard<std::mutex> lock(g_overlayMutex);
    auto it = g_overlaySequences.find(baseName);
    if (it == g_overlaySequences.end()) {
        g_overlaySequences.emplace(baseName, std::move(deduped));
        return;
    }

    // 同一 base 材质只允许一组叠加序列；不一致时保留首组并告警一次
    const auto& kept = it->second;
    bool same = kept.size() == deduped.size();
    if (same) {
        for (size_t i = 0; i < kept.size(); ++i) {
            if (kept[i].name != deduped[i].name) { same = false; break; }
        }
    }
    if (same) return;
    if (g_overlayWarned.insert(baseName).second) {
        std::cerr << "[WARN] 材质 " << baseName
            << " 出现多组叠加序列, 保留首组, 忽略后续" << std::endl;
    }
}

void CreateOverlayJsonFile() {
    nlohmann::json root = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        if (g_overlaySequences.empty()) return;
        for (const auto& entry : g_overlaySequences) {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& layer : entry.second) {
                nlohmann::json value = nlohmann::json::object();
                value["name"] = layer.name;
                value["texture"] = layer.texturePath;
                if (layer.tint.on()) {
                    value["kind"] = TintKindName(layer.tint.kind);
                    if (layer.tint.kind == TintKind::Fixed) {
                        nlohmann::json color = nlohmann::json::array();
                        color.push_back(SrgbChannelToLinear((layer.tint.color >> 16) & 0xFF));
                        color.push_back(SrgbChannelToLinear((layer.tint.color >> 8) & 0xFF));
                        color.push_back(SrgbChannelToLinear(layer.tint.color & 0xFF));
                        value["color"] = std::move(color);
                    }
                }
                list.push_back(std::move(value));
            }
            root[entry.first] = std::move(list);
        }
    }

    std::string path = getExecutableDir() + "overlay.json";
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file << root.dump(2);
        file.close();
        std::cout << "■ overlay.json: " << root.size() << " 个材质带叠加层" << std::endl;
    }
    else {
        std::cerr << "Failed to create overlay.json: " << path << std::endl;
    }
}

// overlay.json 里引用到的全部材质名（base 与各层 name），供材质剪枝时保留
std::unordered_set<std::string> GetOverlayReferencedMaterialNames() {
    std::unordered_set<std::string> names;
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    for (const auto& entry : g_overlaySequences) {
        names.insert(entry.first);
        for (const auto& layer : entry.second) names.insert(layer.name);
    }
    return names;
}

// ---------- doubleSided.json：使用了双面几何的材质名数组 ----------
// 登记发生在导出收尾(分组拼接 / 全模型合并)之后, tint 变体展开早已完成,
// 所以这里的材质名可能带 @grass 之类的后缀, 登记时统一剥掉。
std::string StripTintSuffix(const std::string& name) {
    const size_t at = name.find('@');
    return (at == std::string::npos) ? name : name.substr(0, at);
}

namespace {
    std::mutex g_doubleSidedMutex;
    // 基础材质全名 -> 是否已登记(用 set 去重, 同一 tint 变体只留一条)
    std::unordered_set<std::string> g_doubleSidedMaterials;
    // 分组路径下每个分组都会调一次写出函数, 用这个标记保证只有第一次真正落盘
    bool g_doubleSidedWritten = false;
}

void RegisterDoubleSidedMaterial(const Material& material) {
    const std::string base = StripTintSuffix(material.name);
    if (base.empty()) return;
    std::lock_guard<std::mutex> lock(g_doubleSidedMutex);
    g_doubleSidedMaterials.insert(base);
}

// 输出 doubleSided.json：纯数组，元素为使用了双面几何的材质基础全名。
// 供 Blender 侧按材质名决定是否关闭背面剔除(与 Defs.material_base_name() 口径一致)。
// 注意: 分组路径下每个分组都会调用一次, 所以整体只落盘一次(以第一次的清单为准)。
void CreateDoubleSidedJsonFile() {
    // 先按顺序取走并清空累加器, 再把结果写出(避免长时间持锁)
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(g_doubleSidedMutex);
        if (g_doubleSidedWritten) return;
        g_doubleSidedWritten = true;
        names.assign(g_doubleSidedMaterials.begin(), g_doubleSidedMaterials.end());
        g_doubleSidedMaterials.clear();
    }
    // 排序让输出稳定, 便于跨次导出比对
    std::sort(names.begin(), names.end());

    nlohmann::json root = nlohmann::json::array();
    for (const auto& name : names) root.push_back(name);

    const std::string path = getExecutableDir() + "doubleSided.json";
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file << root.dump(2);
        file.close();
        std::cout << "■ doubleSided.json: " << names.size() << " 个材质使用双面几何" << std::endl;
    }
    else {
        std::cerr << "Failed to create doubleSided.json: " << path << std::endl;
    }
}

// 创建 .mtl 文件,接收 textureToPath 作为参数
void createMtlFile(const ModelData& data, const std::string& mtlFileName) {
    std::string exeDir = getExecutableDir();
    std::string fullMtlPath = exeDir + mtlFileName + ".mtl";

    std::ofstream mtlFile(fullMtlPath);
    if (mtlFile.is_open()) {
        for (size_t i = 0; i < data.materials.size(); ++i) {
            const std::string& textureName = data.materials[i].name;
            std::string texturePath = data.materials[i].texturePath;

            mtlFile << "newmtl " << textureName << "\n";

            // 处理材质类型
            if (texturePath == "None") {
                // LIGHT材质处理
                mtlFile << "Ns 200.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ka 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.900000 0.900000 0.900000\n";
                mtlFile << "Ke 0.900000 0.900000 0.900000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 2\n";
            }
            // 缺失纹理不写虚假的 .png 引用，避免 Blender 显示紫色材质。
            else if (texturePath.empty()) {
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            // 处理纯颜色材质(支持流体格式:color#r g b-流体名 和普通格式:color#r g b=)
            else if (texturePath.find("color#") != std::string::npos) {
                std::string colorStr;
                size_t pos = texturePath.find("-");
                size_t pos_deng = texturePath.find("=");
                if (pos != std::string::npos) {
                    // 流体材质格式,提取"-"前面的颜色部分
                    colorStr = texturePath.substr(std::string("color#").size(), pos - std::string("color#").size());
                }
                else if (pos_deng != std::string::npos) {
                    colorStr = texturePath.substr(std::string("color#").size(), pos_deng - std::string("color#").size());
                }
                else {
                    colorStr = "";
                }
                std::istringstream iss(colorStr);
                float r, g, b;
                if (iss >> r >> g >> b) {
                    mtlFile << "Kd " << std::fixed << std::setprecision(config.decimalPlaces)
                        << r << " " << g << " " << b << "\n";
                }
                else {
                    mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                    std::cerr << "Error: Invalid color format in '" << texturePath << "'\n";
                }
                // 共用普通材质参数
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            else {
                // 普通纹理材质处理
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";

                // 处理纹理路径
                if (mtlFileName.find("//") != std::string::npos) {
                    texturePath = "../" + texturePath;
                }
                if (texturePath.find(".png") == std::string::npos) {
                    texturePath += ".png";
                }
                mtlFile << "map_Kd " << texturePath << "\n";
                // Only emit map_d for a distinct opacity texture; Crafter uses map_Kd's alpha.
            }

            mtlFile << "\n";
        }
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFileName << std::endl;
    }
}

// 单独的文件创建方法
void CreateModelFiles(const ModelData& data, const std::string& filename) {
    auto start = high_resolution_clock::now();  // 新增:开始时间点
    // 创建MTL文件
    try
    {
        createMtlFile(data, filename);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error occurred: " << e.what() << std::endl;
    }
    if (data.vertices.size()>8000)
    {
        // 创建OBJ文件(内存映射)
        createObjFileViaMemoryMapped(data, filename);
    }
    else
    {
        // 创建OBJ文件
        createObjFile(data, filename);
    }
    
    
    
    auto end = high_resolution_clock::now();  // 新增:结束时间点
    auto duration = duration_cast<milliseconds>(end - start);  // 新增:计算时间差
    std::cout << "模型导出obj耗时: " << duration.count() << " ms" << std::endl;  // 新增:输出到控制台
    
    
}

//多个obj文件创建方法
void CreateMultiModelFiles(const ModelData& data, const std::string& filename,
    std::unordered_map<std::string, std::string>& uniqueMaterialsL,
    const std::string& sharedMtlName) { // 新增共享mtl名称参数
    auto start = high_resolution_clock::now();

    if (data.vertices.size() > 8000) {
        createObjFileViaMemoryMapped(data, filename, sharedMtlName); // 传递共享mtl名称
    }
    else {
        createObjFile(data, filename, sharedMtlName); // 传递共享mtl名称
    }

    // 收集材质信息
    for (size_t i = 0; i < data.materials.size(); ++i) {
        uniqueMaterialsL[data.materials[i].name] = data.materials[i].texturePath;
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    std::cout << "模型导出obj耗时: " << duration.count() << " ms" << std::endl;
}
