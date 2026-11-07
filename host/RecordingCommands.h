#pragma once
#include <cstdint>
#include <mutex>
#include <string>

// The UI -> capture-thread recording handshake. Every Start/Stop bumps a
// generation; the capture thread acts on the newest request it sees and
// acknowledges the outcome for that generation only, so an acknowledgement of
// a superseded request can never overwrite the state of a newer one.
class RecordingCommands
{
public:
    enum class State { Stopped, Starting, Recording, Stopping, Failed };
    struct Request { uint64_t generation = 0; bool start = false; std::wstring path; };
    void Start(std::wstring path)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_request.generation; _request.start = true; _request.path = std::move(path);
        _state = State::Starting;
    }
    void Stop()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_request.generation; _request.start = false; _state = State::Stopping;
    }
    Request Snapshot() const
    { std::lock_guard<std::mutex> lock(_mutex); return _request; }
    State Status() const
    { std::lock_guard<std::mutex> lock(_mutex); return _state; }
    void Acknowledge(uint64_t generation, State state)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_request.generation != generation) return;
        _state = state;
        if (state == State::Failed || state == State::Stopped) _request.start = false;
    }
private:
    mutable std::mutex _mutex;
    Request _request;
    State _state = State::Stopped;
};
