#include "diagnostics/d3d11_capture.hpp"
#include "core/logger.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <safetyhook.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace aigiskr::diagnostics {

namespace {

struct PendingImage {
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> pixels;  // RGBA/BGRA top-down tightly packed 4 bytes/px
    std::wstring path;
};

CaptureOptions g_opts;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_captureEnabled{false};
std::atomic<uint32_t> g_snapCount{0};
std::atomic<ULONGLONG> g_lastCaptureTick{0};
std::atomic<uint64_t> g_presentCalls{0};

SafetyHookInline g_presentHook;
std::thread g_bootstrapThread;
std::thread g_writerThread;

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::queue<PendingImage> g_queue;

thread_local int t_presentDepth = 0;

bool CheckManualSnapRequest(std::wstring& outTag) {
    const std::wstring reqPath = g_opts.gameDir + L"aigiskr_snap.req";
    if (GetFileAttributesW(reqPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    wchar_t tag[64] = {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, reqPath.c_str(), L"r") == 0 && f) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            std::wstring ws = core::Utf8ToWide(s);
            for (wchar_t c : ws) {
                if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                    (c >= L'A' && c <= L'Z') || c == L'_' || c == L'-') {
                    if (outTag.size() < 32) outTag.push_back(c);
                }
            }
        }
        fclose(f);
    }
    DeleteFileW(reqPath.c_str());
    return true;
}

bool SavePngWic(IWICImagingFactory* factory, const PendingImage& img) {
    if (!factory || img.width == 0 || img.height == 0) return false;
    WICPixelFormatGUID srcFmt = (img.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                 img.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                                    ? GUID_WICPixelFormat32bppRGBA
                                    : GUID_WICPixelFormat32bppBGRA;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(img.path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(img.width, img.height))) return false;

    WICPixelFormatGUID outFmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&outFmt);

    // Strip alpha (opaque RGB/BGR output so black alpha channels never hide pixels!)
    std::vector<uint8_t> packed(img.pixels.size());
    const uint8_t* src = img.pixels.data();
    for (size_t i = 0; i + 3 < packed.size(); i += 4) {
        packed[i + 0] = src[i + 0];
        packed[i + 1] = src[i + 1];
        packed[i + 2] = src[i + 2];
        packed[i + 3] = 0xFF;
    }

    if (outFmt != srcFmt) {
        ComPtr<IWICBitmap> bmp;
        if (FAILED(factory->CreateBitmapFromMemory(img.width, img.height, srcFmt,
                                                   img.width * 4, static_cast<UINT>(packed.size()), packed.data(), &bmp))) {
            return false;
        }
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(bmp.Get(), outFmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            return false;
        }
        if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    } else {
        if (FAILED(frame->WritePixels(img.height, img.width * 4, static_cast<UINT>(packed.size()), packed.data()))) {
            return false;
        }
    }

    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

void WriterLoop() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }

    while (!g_stop.load(std::memory_order_relaxed)) {
        PendingImage item;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait_for(lock, std::chrono::milliseconds(200), [] {
                return g_stop.load(std::memory_order_relaxed) || !g_queue.empty();
            });
            if (g_queue.empty()) continue;
            item = std::move(g_queue.front());
            g_queue.pop();
        }
        const bool ok = SavePngWic(factory.Get(), item);
        core::Logger::Instance().Logf("[DIAG] Snapshot %s (%ux%u fmt=%u) -> %s",
                                       ok ? "saved" : "FAILED",
                                       item.width, item.height,
                                       static_cast<unsigned>(item.format),
                                       core::WideToUtf8(item.path).c_str());
    }

    factory.Reset();
    if (SUCCEEDED(coHr)) CoUninitialize();
}

void CaptureSwapChainFrame(IDXGISwapChain* sc, const std::wstring& tag) {
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf())))) return;

    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    if (desc.Width < 100 || desc.Height < 100) return;

    ComPtr<ID3D11Device> dev;
    backbuffer->GetDevice(&dev);
    if (!dev) return;

    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return;

    D3D11_TEXTURE2D_DESC stDesc = desc;
    stDesc.MipLevels = 1;
    stDesc.ArraySize = 1;
    stDesc.SampleDesc.Count = 1;
    stDesc.SampleDesc.Quality = 0;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&stDesc, nullptr, &staging))) return;

    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC resDesc = stDesc;
        resDesc.Usage = D3D11_USAGE_DEFAULT;
        resDesc.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> resolved;
        if (FAILED(dev->CreateTexture2D(&resDesc, nullptr, &resolved))) return;
        ctx->ResolveSubresource(resolved.Get(), 0, backbuffer.Get(), 0, desc.Format);
        ctx->CopyResource(staging.Get(), resolved.Get());
    } else {
        ctx->CopyResource(staging.Get(), backbuffer.Get());
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

    PendingImage img;
    img.width = desc.Width;
    img.height = desc.Height;
    img.format = desc.Format;
    img.pixels.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(img.pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
                    reinterpret_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(desc.Width) * 4);
    }
    ctx->Unmap(staging.Get(), 0);

    const uint32_t idx = g_snapCount.fetch_add(1, std::memory_order_relaxed);
    wchar_t fname[128] = {};
    if (!tag.empty()) {
        swprintf_s(fname, L"snap_%03u_%s.png", idx, tag.c_str());
    } else {
        swprintf_s(fname, L"snap_%03u.png", idx);
    }
    img.path = g_opts.snapshotDir + fname;

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_queue.size() < 4) {
            g_queue.push(std::move(img));
            g_queueCv.notify_one();
        }
    }
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    ++t_presentDepth;
    if (t_presentDepth == 1 && g_captureEnabled.load(std::memory_order_relaxed)) {
        const uint64_t callNum = g_presentCalls.fetch_add(1, std::memory_order_relaxed);
        if (callNum == 0) {
            core::Logger::Instance().Logf("[DIAG] First IDXGISwapChain::Present observed sc=0x%p", sc);
        }
        std::wstring tag;
        const bool manual = CheckManualSnapRequest(tag);
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG last = g_lastCaptureTick.load(std::memory_order_relaxed);
        const bool timerDue = (g_opts.intervalMs > 0) &&
                              (g_snapCount.load(std::memory_order_relaxed) < g_opts.maxSnapshots) &&
                              (last == 0 || (now - last) >= g_opts.intervalMs);
        if (manual || timerDue) {
            g_lastCaptureTick.store(now, std::memory_order_relaxed);
            CaptureSwapChainFrame(sc, tag);
        }
    }
    HRESULT hr = g_presentHook.stdcall<HRESULT>(sc, syncInterval, flags);
    --t_presentDepth;
    return hr;
}

void BootstrapPresentHook() {
    using CreateDevScFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                          const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                          ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    HMODULE hD3D11 = nullptr;
    for (int i = 0; i < 300 && !g_stop.load(std::memory_order_relaxed); ++i) {
        hD3D11 = GetModuleHandleW(L"d3d11.dll");
        if (hD3D11 && GetModuleHandleW(L"dxgi.dll")) break;
        Sleep(100);
    }
    if (!hD3D11 || g_stop.load(std::memory_order_relaxed)) {
        core::Logger::Instance().LogLine("[DIAG] d3d11.dll/dxgi.dll not loaded within timeout; capture skipped.");
        return;
    }

    // Give the game a short moment to create its device before we spin a dummy swapchain.
    Sleep(1000);
    if (g_stop.load(std::memory_order_relaxed)) return;

    auto pCreate = reinterpret_cast<CreateDevScFn>(GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain"));
    if (!pCreate) {
        core::Logger::Instance().LogLine("[DIAG] D3D11CreateDeviceAndSwapChain export missing.");
        return;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AigisKRCapDummyWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                                nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 16;
    scd.BufferDesc.Height = 16;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd ? hwnd : GetDesktopWindow();
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    void* presentTarget = nullptr;
    if (SUCCEEDED(hr) && sc) {
        presentTarget = (*reinterpret_cast<void***>(sc.Get()))[8];
    }
    sc.Reset();
    ctx.Reset();
    dev.Reset();
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!presentTarget) {
        core::Logger::Instance().Logf("[DIAG] Dummy swapchain failed hr=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    auto hookRes = safetyhook::InlineHook::create(presentTarget, reinterpret_cast<void*>(&HookedPresent));
    if (!hookRes) {
        core::Logger::Instance().Logf("[DIAG] SafetyHook on IDXGISwapChain::Present failed.");
        return;
    }
    g_presentHook = std::move(*hookRes);
    g_captureEnabled.store(true, std::memory_order_relaxed);
    core::Logger::Instance().Logf("[DIAG] IDXGISwapChain::Present hooked at 0x%p (interval=%ums max=%u)",
                                   presentTarget, g_opts.intervalMs, g_opts.maxSnapshots);
}

}  // namespace

void StartBackbufferCapture(const CaptureOptions& opts) {
    if (!opts.enabled) return;
    g_opts = opts;
    CreateDirectoryW(g_opts.snapshotDir.c_str(), nullptr);
    g_stop.store(false, std::memory_order_relaxed);
    g_writerThread = std::thread(WriterLoop);
    g_bootstrapThread = std::thread(BootstrapPresentHook);
}

void StopBackbufferCapture(bool cleanUnload) {
    g_captureEnabled.store(false, std::memory_order_relaxed);
    g_stop.store(true, std::memory_order_relaxed);
    g_queueCv.notify_all();
    if (cleanUnload) {
        if (g_bootstrapThread.joinable()) g_bootstrapThread.join();
        if (g_writerThread.joinable()) g_writerThread.join();
        g_presentHook = {};
    } else {
        if (g_bootstrapThread.joinable()) g_bootstrapThread.detach();
        if (g_writerThread.joinable()) g_writerThread.detach();
    }
}

}  // namespace aigiskr::diagnostics
