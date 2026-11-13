#include "platform/TrayIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>

namespace fastrecord::platform {

namespace {

template <size_t Size>
void copyText(wchar_t (&destination)[Size], const std::wstring& source) {
    wcsncpy_s(destination, Size, source.c_str(), _TRUNCATE);
}

struct IconBitmapGuard {
    ICONINFO& info;
    ~IconBitmapGuard() {
        if (info.hbmColor != nullptr) {
            DeleteObject(info.hbmColor);
        }
        if (info.hbmMask != nullptr) {
            DeleteObject(info.hbmMask);
        }
    }
};

void blendPixel(
    std::uint8_t* destination,
    std::uint8_t blue,
    std::uint8_t green,
    std::uint8_t red,
    std::uint8_t alpha) {
    if (alpha == 0) {
        return;
    }

    const std::uint32_t destinationAlpha = destination[3];
    const std::uint32_t inverseAlpha = 255u - alpha;
    const std::uint32_t outputAlpha = alpha +
        (destinationAlpha * inverseAlpha + 127u) / 255u;
    const auto compositeChannel = [=](std::uint8_t source, std::uint8_t current) {
        const std::uint32_t sourcePremultiplied =
            static_cast<std::uint32_t>(source) * alpha;
        const std::uint32_t destinationPremultiplied =
            (static_cast<std::uint32_t>(current) * destinationAlpha * inverseAlpha + 127u) / 255u;
        return static_cast<std::uint8_t>(
            (sourcePremultiplied + destinationPremultiplied + outputAlpha / 2u) / outputAlpha);
    };

    destination[0] = compositeChannel(blue, destination[0]);
    destination[1] = compositeChannel(green, destination[1]);
    destination[2] = compositeChannel(red, destination[2]);
    destination[3] = static_cast<std::uint8_t>(outputAlpha);
}

void paintDisc(
    std::uint8_t* pixels,
    int width,
    int height,
    float centerX,
    float centerY,
    float radius,
    std::uint8_t blue,
    std::uint8_t green,
    std::uint8_t red,
    std::uint8_t opacity) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(radius + 0.5F - distance, 0.0F, 1.0F);
            const auto alpha = static_cast<std::uint8_t>(opacity * coverage);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            blendPixel(pixels + offset, blue, green, red, alpha);
        }
    }
}

void paintRing(
    std::uint8_t* pixels,
    int width,
    int height,
    float centerX,
    float centerY,
    float radius,
    float thickness,
    std::uint8_t blue,
    std::uint8_t green,
    std::uint8_t red,
    std::uint8_t opacity) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(
                thickness + 0.5F - std::abs(distance - radius),
                0.0F,
                1.0F);
            const auto alpha = static_cast<std::uint8_t>(opacity * coverage);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            blendPixel(pixels + offset, blue, green, red, alpha);
        }
    }
}

} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

HICON TrayIcon::createPulseFrame(HICON baseIcon, float intensity) {
    if (baseIcon == nullptr) {
        return nullptr;
    }

    ICONINFO sourceInfo{};
    if (!GetIconInfo(baseIcon, &sourceInfo) || sourceInfo.hbmColor == nullptr) {
        if (sourceInfo.hbmColor != nullptr) {
            DeleteObject(sourceInfo.hbmColor);
        }
        if (sourceInfo.hbmMask != nullptr) {
            DeleteObject(sourceInfo.hbmMask);
        }
        return nullptr;
    }
    const IconBitmapGuard sourceBitmaps{sourceInfo};

    BITMAP sourceBitmap{};
    if (GetObjectW(sourceInfo.hbmColor, sizeof(sourceBitmap), &sourceBitmap) == 0) {
        return nullptr;
    }
    if (sourceBitmap.bmWidth <= 0 || sourceBitmap.bmHeight == 0) {
        return nullptr;
    }

    // Keep the logo fixed; only the recording indicator's glow changes per frame.
    constexpr int width = 32;
    constexpr int height = 32;
    intensity = std::clamp(intensity, 0.0F, 1.0F);

    BITMAPINFO colorDescription{};
    colorDescription.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    colorDescription.bmiHeader.biWidth = width;
    colorDescription.bmiHeader.biHeight = -height;
    colorDescription.bmiHeader.biPlanes = 1;
    colorDescription.bmiHeader.biBitCount = 32;
    colorDescription.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    void* colorPixels = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(
        screen,
        &colorDescription,
        DIB_RGB_COLORS,
        &colorPixels,
        nullptr,
        0);
    if (colorBitmap == nullptr || colorPixels == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    std::memset(colorPixels, 0, static_cast<std::size_t>(width) * height * 4);

    HDC memoryDc = CreateCompatibleDC(screen);
    if (memoryDc == nullptr) {
        DeleteObject(colorBitmap);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    const HGDIOBJ previousBitmap = SelectObject(memoryDc, colorBitmap);
    const bool bitmapSelected = previousBitmap != nullptr && previousBitmap != HGDI_ERROR;
    const bool iconDrawn = bitmapSelected && DrawIconEx(
            memoryDc, 0, 0, baseIcon, width, height, 0, nullptr, DI_NORMAL) != FALSE;
    if (previousBitmap != nullptr && previousBitmap != HGDI_ERROR) {
        SelectObject(memoryDc, previousBitmap);
    }
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screen);
    if (!iconDrawn) {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint8_t*>(colorPixels);
    constexpr float indicatorX = 24.0F;
    constexpr float indicatorY = 7.0F;
    const auto glowOpacity = static_cast<std::uint8_t>(190.0F * intensity);
    paintRing(
        pixels,
        width,
        height,
        indicatorX,
        indicatorY,
        4.1F,
        1.25F,
        62,
        54,
        255,
        glowOpacity);
    paintDisc(pixels, width, height, indicatorX, indicatorY, 3.35F, 18, 19, 25, 255);
    paintDisc(pixels, width, height, indicatorX, indicatorY, 2.75F, 240, 241, 246, 255);
    const auto coreBlue = static_cast<std::uint8_t>(48.0F + 28.0F * intensity);
    const auto coreGreen = static_cast<std::uint8_t>(38.0F + 30.0F * intensity);
    paintDisc(pixels, width, height, indicatorX, indicatorY, 2.1F, coreBlue, coreGreen, 255, 255);

    struct MonochromeBitmapInfo {
        BITMAPINFOHEADER header;
        RGBQUAD colors[2];
    } maskDescription{};
    maskDescription.header.biSize = sizeof(BITMAPINFOHEADER);
    maskDescription.header.biWidth = width;
    maskDescription.header.biHeight = -height;
    maskDescription.header.biPlanes = 1;
    maskDescription.header.biBitCount = 1;
    maskDescription.header.biCompression = BI_RGB;
    maskDescription.colors[0] = RGBQUAD{0, 0, 0, 0};
    maskDescription.colors[1] = RGBQUAD{255, 255, 255, 0};
    void* maskPixels = nullptr;
    screen = GetDC(nullptr);
    if (screen == nullptr) {
        DeleteObject(colorBitmap);
        return nullptr;
    }
    HBITMAP maskBitmap = CreateDIBSection(
        screen,
        reinterpret_cast<BITMAPINFO*>(&maskDescription),
        DIB_RGB_COLORS,
        &maskPixels,
        nullptr,
        0);
    ReleaseDC(nullptr, screen);
    if (maskBitmap == nullptr || maskPixels == nullptr) {
        if (maskBitmap != nullptr) {
            DeleteObject(maskBitmap);
        }
        DeleteObject(colorBitmap);
        return nullptr;
    }

    const std::size_t maskStride = ((static_cast<std::size_t>(width) + 31u) / 32u) * 4u;
    auto* maskBytes = static_cast<std::uint8_t*>(maskPixels);
    std::memset(maskBytes, 0, maskStride * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t colorOffset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (pixels[colorOffset + 3] == 0) {
                const std::size_t maskOffset = static_cast<std::size_t>(y) * maskStride +
                    static_cast<std::size_t>(x) / 8u;
                maskBytes[maskOffset] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
            }
        }
    }

    ICONINFO frameInfo{};
    frameInfo.fIcon = TRUE;
    frameInfo.hbmColor = colorBitmap;
    frameInfo.hbmMask = maskBitmap;
    const HICON frame = CreateIconIndirect(&frameInfo);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return frame;
}

bool TrayIcon::create(
    HWND owner,
    HICON icon,
    const std::wstring& tooltip,
    Callback callback) {
    if (m_created || owner == nullptr || icon == nullptr) {
        return false;
    }

    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = owner;
    m_data.uID = 1;
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_data.uCallbackMessage = kCallbackMessage;
    m_data.hIcon = icon;
    copyText(m_data.szTip, tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &m_data)) {
        m_data = {};
        return false;
    }

    m_data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_data);

    m_callback = std::move(callback);
    m_created = true;
    return true;
}

void TrayIcon::destroy() {
    if (!m_created) {
        return;
    }

    Shell_NotifyIconW(NIM_DELETE, &m_data);
    m_callback = {};
    m_data = {};
    m_created = false;
}

bool TrayIcon::setIcon(HICON icon) {
    if (!m_created || icon == nullptr) {
        return false;
    }

    m_data.hIcon = icon;
    m_data.uFlags = NIF_ICON;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

bool TrayIcon::setTooltip(const std::wstring& tooltip) {
    if (!m_created) {
        return false;
    }

    copyText(m_data.szTip, tooltip);
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

bool TrayIcon::showBalloon(
    const std::wstring& title,
    const std::wstring& message,
    DWORD infoFlags) {
    if (!m_created) {
        return false;
    }

    m_data.uFlags = NIF_INFO;
    copyText(m_data.szInfoTitle, title);
    copyText(m_data.szInfo, message);
    m_data.dwInfoFlags = infoFlags;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

} // namespace fastrecord::platform
