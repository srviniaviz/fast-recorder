#include "recording/NvencBackend.h"
#include "recording/GraphicsCapture.h"

#include <Windows.h>
#include <mfapi.h>
#include <roapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#ifndef FASTRECORD_HAS_NVENC
#define FASTRECORD_HAS_NVENC 0
#endif

#if FASTRECORD_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace fastrecord::recording {

namespace {

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

std::filesystem::path createOutputPath(const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[96]{};
    swprintf_s(
        name,
        L"Fast Record %04u-%02u-%02u %02u-%02u-%02u-%03u.mp4",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);
    return directory / name;
}

struct MediaRuntime {
    MediaRuntime() {
        winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED));
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            RoUninitialize();
            winrt::check_hresult(result);
        }
    }

    ~MediaRuntime() {
        winrt::clear_factory_cache();
        MFShutdown();
        RoUninitialize();
    }
};

#if FASTRECORD_HAS_NVENC

class NvencLibrary final {
public:
    NvencLibrary() {
#if defined(_WIN64)
        handle = LoadLibraryW(L"nvEncodeAPI64.dll");
#else
        handle = LoadLibraryW(L"nvEncodeAPI.dll");
#endif
    }

    ~NvencLibrary() {
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
    }

    NvencLibrary(const NvencLibrary&) = delete;
    NvencLibrary& operator=(const NvencLibrary&) = delete;

    HMODULE handle{nullptr};
};

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

std::wstring nvencStatusMessage(
    const wchar_t* operation,
    NVENCSTATUS status,
    const NV_ENCODE_API_FUNCTION_LIST* functions,
    void* session) {
    std::wstring message = operation;
    message += L" (código NVENC ";
    message += std::to_wstring(static_cast<int>(status));
    message += L").";

    if (functions != nullptr && session != nullptr && functions->nvEncGetLastErrorString != nullptr) {
        if (const char* detail = functions->nvEncGetLastErrorString(session); detail != nullptr && *detail != '\0') {
            const int length = MultiByteToWideChar(CP_UTF8, 0, detail, -1, nullptr, 0);
            if (length > 1) {
                std::wstring converted(static_cast<size_t>(length - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, detail, -1, converted.data(), length);
                message += L" ";
                message += converted;
            }
        }
    }
    return message;
}

size_t findStartCode(const std::vector<std::uint8_t>& data, size_t offset, size_t& codeSize) {
    for (size_t index = offset; index + 3 <= data.size(); ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            codeSize = 3;
            return index;
        }
        if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            codeSize = 4;
            return index;
        }
    }
    codeSize = 0;
    return std::numeric_limits<size_t>::max();
}

bool normalizeH264ToAnnexB(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start != std::numeric_limits<size_t>::max()) {
        packet = encoded;
        return true;
    }

    // Keep compatibility with drivers that return AVCC/length-prefixed output.
    for (size_t offset = 0; offset + sizeof(DWORD) <= encoded.size();) {
        const DWORD nalSize =
            (static_cast<DWORD>(encoded[offset]) << 24) |
            (static_cast<DWORD>(encoded[offset + 1]) << 16) |
            (static_cast<DWORD>(encoded[offset + 2]) << 8) |
            static_cast<DWORD>(encoded[offset + 3]);
        offset += sizeof(DWORD);
        if (nalSize == 0 || nalSize > encoded.size() - offset) {
            packet.clear();
            return false;
        }

        packet.insert(packet.end(), {0, 0, 0, 1});
        packet.insert(packet.end(), encoded.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset + nalSize));
        offset += nalSize;
    }

    return !packet.empty();
}

bool normalizeH264ToLengthPrefixed(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start == std::numeric_limits<size_t>::max()) {
        packet = encoded;
        return true;
    }

    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        size_t nextCodeSize = 0;
        const size_t next = findStartCode(encoded, nalStart, nextCodeSize);
        size_t nalEnd = next == std::numeric_limits<size_t>::max() ? encoded.size() : next;
        while (nalEnd > nalStart && encoded[nalEnd - 1] == 0) {
            --nalEnd;
        }

        if (nalEnd > nalStart) {
            const size_t nalSize = nalEnd - nalStart;
            if (nalSize > UINT32_MAX) {
                packet.clear();
                return false;
            }
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 24) & 0xff));
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 16) & 0xff));
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 8) & 0xff));
            packet.push_back(static_cast<std::uint8_t>(nalSize & 0xff));
            packet.insert(packet.end(), encoded.begin() + static_cast<std::ptrdiff_t>(nalStart),
                encoded.begin() + static_cast<std::ptrdiff_t>(nalEnd));
        }

        start = next;
        codeSize = nextCodeSize;
    }

    return !packet.empty();
}

bool extractH264SequenceHeader(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sequenceHeader) {
    sequenceHeader.clear();
    bool hasSps = false;
    bool hasPps = false;

    auto appendNal = [&](const std::uint8_t* nal, size_t length) {
        sequenceHeader.insert(sequenceHeader.end(), {0, 0, 0, 1});
        sequenceHeader.insert(sequenceHeader.end(), nal, nal + length);
        if ((nal[0] & 0x1f) == 7) {
            hasSps = true;
        } else if ((nal[0] & 0x1f) == 8) {
            hasPps = true;
        }
    };

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start != std::numeric_limits<size_t>::max()) {
        while (start != std::numeric_limits<size_t>::max()) {
            const size_t nalStart = start + codeSize;
            size_t nextCodeSize = 0;
            const size_t next = findStartCode(encoded, nalStart, nextCodeSize);
            size_t nalEnd = next == std::numeric_limits<size_t>::max() ? encoded.size() : next;
            while (nalEnd > nalStart && encoded[nalEnd - 1] == 0) {
                --nalEnd;
            }
            if (nalEnd > nalStart && ((encoded[nalStart] & 0x1f) == 7 || (encoded[nalStart] & 0x1f) == 8)) {
                appendNal(encoded.data() + nalStart, nalEnd - nalStart);
            }
            start = next;
            codeSize = nextCodeSize;
        }
    } else {
        for (size_t offset = 0; offset + sizeof(DWORD) <= encoded.size();) {
            const DWORD nalLength =
                (static_cast<DWORD>(encoded[offset]) << 24) |
                (static_cast<DWORD>(encoded[offset + 1]) << 16) |
                (static_cast<DWORD>(encoded[offset + 2]) << 8) |
                static_cast<DWORD>(encoded[offset + 3]);
            offset += sizeof(DWORD);
            if (nalLength == 0 || nalLength > encoded.size() - offset) {
                sequenceHeader.clear();
                return false;
            }
            if ((encoded[offset] & 0x1f) == 7 || (encoded[offset] & 0x1f) == 8) {
                appendNal(encoded.data() + offset, nalLength);
            }
            offset += nalLength;
        }
    }

    return hasSps && hasPps;
}

void writeBigEndian(std::ostream& output, std::uint64_t value, unsigned byteCount) {
    for (unsigned index = 0; index < byteCount; ++index) {
        const unsigned shift = (byteCount - index - 1) * 8;
        output.put(static_cast<char>((value >> shift) & 0xff));
    }
}

class Mp4Builder final {
public:
    void u8(std::uint8_t value) {
        bytes.push_back(value);
    }

    void u16(std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void u32(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32));
        u32(static_cast<std::uint32_t>(value & 0xffffffff));
    }

    void fullBox(std::uint8_t version, std::uint32_t flags) {
        u8(version);
        u8(static_cast<std::uint8_t>((flags >> 16) & 0xff));
        u8(static_cast<std::uint8_t>((flags >> 8) & 0xff));
        u8(static_cast<std::uint8_t>(flags & 0xff));
    }

    void fourcc(const char* value) {
        bytes.insert(bytes.end(), value, value + 4);
    }

    void append(const std::vector<std::uint8_t>& value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void zeros(size_t count) {
        bytes.insert(bytes.end(), count, 0);
    }

    size_t beginBox(const char* type) {
        const size_t position = bytes.size();
        u32(0);
        fourcc(type);
        return position;
    }

    void endBox(size_t position) {
        const auto size = bytes.size() - position;
        if (size > UINT32_MAX) {
            throw std::runtime_error("MP4 box is too large");
        }
        bytes[position] = static_cast<std::uint8_t>((size >> 24) & 0xff);
        bytes[position + 1] = static_cast<std::uint8_t>((size >> 16) & 0xff);
        bytes[position + 2] = static_cast<std::uint8_t>((size >> 8) & 0xff);
        bytes[position + 3] = static_cast<std::uint8_t>(size & 0xff);
    }

    std::vector<std::uint8_t> bytes;
};

bool containsH264NalType(const std::vector<std::uint8_t>& encoded, std::uint8_t expectedType) {
    std::vector<std::uint8_t> annexB;
    if (!normalizeH264ToAnnexB(encoded, annexB)) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(annexB, 0, codeSize);
    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        if (nalStart < annexB.size() && (annexB[nalStart] & 0x1f) == expectedType) {
            return true;
        }
        size_t nextCodeSize = 0;
        start = findStartCode(annexB, nalStart, nextCodeSize);
        codeSize = nextCodeSize;
    }
    return false;
}

class H264Mp4Writer final {
public:
    ~H264Mp4Writer() {
        std::wstring ignored;
        finalize(ignored);
    }

    bool start(
        const std::filesystem::path& output,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        const std::vector<std::uint8_t>& sequenceHeader,
        std::wstring& error) {
        if (!extractParameterSets(sequenceHeader, m_sps, m_pps)) {
            error = L"O cabeçalho H.264 do NVENC não contém SPS e PPS válidos.";
            return false;
        }

        m_file.open(output, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!m_file.is_open()) {
            error = L"Não foi possível criar o arquivo MP4 da gravação.";
            return false;
        }

        m_width = width;
        m_height = height;
        m_timeScale = fps;
        m_bitrate = bitrate;
        m_mdatOffset = 32;
        m_dataOffset = 48;
        m_dataBytes = 0;
        m_samples.clear();
        m_keyFrames.clear();

        writeBigEndian(m_file, 32, 4);
        m_file.write("ftyp", 4);
        m_file.write("isom", 4);
        writeBigEndian(m_file, 0x00000200, 4);
        m_file.write("isom", 4);
        m_file.write("iso2", 4);
        m_file.write("avc1", 4);
        m_file.write("mp41", 4);

        writeBigEndian(m_file, 1, 4);
        m_file.write("mdat", 4);
        writeBigEndian(m_file, 0, 8);
        m_file.flush();
        if (!m_file.good()) {
            error = L"Não foi possível preparar o contêiner MP4 da gravação.";
            m_file.close();
            return false;
        }

        m_started = true;
        return true;
    }

    bool write(
        const std::vector<std::uint8_t>& encoded,
        LONGLONG timestamp,
        std::wstring& error) {
        (void)timestamp;
        if (!m_started) {
            error = L"O contêiner MP4 não está aberto.";
            return false;
        }

        std::vector<std::uint8_t> packet;
        if (!normalizeH264ToLengthPrefixed(encoded, packet)) {
            error = L"O NVENC retornou um pacote H.264 vazio ou inválido.";
            return false;
        }
        if (packet.size() > static_cast<size_t>(UINT32_MAX)) {
            error = L"O pacote H.264 retornado pelo NVENC é grande demais.";
            return false;
        }

        const auto offset = m_dataOffset + m_dataBytes;
        m_file.seekp(0, std::ios::end);
        m_file.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
        if (!m_file.good()) {
            error = L"Não foi possível gravar o pacote H.264 no arquivo MP4.";
            return false;
        }

        m_dataBytes += packet.size();
        m_samples.push_back({offset, static_cast<std::uint32_t>(packet.size())});
        if (m_firstSample || containsH264NalType(encoded, 5)) {
            m_keyFrames.push_back(m_samples.size());
        }
        m_firstSample = false;
        return true;
    }

    bool finalize(std::wstring& error) {
        if (!m_started) {
            return true;
        }

        try {
            if (m_samples.empty()) {
                error = L"A gravação não produziu nenhum frame H.264.";
                m_file.close();
                m_started = false;
                return false;
            }

            Mp4Builder moov;
            buildMoov(moov);

            m_file.seekp(m_mdatOffset, std::ios::beg);
            writeBigEndian(m_file, 1, 4);
            m_file.write("mdat", 4);
            writeBigEndian(m_file, 16 + m_dataBytes, 8);
            m_file.seekp(0, std::ios::end);
            m_file.write(reinterpret_cast<const char*>(moov.bytes.data()),
                static_cast<std::streamsize>(moov.bytes.size()));
            m_file.flush();
            const bool good = m_file.good();
            m_file.close();
            m_started = false;
            if (!good) {
                error = L"Não foi possível finalizar o arquivo MP4 da gravação.";
                return false;
            }
            return true;
        } catch (const std::exception&) {
            m_file.close();
            m_started = false;
            error = L"Não foi possível montar o índice do arquivo MP4 da gravação.";
            return false;
        }
    }

private:
    struct SampleInfo {
        std::uint64_t offset;
        std::uint32_t size;
    };

    static bool extractParameterSets(
        const std::vector<std::uint8_t>& header,
        std::vector<std::uint8_t>& sps,
        std::vector<std::uint8_t>& pps) {
        sps.clear();
        pps.clear();
        size_t codeSize = 0;
        size_t start = findStartCode(header, 0, codeSize);
        while (start != std::numeric_limits<size_t>::max()) {
            const size_t nalStart = start + codeSize;
            size_t nextCodeSize = 0;
            const size_t next = findStartCode(header, nalStart, nextCodeSize);
            size_t nalEnd = next == std::numeric_limits<size_t>::max() ? header.size() : next;
            while (nalEnd > nalStart && header[nalEnd - 1] == 0) {
                --nalEnd;
            }
            if (nalEnd > nalStart) {
                const auto type = header[nalStart] & 0x1f;
                if (type == 7 && sps.empty()) {
                    sps.assign(header.begin() + static_cast<std::ptrdiff_t>(nalStart),
                        header.begin() + static_cast<std::ptrdiff_t>(nalEnd));
                } else if (type == 8 && pps.empty()) {
                    pps.assign(header.begin() + static_cast<std::ptrdiff_t>(nalStart),
                        header.begin() + static_cast<std::ptrdiff_t>(nalEnd));
                }
            }
            start = next;
            codeSize = nextCodeSize;
        }
        return sps.size() >= 4 && pps.size() >= 2;
    }

    static void writeIdentityMatrix(Mp4Builder& builder) {
        builder.u32(0x00010000);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0x00010000);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0x40000000);
    }

    void buildAvc1(Mp4Builder& builder) const {
        const size_t avc1 = builder.beginBox("avc1");
        builder.zeros(6);
        builder.u16(1);
        builder.u16(0);
        builder.u16(0);
        builder.zeros(12);
        builder.u16(static_cast<std::uint16_t>(m_width));
        builder.u16(static_cast<std::uint16_t>(m_height));
        builder.u32(0x00480000);
        builder.u32(0x00480000);
        builder.u32(0);
        builder.u16(1);
        builder.zeros(32);
        builder.u16(0x0018);
        builder.u16(0xffff);

        const size_t avcC = builder.beginBox("avcC");
        builder.u8(1);
        builder.u8(m_sps[1]);
        builder.u8(m_sps[2]);
        builder.u8(m_sps[3]);
        builder.u8(0xff);
        builder.u8(0xe1);
        builder.u16(static_cast<std::uint16_t>(m_sps.size()));
        builder.append(m_sps);
        builder.u8(1);
        builder.u16(static_cast<std::uint16_t>(m_pps.size()));
        builder.append(m_pps);
        builder.endBox(avcC);

        const size_t btrt = builder.beginBox("btrt");
        builder.u32(0);
        builder.u32(m_bitrate);
        builder.u32(m_bitrate);
        builder.endBox(btrt);
        builder.endBox(avc1);
    }

    void buildMoov(Mp4Builder& builder) const {
        const auto sampleCount = static_cast<std::uint32_t>(m_samples.size());
        const size_t moov = builder.beginBox("moov");

        const size_t mvhd = builder.beginBox("mvhd");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(m_timeScale);
        builder.u32(sampleCount);
        builder.u32(0x00010000);
        builder.u16(0x0100);
        builder.u16(0);
        builder.zeros(8);
        writeIdentityMatrix(builder);
        builder.zeros(24);
        builder.u32(2);
        builder.endBox(mvhd);

        const size_t trak = builder.beginBox("trak");
        const size_t tkhd = builder.beginBox("tkhd");
        builder.fullBox(0, 0x000007);
        builder.u32(0);
        builder.u32(0);
        builder.u32(1);
        builder.u32(0);
        builder.u32(sampleCount);
        builder.zeros(8);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        writeIdentityMatrix(builder);
        builder.u32(m_width << 16);
        builder.u32(m_height << 16);
        builder.endBox(tkhd);

        const size_t mdia = builder.beginBox("mdia");
        const size_t mdhd = builder.beginBox("mdhd");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(m_timeScale);
        builder.u32(sampleCount);
        builder.u16(0x55c4);
        builder.u16(0);
        builder.endBox(mdhd);

        const size_t hdlr = builder.beginBox("hdlr");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.fourcc("vide");
        builder.zeros(12);
        builder.append({
            'V', 'i', 'd', 'e', 'o', 'H', 'a', 'n', 'd', 'l', 'e', 'r', 0});
        builder.endBox(hdlr);

        const size_t minf = builder.beginBox("minf");
        const size_t vmhd = builder.beginBox("vmhd");
        builder.fullBox(0, 1);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.endBox(vmhd);

        const size_t dinf = builder.beginBox("dinf");
        const size_t dref = builder.beginBox("dref");
        builder.fullBox(0, 0);
        builder.u32(1);
        const size_t url = builder.beginBox("url ");
        builder.fullBox(0, 1);
        builder.endBox(url);
        builder.endBox(dref);
        builder.endBox(dinf);

        const size_t stbl = builder.beginBox("stbl");
        const size_t stsd = builder.beginBox("stsd");
        builder.fullBox(0, 0);
        builder.u32(1);
        buildAvc1(builder);
        builder.endBox(stsd);

        const size_t stts = builder.beginBox("stts");
        builder.fullBox(0, 0);
        builder.u32(1);
        builder.u32(sampleCount);
        builder.u32(1);
        builder.endBox(stts);

        const size_t stsc = builder.beginBox("stsc");
        builder.fullBox(0, 0);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.endBox(stsc);

        const size_t stsz = builder.beginBox("stsz");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(sampleCount);
        for (const auto& sample : m_samples) {
            builder.u32(sample.size);
        }
        builder.endBox(stsz);

        const size_t co64 = builder.beginBox("co64");
        builder.fullBox(0, 0);
        builder.u32(sampleCount);
        for (const auto& sample : m_samples) {
            builder.u64(sample.offset);
        }
        builder.endBox(co64);

        const size_t stss = builder.beginBox("stss");
        builder.fullBox(0, 0);
        builder.u32(static_cast<std::uint32_t>(m_keyFrames.size()));
        for (const auto frame : m_keyFrames) {
            builder.u32(static_cast<std::uint32_t>(frame));
        }
        builder.endBox(stss);
        builder.endBox(stbl);
        builder.endBox(minf);
        builder.endBox(mdia);
        builder.endBox(trak);
        builder.endBox(moov);
    }

    std::fstream m_file;
    std::vector<std::uint8_t> m_sps;
    std::vector<std::uint8_t> m_pps;
    std::vector<SampleInfo> m_samples;
    std::vector<size_t> m_keyFrames;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::uint32_t m_timeScale{0};
    std::uint32_t m_bitrate{0};
    std::uint64_t m_mdatOffset{0};
    std::uint64_t m_dataOffset{0};
    std::uint64_t m_dataBytes{0};
    bool m_started{false};
    bool m_firstSample{true};
};

class NvencEncoder final {
public:
    ~NvencEncoder() {
        shutdown();
    }

    bool initialize(
        ID3D11Device* device,
        ID3D11Texture2D* texture,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        std::wstring& error) {
        if (device == nullptr || texture == nullptr) {
            error = L"O dispositivo Direct3D 11 não está disponível para o NVENC.";
            return false;
        }

        m_library = std::make_unique<NvencLibrary>();
        if (m_library->handle == nullptr) {
            error = L"NVENC não disponível: nvEncodeAPI.dll não foi encontrada.";
            return false;
        }

        const auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
            GetProcAddress(m_library->handle, "NvEncodeAPICreateInstance"));
        if (createInstance == nullptr) {
            error = L"NVENC não disponível: a API do driver não foi exportada.";
            return false;
        }

        m_functions = {};
        m_functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS status = createInstance(&m_functions);
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível inicializar a API do NVENC", status, &m_functions, nullptr);
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        openParams.device = device;
        openParams.apiVersion = NVENCAPI_VERSION;
        status = m_functions.nvEncOpenEncodeSessionEx(&openParams, &m_session);
        if (status != NV_ENC_SUCCESS || m_session == nullptr) {
            error = nvencStatusMessage(L"Não foi possível abrir uma sessão NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }

        GUID presetGuid = NV_ENC_PRESET_P3_GUID;
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (m_functions.nvEncGetEncodePresetConfigEx != nullptr) {
            status = m_functions.nvEncGetEncodePresetConfigEx(
                m_session,
                NV_ENC_CODEC_H264_GUID,
                presetGuid,
                NV_ENC_TUNING_INFO_LOW_LATENCY,
                &preset);
        } else {
            status = NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        if (status != NV_ENC_SUCCESS) {
            status = m_functions.nvEncGetEncodePresetConfig(
                m_session, NV_ENC_CODEC_H264_GUID, presetGuid, &preset);
        }
        if (status != NV_ENC_SUCCESS) {
            // Some older driver branches expose the legacy presets only.
            presetGuid = NV_ENC_PRESET_P1_GUID;
            preset = {};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            if (m_functions.nvEncGetEncodePresetConfigEx != nullptr) {
                status = m_functions.nvEncGetEncodePresetConfigEx(
                    m_session,
                    NV_ENC_CODEC_H264_GUID,
                    presetGuid,
                    NV_ENC_TUNING_INFO_LOW_LATENCY,
                    &preset);
            } else {
                status = NV_ENC_ERR_UNSUPPORTED_PARAM;
            }
            if (status != NV_ENC_SUCCESS) {
                status = m_functions.nvEncGetEncodePresetConfig(
                    m_session, NV_ENC_CODEC_H264_GUID, presetGuid, &preset);
            }
        }
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível consultar o preset H.264 do NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }

        m_config = preset.presetCfg;
        m_config.version = NV_ENC_CONFIG_VER;
        m_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        m_config.gopLength = std::max<std::uint32_t>(fps * 2, 1);
        m_config.frameIntervalP = 1;
        m_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
        m_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        m_config.rcParams.averageBitRate = bitrate;
        m_config.rcParams.maxBitRate = bitrate;
        m_config.rcParams.enableLookahead = 0;
        m_config.rcParams.lookaheadDepth = 0;
        m_config.rcParams.zeroReorderDelay = 1;
        m_config.encodeCodecConfig.h264Config.idrPeriod = m_config.gopLength;
        m_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        m_config.encodeCodecConfig.h264Config.disableSPSPPS = 0;
        m_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
        m_config.encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        m_config.encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;

        NV_ENC_INITIALIZE_PARAMS initializeParams{};
        initializeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initializeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initializeParams.presetGUID = presetGuid;
        initializeParams.encodeWidth = width;
        initializeParams.encodeHeight = height;
        initializeParams.darWidth = width;
        initializeParams.darHeight = height;
        initializeParams.frameRateNum = fps;
        initializeParams.frameRateDen = 1;
        initializeParams.enableEncodeAsync = 0;
        initializeParams.enablePTD = 1;
        initializeParams.maxEncodeWidth = width;
        initializeParams.maxEncodeHeight = height;
        initializeParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        initializeParams.encodeConfig = &m_config;
        status = m_functions.nvEncInitializeEncoder(m_session, &initializeParams);
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível inicializar o encoder H.264 NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = m_functions.nvEncCreateBitstreamBuffer(m_session, &bitstream);
        if (status != NV_ENC_SUCCESS || bitstream.bitstreamBuffer == nullptr) {
            error = nvencStatusMessage(L"Não foi possível criar o buffer de saída do NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }
        m_bitstream = bitstream.bitstreamBuffer;

        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.width = width;
        resource.height = height;
        resource.pitch = 0;
        resource.subResourceIndex = 0;
        resource.resourceToRegister = texture;
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        status = m_functions.nvEncRegisterResource(m_session, &resource);
        if (status != NV_ENC_SUCCESS || resource.registeredResource == nullptr) {
            error = nvencStatusMessage(L"Não foi possível registrar a textura Direct3D 11 no NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }
        m_registeredResource = resource.registeredResource;
        m_width = width;
        m_height = height;
        m_frameDuration = kHundredNanosecondsPerSecond / fps;
        m_firstFrame = true;
        m_frameIndex = 0;
        return true;
    }

    bool encode(
        ID3D11Texture2D* texture,
        LONGLONG timestamp,
        std::vector<std::uint8_t>& packet,
        std::wstring& error) {
        packet.clear();
        if (texture == nullptr || m_session == nullptr || m_registeredResource == nullptr) {
            error = L"O encoder NVENC não está pronto para receber o frame.";
            return false;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = m_registeredResource;
        NVENCSTATUS status = m_functions.nvEncMapInputResource(m_session, &map);
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível mapear a textura para o NVENC", status, &m_functions, m_session);
            return false;
        }

        bool mapped = true;
        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputWidth = m_width;
        picture.inputHeight = m_height;
        picture.inputPitch = 0;
        picture.encodePicFlags = m_firstFrame
            ? NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS
            : 0;
        picture.frameIdx = m_frameIndex++;
        picture.inputTimeStamp = static_cast<std::uint64_t>(timestamp);
        picture.inputDuration = static_cast<std::uint64_t>(m_frameDuration);
        picture.inputBuffer = map.mappedResource;
        picture.outputBitstream = m_bitstream;
        picture.bufferFmt = map.mappedBufferFmt;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;

        status = m_functions.nvEncEncodePicture(m_session, &picture);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            return true;
        }
        if (status != NV_ENC_SUCCESS) {
            m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            error = nvencStatusMessage(L"O NVENC não conseguiu codificar o frame", status, &m_functions, m_session);
            return false;
        }
        m_firstFrame = false;

        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = m_bitstream;
        status = m_functions.nvEncLockBitstream(m_session, &lock);
        if (status == NV_ENC_SUCCESS) {
            const auto* data = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
            if (data != nullptr && lock.bitstreamSizeInBytes > 0) {
                packet.assign(data, data + lock.bitstreamSizeInBytes);
            }
            const NVENCSTATUS unlockStatus = m_functions.nvEncUnlockBitstream(m_session, m_bitstream);
            if (unlockStatus != NV_ENC_SUCCESS && error.empty()) {
                error = nvencStatusMessage(L"Não foi possível liberar o pacote do NVENC", unlockStatus, &m_functions, m_session);
                status = unlockStatus;
            }
        }
        if (mapped) {
            const NVENCSTATUS unmapStatus = m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            mapped = false;
            if (unmapStatus != NV_ENC_SUCCESS && error.empty()) {
                error = nvencStatusMessage(L"Não foi possível liberar a textura do NVENC", unmapStatus, &m_functions, m_session);
                status = unmapStatus;
            }
        }
        if (status != NV_ENC_SUCCESS) {
            if (error.empty()) {
                error = nvencStatusMessage(L"Não foi possível obter o pacote H.264 do NVENC", status, &m_functions, m_session);
            }
            return false;
        }
        return true;
    }

    bool flush(std::vector<std::uint8_t>& packet, std::wstring& error) {
        packet.clear();
        if (m_session == nullptr || m_bitstream == nullptr) {
            return true;
        }

        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        NVENCSTATUS status = m_functions.nvEncEncodePicture(m_session, &picture);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            return true;
        }
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível finalizar o fluxo do NVENC", status, &m_functions, m_session);
            return false;
        }
        return true;
    }

private:
    void shutdown() noexcept {
        if (m_session != nullptr) {
            if (m_registeredResource != nullptr && m_functions.nvEncUnregisterResource != nullptr) {
                m_functions.nvEncUnregisterResource(m_session, m_registeredResource);
                m_registeredResource = nullptr;
            }
            if (m_bitstream != nullptr && m_functions.nvEncDestroyBitstreamBuffer != nullptr) {
                m_functions.nvEncDestroyBitstreamBuffer(m_session, m_bitstream);
                m_bitstream = nullptr;
            }
            if (m_functions.nvEncDestroyEncoder != nullptr) {
                m_functions.nvEncDestroyEncoder(m_session);
            }
            m_session = nullptr;
        }
        m_library.reset();
    }

    std::unique_ptr<NvencLibrary> m_library;
    NV_ENCODE_API_FUNCTION_LIST m_functions{};
    void* m_session{nullptr};
    NV_ENC_REGISTERED_PTR m_registeredResource{nullptr};
    NV_ENC_OUTPUT_PTR m_bitstream{nullptr};
    NV_ENC_CONFIG m_config{};
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    LONGLONG m_frameDuration{0};
    std::uint32_t m_frameIndex{0};
    bool m_firstFrame{false};
};

#endif // FASTRECORD_HAS_NVENC

} // namespace

NvencBackend::~NvencBackend() {
    std::wstring ignored;
    stop(ignored);
}

bool NvencBackend::start(const RecordingSettings& settings, std::wstring& error) {
#if !FASTRECORD_HAS_NVENC
    error = L"Esta build não inclui os headers do NVENC. Reconfigure com FASTRECORD_FETCH_ENCODER_HEADERS=ON.";
    return false;
#else
    if (m_worker.joinable() || m_running.load()) {
        error = L"Já existe uma captura em andamento.";
        return false;
    }
    if (settings.outputDirectory.empty() || settings.width < 2 || settings.height < 2 ||
        settings.width > 3840 || settings.height > 2160 ||
        (settings.width % 2) != 0 || (settings.height % 2) != 0 ||
        settings.framesPerSecond == 0 || settings.framesPerSecond > 120 ||
        settings.bitrateMbps < 4 || settings.bitrateMbps > 80 ||
        settings.target.kind == CaptureTargetKind::SelectedWindow) {
        error = L"As configurações da gravação são inválidas.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(settings.outputDirectory, directoryError);
    if (directoryError) {
        error = L"Não foi possível criar a pasta de gravações.";
        return false;
    }

    {
        std::scoped_lock lock(m_mutex);
        m_outputPath = createOutputPath(settings.outputDirectory);
        m_workerError.clear();
    }
    m_stopRequested = false;
    m_paused = false;

    std::promise<std::wstring> startupResult;
    auto startupFuture = startupResult.get_future();
    try {
        m_worker = std::thread([this, settings, promise = std::move(startupResult)]() mutable {
            try {
                recordLoop(settings, std::move(promise));
            } catch (const winrt::hresult_error& failure) {
                setWorkerError(failure.message().c_str());
            } catch (...) {
                setWorkerError(L"Falha inesperada no backend NVENC.");
            }
            m_running = false;
        });
        error = startupFuture.get();
    } catch (...) {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"Não foi possível iniciar o NVENC." : m_workerError;
        return false;
    }
    if (!error.empty()) {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        return false;
    }
    return true;
#endif
}

bool NvencBackend::pause(std::wstring& error) {
    if (!m_running.load() || m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está ativa." : m_workerError;
        return false;
    }
    m_paused = true;
    return true;
}

bool NvencBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está pausada." : m_workerError;
        return false;
    }
    m_paused = false;
    return true;
}

bool NvencBackend::stop(std::wstring& error) {
    if (!m_worker.joinable()) {
        error = L"Não há uma captura ativa.";
        return false;
    }

    m_stopRequested = true;
    m_paused = false;
    m_worker.join();

    std::scoped_lock lock(m_mutex);
    error = m_workerError;
    return error.empty();
}

std::filesystem::path NvencBackend::outputPath() const {
    std::scoped_lock lock(m_mutex);
    return m_outputPath;
}

void NvencBackend::setWorkerError(std::wstring error) {
    std::scoped_lock lock(m_mutex);
    if (m_workerError.empty()) {
        m_workerError = std::move(error);
    }
}

void NvencBackend::recordLoop(
    RecordingSettings settings,
    std::promise<std::wstring> startupResult) {
    bool startupReported = false;
    auto reportStartup = [&](std::wstring message) {
        if (!startupReported) {
            startupResult.set_value(std::move(message));
            startupReported = true;
        }
    };

#if FASTRECORD_HAS_NVENC
    try {
        MediaRuntime runtime;
        const auto width = settings.width;
        const auto height = settings.height;
        const auto fps = settings.framesPerSecond;
        const auto bitrate = settings.bitrateMbps * 1'000'000u;
        const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;

        auto capture = std::make_unique<GraphicsCapture>(settings);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->update()) {
            if (m_stopRequested || std::chrono::steady_clock::now() >= deadline) {
                throw winrt::hresult_error(E_ABORT, L"O Windows não entregou frames do monitor.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        NvencEncoder encoder;
        std::wstring encoderError;
        if (!encoder.initialize(capture->device(), capture->texture(), width, height, fps, bitrate, encoderError)) {
            reportStartup(encoderError);
            setWorkerError(encoderError);
            return;
        }

        std::vector<std::uint8_t> firstPacket;
        LONGLONG timestamp = 0;
        std::wstring loopError;
        for (std::uint32_t attempt = 0; attempt < 4 && firstPacket.empty(); ++attempt) {
            if (attempt > 0) {
                if (!capture->update()) {
                    continue;
                }
                timestamp = static_cast<LONGLONG>(attempt) * frameDuration;
            }
            if (!encoder.encode(capture->texture(), timestamp, firstPacket, loopError)) {
                reportStartup(loopError);
                setWorkerError(loopError);
                return;
            }
        }
        if (firstPacket.empty()) {
            loopError = L"O NVENC não produziu o primeiro frame H.264.";
            reportStartup(loopError);
            setWorkerError(loopError);
            return;
        }

        std::vector<std::uint8_t> sequenceHeader;
        if (!extractH264SequenceHeader(firstPacket, sequenceHeader)) {
            loopError = L"O primeiro pacote H.264 do NVENC não contém SPS e PPS.";
            reportStartup(loopError);
            setWorkerError(loopError);
            return;
        }

        H264Mp4Writer writer;
        const auto output = outputPath();
        std::wstring writerError;
        if (!writer.start(output, width, height, fps, bitrate, sequenceHeader, writerError)) {
            reportStartup(writerError);
            setWorkerError(writerError);
            return;
        }
        if (!writer.write(firstPacket, timestamp, writerError)) {
            reportStartup(writerError);
            setWorkerError(writerError);
            return;
        }

        m_running = true;
        reportStartup({});

        timestamp += frameDuration;
        auto nextFrame = std::chrono::steady_clock::now();
        while (!m_stopRequested.load()) {
            if (m_paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                nextFrame = std::chrono::steady_clock::now();
                continue;
            }

            if (!capture->update()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            std::vector<std::uint8_t> packet;
            if (!encoder.encode(capture->texture(), timestamp, packet, loopError)) {
                break;
            }
            if (!packet.empty() && !writer.write(packet, timestamp, loopError)) {
                break;
            }

            timestamp += frameDuration;
            nextFrame += std::chrono::nanoseconds(frameDuration * 100);
            const auto now = std::chrono::steady_clock::now();
            if (nextFrame > now) {
                std::this_thread::sleep_until(nextFrame);
            } else if (now - nextFrame > std::chrono::milliseconds(250)) {
                nextFrame = now;
            }
        }

        std::vector<std::uint8_t> finalPacket;
        if (loopError.empty() && !encoder.flush(finalPacket, loopError)) {
            // The encoder error is reported below together with the finalize error.
        }
        if (loopError.empty() && !finalPacket.empty() && !writer.write(finalPacket, timestamp, loopError)) {
            // Keep the first meaningful error.
        }
        std::wstring finalizeError;
        if (!writer.finalize(finalizeError) && loopError.empty()) {
            loopError = std::move(finalizeError);
        }
        if (!loopError.empty()) {
            setWorkerError(std::move(loopError));
        }
    } catch (const winrt::hresult_error& failure) {
        const std::wstring message = failure.message().c_str();
        reportStartup(message);
        setWorkerError(message);
    } catch (...) {
        reportStartup(L"Falha inesperada no backend NVENC.");
        setWorkerError(L"Falha inesperada no backend NVENC.");
    }
#else
    reportStartup(L"Esta build não inclui suporte ao NVENC.");
#endif
}

} // namespace fastrecord::recording
