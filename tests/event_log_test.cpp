// The network log's ring (net/event_log.h): order, wrap, cut lines, timestamps.
#include "net/event_log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using net::eventlog::Event;
using net::eventlog::Level;
using net::eventlog::Log;

namespace {
int g_fail = 0;
#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)
}  // namespace

int main() {
    {
        Log log;
        Event out[4];
        CHECK(log.generation() == 0);
        CHECK(log.last(out, 4) == 0);
        log.add(Level::Good, 2 * 3600 + 3 * 60 + 45, "room 7 created");
        CHECK(log.generation() == 1);
        std::uint64_t gen = 0;
        CHECK(log.last(out, 4, &gen) == 1);
        CHECK(gen == 1);
        CHECK(out[0].level == Level::Good);
        CHECK(std::strcmp(out[0].text, "02:03:45 room 7 created") == 0);
    }
    {
        // Past the ring's size: the newest, oldest first, up to what was asked.
        Log log;
        const int total = Log::kKept + 5;
        for (int i = 0; i < total; ++i) log.add(Level::Info, 0, std::to_string(i).c_str());
        Event out[Log::kKept + 8];
        CHECK(log.last(out, Log::kKept + 8) == Log::kKept);
        CHECK(std::strcmp(out[0].text, "00:00:00 5") == 0);
        CHECK(std::strcmp(out[Log::kKept - 1].text, ("00:00:00 " + std::to_string(total - 1)).c_str()) == 0);
        Event few[3];
        CHECK(log.last(few, 3) == 3);
        CHECK(std::strcmp(few[0].text, ("00:00:00 " + std::to_string(total - 3)).c_str()) == 0);
        CHECK(std::strcmp(few[2].text, ("00:00:00 " + std::to_string(total - 1)).c_str()) == 0);
    }
    {
        // A long line is cut and stays terminated.
        Log log;
        const std::string longer(500, 'x');
        log.add(Level::Bad, 86399, longer.c_str());
        Event out[1];
        CHECK(log.last(out, 1) == 1);
        CHECK(std::strlen(out[0].text) == sizeof(out[0].text) - 1);
        CHECK(std::strncmp(out[0].text, "23:59:59 xxx", 12) == 0);
    }
    {
        // note() formats and reaches the one log.
        const std::uint64_t before = net::eventlog::log().generation();
        net::eventlog::note(Level::Warn, "no data from %s for %d s", "Someone", 10);
        CHECK(net::eventlog::log().generation() == before + 1);
        Event out[1];
        CHECK(net::eventlog::log().last(out, 1) == 1);
        CHECK(out[0].level == Level::Warn);
        CHECK(std::strstr(out[0].text, " no data from Someone for 10 s") != nullptr);
    }
    {
        // Writers on several threads: every event is counted and each line whole.
        Log log;
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&log] {
                for (int i = 0; i < 1000; ++i) log.add(Level::Info, 0, "whole");
            });
        }
        Event out[Log::kKept];
        for (int i = 0; i < 200; ++i) {
            const int n = log.last(out, Log::kKept);
            for (int k = 0; k < n; ++k) CHECK(std::strcmp(out[k].text, "00:00:00 whole") == 0);
        }
        for (std::thread& th : threads) th.join();
        CHECK(log.generation() == 4000);
    }
    if (g_fail) return 1;
    std::printf("event_log_test: ok\n");
    return 0;
}
