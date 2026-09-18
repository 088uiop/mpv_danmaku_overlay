#pragma once

// ============================================================
// platform.h - Windows 平台统一头文件与工具函数
// danmaku_overlay - 高性能 Windows libmpv 弹幕播放器
// ============================================================
// 集中管理 Win32 / DirectX 头文件包含与 DPI 工具,
// 避免分散在各源文件中重复包含。
// ============================================================

// 必须在最前面定义, 避免 windows.h 污染全局命名空间
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Unicode 编码 (使用 WCHAR / wstring)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows 系统头文件
#include <windows.h>
#include <windowsx.h>

// DXGI / Direct3D 11
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>       // IDXGIFactory4 (翻转模型)
#include <dxgi1_6.h>       // IDXGIFactory6 (枚举适配器偏好)

// Direct2D / DirectWrite (弹幕文字渲染)
// 基础头文件在所有 Windows SDK / MinGW 版本中可用;
// 版本扩展头文件按可用性条件包含。
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_2.h>

// DWM (桌面窗口管理器 - 透明/合成)
#include <dwmapi.h>

// DirectComposition (GPU 直接合成, 无 CPU 读回)
#include <dcomp.h>

// COM 智能指针
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// 项目内部头文件 (同目录引用, 避免 include 路径问题)
#include "types.h"
#include "log.h"

// C 标准库
#include <cstdint>
#include <cstdlib>
#include <cstring>

// STL
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

// ============================================================
// 常用 COM 安全释放
// ============================================================
template <typename T>
inline void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

#define SAFE_RELEASE(p) SafeRelease(p)

// ============================================================
// HRESULT 检查宏
// ============================================================
#define CHECK_HR(hr, msg, ...)                                       \
    do {                                                             \
        HRESULT _hr = (hr);                                          \
        if (FAILED(_hr)) {                                           \
            DANMAKU_LOG_ERROR(LOG_TAG, "%s (HRESULT=0x%08X)", msg,  \
                             static_cast<unsigned int>(_hr));       \
            return ::danmaku_overlay::ErrorCode::D3D11Failed;            \
        }                                                            \
    } while (0)

#define CHECK_HR_RET(hr, msg, ret)                                   \
    do {                                                             \
        HRESULT _hr = (hr);                                          \
        if (FAILED(_hr)) {                                           \
            DANMAKU_LOG_ERROR(LOG_TAG, "%s (HRESULT=0x%08X)", msg,  \
                             static_cast<unsigned int>(_hr));       \
            return (ret);                                            \
        }                                                            \
    } while (0)

// ============================================================
// DPI 工具函数
// ============================================================
namespace danmaku_overlay {
namespace platform {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

// 设置进程 DPI 感知 (Per-Monitor V2 优先, 回退 V1, 最后回退 System)
inline bool SetDpiAwareness() {
    // Windows 10 1703+ : Per-Monitor V2
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        // SetProcessDpiAwarenessContext
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        auto pfn = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pfn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_CONTEXT_HANDLE)-4)
            // 用整数 -4 对应 Windows SDK 的 PER_MONITOR_AWARE_V2
            DPI_AWARENESS_CONTEXT v2_ctx = reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4);
            if (pfn(v2_ctx)) {
                DANMAKU_LOG_INFO(LOG_TAG, "DPI 感知: Per-Monitor V2");
                return true;
            }
        }
    }

    // 回退: SetProcessDpiAwareness (Windows 8.1+)
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
        auto pfn = reinterpret_cast<PFN_SetProcessDpiAwareness>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (pfn) {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            if (SUCCEEDED(pfn(2))) {
                DANMAKU_LOG_INFO(LOG_TAG, "DPI 感知: Per-Monitor V1 (回退)");
                FreeLibrary(shcore);
                return true;
            }
        }
        FreeLibrary(shcore);
    }

    // 最后回退: SetProcessDPIAware (Vista+)
    if (SetProcessDPIAware()) {
        DANMAKU_LOG_INFO(LOG_TAG, "DPI 感知: System (回退)");
        return true;
    }

    DANMAKU_LOG_WARN(LOG_TAG, "DPI 感知设置失败");
    return false;
}

// 获取指定窗口的 DPI
inline DpiInfo GetWindowDpi(HWND hwnd) {
    DpiInfo info;

    // Windows 10 1607+: GetDpiForWindow
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        auto pfn = reinterpret_cast<PFN_GetDpiForWindow>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (pfn) {
            info.dpi_x = pfn(hwnd);
            info.dpi_y = info.dpi_x;
            info.update();
            return info;
        }
    }

    // 回退: 从显示器获取
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
        auto pfn = reinterpret_cast<PFN_GetDpiForMonitor>(
            GetProcAddress(shcore, "GetDpiForMonitor"));
        if (pfn) {
            HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            // MDT_EFFECTIVE_DPI = 0
            if (SUCCEEDED(pfn(hmon, 0, &info.dpi_x, &info.dpi_y))) {
                info.update();
                FreeLibrary(shcore);
                return info;
            }
        }
        FreeLibrary(shcore);
    }

    // 最终回退: 96 DPI
    info.dpi_x = info.dpi_y = 96;
    info.update();
    return info;
}

// 逻辑像素 -> 物理像素
inline int LogicalToPhysical(int logical, uint32_t dpi) {
    return MulDiv(logical, dpi, 96);
}

// 物理像素 -> 逻辑像素
inline int PhysicalToLogical(int physical, uint32_t dpi) {
    return MulDiv(physical, 96, dpi);
}

// UTF-8 <-> UTF-16 转换
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                  static_cast<int>(utf8.size()),
                                  nullptr, 0);
    std::wstring wide(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        wide.data(), len);
    return wide;
}

inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                   static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()),
                        utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// UTF-16 -> UTF-32 (DirectWrite 需要 UTF-32 进行码点级排版)
inline std::u32string WideToUtf32(const std::wstring& wide) {
    std::u32string u32;
    u32.reserve(wide.size());
    size_t i = 0;
    while (i < wide.size()) {
        wchar_t c = wide[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < wide.size()) {
            // 代理对 (surrogate pair)
            wchar_t c2 = wide[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                u32.push_back(cp);
                i += 2;
                continue;
            }
        }
        u32.push_back(static_cast<char32_t>(c));
        i += 1;
    }
    return u32;
}

#pragma GCC diagnostic pop

} // namespace platform
} // namespace danmaku_overlay
