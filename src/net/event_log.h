#pragma once

// The network log the F10 option "Network log" draws (host/window.cpp): the
// last matchmaking and connection events, in words, one line each. Rooms, who
// joined and left, connection attempts and how they ended. Events are rare
// (a few a minute) and always kept, so turning the display on shows what just
// happened. Lines name players by online id and give ports only:
// the display may end up in a stream or a recording. The log file has the same
// events in its own np:, net: punch and stun: lines.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace net::eventlog {

enum class Level : std::uint8_t { Info, Good, Warn, Bad };

struct Event {
    Level level = Level::Info;
    char text[112] = "";  // "HH:MM:SS what happened"; a longer line is cut
};

class Log {
public:
    static constexpr int kKept = 32;

    void add(Level level, int seconds_of_day, const char* what) {
        std::lock_guard<std::mutex> lk(mu_);
        Event& e = events_[generation_ % kKept];
        e.level = level;
        std::snprintf(e.text, sizeof(e.text), "%02d:%02d:%02d %s", seconds_of_day / 3600, seconds_of_day / 60 % 60,
                      seconds_of_day % 60, what);
        ++generation_;
    }

    // Copies the newest `max` events, oldest first, and returns how many.
    int last(Event* out, int max, std::uint64_t* generation = nullptr) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (generation) *generation = generation_;
        std::uint64_t n = generation_ < kKept ? generation_ : kKept;
        if (n > static_cast<std::uint64_t>(max)) n = static_cast<std::uint64_t>(max);
        for (std::uint64_t i = 0; i < n; ++i) out[i] = events_[(generation_ - n + i) % kKept];
        return static_cast<int>(n);
    }

    std::uint64_t generation() const {
        std::lock_guard<std::mutex> lk(mu_);
        return generation_;
    }

private:
    mutable std::mutex mu_;
    Event events_[kKept];
    std::uint64_t generation_ = 0;  // events ever added
};

// The one log; never destroyed, since detached threads write to it.
inline Log& log() {
    static Log* l = new Log;
    return *l;
}

// Adds an event stamped with the local time.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void note(Level level, const char* fmt, ...) {
    char what[112];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    log().add(level, tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec, what);
}

}  // namespace net::eventlog
