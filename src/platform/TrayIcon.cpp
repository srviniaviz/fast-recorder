#include "platform/TrayIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

namespace fastrecord::platform {

namespace {

template <size_t Size>
void copyText(wchar_t (&destination)[Size], const std::wstring& source) {
    wcsncpy_s(destination, Size, source.c_str(), _TRUNCATE);
}

struct Color {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

// Straight-alpha BGRA canvas drawn directly at the tray size, so the shell
// never has to downscale (and blur) the recording indicator.
class Canvas {
public:
    explicit Canvas(int size)
        : m_size(size), m_pixels(static_cast<std::size_t>(size) * size * 4, 0) {}

    int size() const noexcept { return m_size; }
    const std::vector<std::uint8_t>& pixels() const noexcept { return m_pixels; }

    void disc(float radius, Color color, float opacity = 1.0F) {
        const float center = m_size / 2.0F;
        fill(color, opacity, [=](float x, float y) {
            const float distance = std::hypot(x - center, y - center);
            return std::clamp(radius + 0.5F - distance, 0.0F, 1.0F);
        });
    }

    void rect(float left, float top, float right, float bottom, Color color) {
        fill(color, 1.0F, [=](float x, float y) {
            const float horizontal =
                std::clamp(std::min(x - left, right - x) + 0.5F, 0.0F, 1.0F);
            const float vertical =
                std::clamp(std::min(y - top, bottom - y) + 0.5F, 0.0F, 1.0F);
            return horizontal * vertical;
        });
    }

private:
    template <typename Coverage>
    void fill(Color color, float opacity, Coverage coverage) {
        for (int y = 0; y < m_size; ++y) {
            for (int x = 0; x < m_size; ++x) {
                const float alpha =
                    opacity * coverage(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F);
                if (alpha > 0.0F) {
                    blend(x, y, color, alpha);
                }
            }
        }
    }

    void blend(int x, int y, Color color, float alpha) {
        std::uint8_t* pixel = m_pixels.data() + (static_cast<std::size_t>(y) * m_size + x) * 4;
        const float destinationAlpha = pixel[3] / 255.0F;
        const float outputAlpha = alpha + destinationAlpha * (1.0F - alpha);
        const auto channel = [&](std::uint8_t source, std::uint8_t current) {
            const float value = (source * alpha + current * destinationAlpha * (1.0F - alpha)) /
                outputAlpha;
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
        };
        pixel[0] = channel(color.blue, pixel[0]);
        pixel[1] = channel(color.green, pixel[1]);
        pixel[2] = channel(color.red, pixel[2]);
        pixel[3] = static_cast<std::uint8_t>(std::lround(outputAlpha * 255.0F));
    }

    int m_size;
    std::vector<std::uint8_t> m_pixels;
};

HICON iconFromCanvas(const Canvas& canvas) {
    const int size = canvas.size();

    BITMAPINFO colorDescription{};
    colorDescription.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    colorDescription.bmiHeader.biWidth = size;
    colorDescription.bmiHeader.biHeight = -size;
    colorDescription.bmiHeader.biPlanes = 1;
    colorDescription.bmiHeader.biBitCount = 32;
    colorDescription.bmiHeader.biCompression = BI_RGB;

    void* colorPixels = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(
        nullptr, &colorDescription, DIB_RGB_COLORS, &colorPixels, nullptr, 0);
    if (colorBitmap == nullptr || colorPixels == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        return nullptr;
    }
    std::memcpy(colorPixels, canvas.pixels().data(), canvas.pixels().size());

    // The alpha channel drives rendering; the mask only marks fully clear pixels.
    const std::size_t maskStride = ((static_cast<std::size_t>(size) + 15u) / 16u) * 2u;
    std::vector<std::uint8_t> maskBits(maskStride * size, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (canvas.pixels()[(static_cast<std::size_t>(y) * size + x) * 4 + 3] == 0) {
                maskBits[static_cast<std::size_t>(y) * maskStride + x / 8] |=
                    static_cast<std::uint8_t>(0x80u >> (x % 8));
            }
        }
    }
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits.data());
    if (maskBitmap == nullptr) {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colorBitmap;
    info.hbmMask = maskBitmap;
    const HICON icon = CreateIconIndirect(&info);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon;
}

constexpr Color kOutline{14, 15, 19};
constexpr Color kRingLight{246, 247, 250};
constexpr Color kRecordDim{196, 32, 44};
constexpr Color kRecordBright{255, 59, 70};
constexpr Color kPausedRing{150, 156, 168};
constexpr Color kPausedFill{38, 41, 49};

std::uint8_t mix(std::uint8_t from, std::uint8_t to, float amount) {
    return static_cast<std::uint8_t>(std::lround(from + (to - from) * amount));
}

} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

int TrayIcon::traySize() {
    const int size = GetSystemMetrics(SM_CXSMICON);
    return std::clamp(size > 0 ? size : 16, 16, 64);
}

HICON TrayIcon::createRecordingFrame(int size, float intensity) {
    if (size < 8) {
        return nullptr;
    }
    intensity = std::clamp(intensity, 0.0F, 1.0F);
    const float unit = size / 16.0F;

    // Inverted logo: light ring around a solid red "record" dot that breathes.
    // The dark rim keeps the shape readable on light taskbars too.
    Canvas canvas(size);
    canvas.disc(8.0F * unit - 0.25F, kOutline, 0.9F);
    canvas.disc(6.9F * unit, kRingLight);
    canvas.disc(5.0F * unit, kOutline, 0.55F);
    const Color core{
        mix(kRecordDim.red, kRecordBright.red, intensity),
        mix(kRecordDim.green, kRecordBright.green, intensity),
        mix(kRecordDim.blue, kRecordBright.blue, intensity),
    };
    canvas.disc((3.6F + 0.9F * intensity) * unit, core);
    return iconFromCanvas(canvas);
}

HICON TrayIcon::createPausedIcon(int size) {
    if (size < 8) {
        return nullptr;
    }
    const float unit = size / 16.0F;
    const float center = size / 2.0F;

    Canvas canvas(size);
    canvas.disc(8.0F * unit - 0.25F, kOutline, 0.9F);
    canvas.disc(6.9F * unit, kPausedRing);
    canvas.disc(5.6F * unit, kPausedFill);
    const float barWidth = 1.7F * unit;
    const float gap = 1.0F * unit;
    const float top = center - 3.0F * unit;
    const float bottom = center + 3.0F * unit;
    canvas.rect(center - gap - barWidth, top, center - gap, bottom, kRingLight);
    canvas.rect(center + gap, top, center + gap + barWidth, bottom, kRingLight);
    return iconFromCanvas(canvas);
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
