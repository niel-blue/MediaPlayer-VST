#include "MediaPlayerDebugLog.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace MediaPlayer {
namespace {

std::mutex logMutex;
std::ofstream logStream;

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime {};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localTime = *std::localtime(&time);
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream result;
    result << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count();
    return result.str();
}

std::filesystem::path logPath()
{
    try {
        return std::filesystem::temp_directory_path() /
               "DAWMediaPlayer-debug.log";
    } catch (...) {
        return "DAWMediaPlayer-debug.log";
    }
}

} // namespace

void DebugLog::write(std::string_view message) noexcept
{
    try {
        std::lock_guard lock(logMutex);
        if (!logStream.is_open()) {
            logStream.open(logPath(), std::ios::out | std::ios::app);
            if (!logStream.is_open())
                return;

            logStream << "\n--- DAW Media Player diagnostic session ---\n";
        }

        logStream << timestamp()
                  << " [thread=" << std::this_thread::get_id() << "] "
                  << message << '\n';
        logStream.flush();
    } catch (...) {
        // Diagnostics must never affect playback if the log cannot be written.
    }
}

} // namespace MediaPlayer
