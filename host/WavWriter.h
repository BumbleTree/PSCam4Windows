#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <windows.h>

// Exclusive PCM WAV creation. Long recordings are segmented before RIFF's
// 32-bit size limit; each segment is finalized before opening the next one.
class WavWriter
{
public:
    explicit WavWriter(uint32_t segmentBytes = 0xFFFF0000u) : _limit(segmentBytes) {}
    ~WavWriter() { Close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool Open(const wchar_t* path, uint32_t sampleRate, uint16_t channels)
    {
        Close();
        _writeFailed = false; _totalFrames = 0; _segment = 0;
        if (!path || !*path || !channels || channels > 4 || !sampleRate ||
            sampleRate > UINT32_MAX / (channels * 2u) ||
            _limit < channels * 2u || _limit > UINT32_MAX - 36u)
            return false;
        _path = path; _channels = channels; _sampleRate = sampleRate;
        return OpenSegment();
    }
    bool IsOpen() const { return _f != nullptr; }
    bool WriteFailed() const { return _writeFailed; }
    double Seconds() const { return _sampleRate ? double(_totalFrames) / _sampleRate : 0.0; }

    void Write(const int16_t* samples, uint32_t frames)
    {
        if (!_f || !frames || _writeFailed) return;
        if (!samples) { _writeFailed = true; return; }
        const uint32_t align = _channels * 2u;
        const uint64_t capacity = _limit / align;
        while (frames && !_writeFailed)
        {
            if (_frames == capacity)
            {
                CloseSegment();
                if (_writeFailed) break;
                ++_segment;
                if (!OpenSegment()) { _writeFailed = true; break; }
            }
            const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(frames, capacity - _frames));
            const size_t wrote = fwrite(samples, align, count, _f);
            _frames += wrote; _totalFrames += wrote;
            if (wrote != count) { _writeFailed = true; break; }
            samples += static_cast<size_t>(count) * _channels;
            frames -= count;
        }
    }
    void Close() { if (_f) CloseSegment(); }

private:
    void MarkStart()
    {
        LARGE_INTEGER f{}, t{};
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        _qpcFreq = f.QuadPart; _qpcStart = t.QuadPart;
    }
    // The rate this segment's samples really arrived at, by wall clock, so the
    // finalized header can carry it instead of the nominal rate (a source that
    // runs fast would otherwise play back slow). 0 until there is at least a
    // second of audio to measure, or when the result is implausible (+/-10 %).
    uint32_t MeasuredRate() const
    {
        if (!_qpcFreq || !_qpcStart || _frames < 48000) return 0;
        LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
        const double secs = double(t.QuadPart - _qpcStart) / double(_qpcFreq);
        if (secs < 1.0) return 0;
        const double r = double(_frames) / secs;
        return r > _sampleRate * 0.9 && r < _sampleRate * 1.1 ? uint32_t(r + 0.5) : 0;
    }
    bool OpenSegment()
    {
        std::wstring name = _path;
        if (_segment)
        {
            const size_t ext = name.find_last_of(L'.');
            const size_t slash = name.find_last_of(L"\\/");
            name.insert(ext == std::wstring::npos || (slash != std::wstring::npos && ext < slash) ? name.size() : ext,
                        L"-part" + std::to_wstring(_segment + 1));
        }
        HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const intptr_t fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_WRONLY | _O_BINARY);
        if (fd == -1) { CloseHandle(file); return false; }
        _f = _fdopen(static_cast<int>(fd), "wb");
        if (!_f) { _close(static_cast<int>(fd)); return false; }
        _frames = 0; MarkStart();
        const uint16_t align = static_cast<uint16_t>(_channels * 2);
        const bool ok = Tag("RIFF") && U32(36) && Tag("WAVE") && Tag("fmt ") &&
            U32(16) && U16(1) && U16(_channels) && U32(_sampleRate) &&
            U32(_sampleRate * align) && U16(align) && U16(16) && Tag("data") && U32(0);
        if (!ok || fflush(_f) != 0)
        { _writeFailed = true; fclose(_f); _f = nullptr; return false; }
        return true;
    }
    void CloseSegment()
    {
        const uint32_t bytes = static_cast<uint32_t>(_frames * _channels * 2);
        if (fseek(_f, 4, SEEK_SET) != 0 || !U32(36 + bytes)) _writeFailed = true;
        const uint32_t rate = MeasuredRate();
        if (rate && (fseek(_f, 24, SEEK_SET) != 0 || !U32(rate) || !U32(rate * _channels * 2u)))
            _writeFailed = true;
        if (fseek(_f, 40, SEEK_SET) != 0 || !U32(bytes)) _writeFailed = true;
        if (fclose(_f) != 0) _writeFailed = true;
        _f = nullptr;
    }
    bool Tag(const char* tag) { return fwrite(tag, 1, 4, _f) == 4; }
    bool U32(uint32_t v) { return fwrite(&v, 4, 1, _f) == 1; }
    bool U16(uint16_t v) { return fwrite(&v, 2, 1, _f) == 1; }
    FILE* _f = nullptr;
    std::wstring _path;
    uint32_t _limit, _sampleRate = 0, _segment = 0;
    uint16_t _channels = 0;
    uint64_t _frames = 0, _totalFrames = 0;
    bool _writeFailed = false;
    long long _qpcFreq = 0, _qpcStart = 0;
};
