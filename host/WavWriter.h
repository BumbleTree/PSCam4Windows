#pragma once
//
// WavWriter — minimal RIFF/PCM writer for the camera microphone capture.
//
// Header-only on purpose: it is a few dozen lines, it has no dependencies beyond
// <cstdio>, and adding a .cpp would mean touching build.bat (which lists every
// source file by hand). Writes a placeholder header up front and patches the two
// size fields on Close(), so a recording that is interrupted by a crash still
// leaves a file most players can open.
//
#include <cstdint>
#include <cstdio>
#include <string>
#include <windows.h>   // QueryPerformanceCounter, for the true-rate patch

class WavWriter
{
public:
    ~WavWriter() { Close(); }

    bool Open(const wchar_t* path, uint32_t sampleRate, uint16_t channels)
    {
        Close();
        if (_wfopen_s(&_f, path, L"wb") != 0 || !_f)
            return false;
        _channels    = channels;
        _sampleRate  = sampleRate;
        _frames      = 0;
        _writeFailed = false;
        MarkStart();

        const uint16_t bits  = 16;
        const uint16_t align = (uint16_t)(channels * bits / 8);
        WriteTag("RIFF");  WriteU32(0);            // patched on Close
        WriteTag("WAVE");  WriteTag("fmt ");
        WriteU32(16);                              // PCM fmt chunk size
        WriteU16(1);                               // WAVE_FORMAT_PCM
        WriteU16(channels);
        WriteU32(sampleRate);
        WriteU32(sampleRate * align);              // byte rate
        WriteU16(align);
        WriteU16(bits);
        WriteTag("data");  WriteU32(0);            // patched on Close
        return true;
    }

    bool IsOpen() const { return _f != nullptr; }

    // The nominal rate is a LABEL, and on the PS4 it is the wrong one. The
    // camera runs at 60.029 fps, not 60.000, so the DSP's 800 samples per frame
    // are 48,023 real samples a second while the header says 48,000 — the file
    // plays 483 ppm slow. Worse, the error is per mode: the two
    // 640x400 modes share a line rate that differs from 1280x800@60 by 533 ppm.
    //
    // Rather than guess or tabulate, measure it: frames written over elapsed
    // wall time IS the rate that makes the file play back in the time it took
    // to record. Patched on Close alongside the two size fields, so it uses the
    // whole recording rather than a warm-up estimate.
    void MarkStart()
    {
        LARGE_INTEGER f{}, t{};
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        _qpcFreq = f.QuadPart; _qpcStart = t.QuadPart;
    }
    uint32_t MeasuredRate() const
    {
        if (!_qpcFreq || !_qpcStart || _frames < 48000) return 0;   // <1 s: too short
        LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
        const double secs = double(t.QuadPart - _qpcStart) / double(_qpcFreq);
        if (secs < 1.0) return 0;
        const double r = (double)_frames / secs;
        // Refuse anything implausible; a wrong rate is worse than a nominal one.
        return (r > _sampleRate * 0.9 && r < _sampleRate * 1.1) ? (uint32_t)(r + 0.5) : 0;
    }

    // `frames` interleaved sample frames of `channels` int16 each.
    //
    // Counts what LANDED: Close() patches the sizes from _frames, so counting a
    // short write would claim more audio than the file holds.
    void Write(const int16_t* samples, uint32_t frames)
    {
        if (!_f || !frames)
            return;
        const size_t wrote = fwrite(samples, sizeof(int16_t) * _channels, frames, _f);
        _frames += wrote;
        if (wrote != frames)
            _writeFailed = true;
    }

    // True if any write was short, or the size patch-up in Close() failed.
    bool WriteFailed() const { return _writeFailed; }

    double   Seconds() const { return _sampleRate ? (double)_frames / _sampleRate : 0.0; }

    void Close()
    {
        if (!_f)
            return;
        const uint32_t dataBytes = (uint32_t)(_frames * _channels * sizeof(int16_t));
        if (fseek(_f, 4, SEEK_SET) != 0 || !WriteU32(36 + dataBytes))   // RIFF size
            _writeFailed = true;
        // Patch the sample rate (offset 24) and byte rate (28) to what was
        // actually delivered, so the recording plays at the speed it happened.
        const uint32_t rate = MeasuredRate();
        if (rate)
        {
            _sampleRate = rate;
            const uint16_t align = (uint16_t)(_channels * 16 / 8);
            if (fseek(_f, 24, SEEK_SET) != 0 || !WriteU32(rate) ||
                !WriteU32(rate * align))
                _writeFailed = true;
        }
        if (fseek(_f, 40, SEEK_SET) != 0 || !WriteU32(dataBytes))       // data size
            _writeFailed = true;
        if (fclose(_f) != 0)
            _writeFailed = true;
        _f = nullptr;
    }

private:
    void WriteTag(const char* t) { fwrite(t, 1, 4, _f); }
    bool WriteU32(uint32_t v)    { return fwrite(&v, 4, 1, _f) == 1; }
    void WriteU16(uint16_t v)    { fwrite(&v, 2, 1, _f); }

    FILE*    _f = nullptr;
    uint16_t _channels = 0;
    uint32_t _sampleRate = 0;
    uint64_t _frames = 0;
    bool     _writeFailed = false;
    long long _qpcFreq = 0, _qpcStart = 0;
};
