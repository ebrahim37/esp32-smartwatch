#include <atomic>
#include <thread>
#include <vector>

#include "ble/fetch.hpp"
#include "minitest.hpp"

namespace swlink {
bool ready() { return true; }
bool enqueue(uint8_t, std::vector<uint8_t>&&) { return true; }
}  // namespace swlink

TEST(fetch_limits_simultaneous_requests_and_completes_each_once) {
    for (int round = 0; round < 16; ++round) {
        std::atomic<bool> go{false};
        std::atomic<unsigned> started{0}, accepted{0}, callbacks{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < 32; ++i) workers.emplace_back([&] {
            fetch::Request req;
            req.url = "https://example.invalid/test";
            req.body = std::string(2048, 'b');
            req.headers = {{"Authorization", std::string(1024, 'h')}};
            ++started;
            while (!go.load()) std::this_thread::yield();
            if (fetch::request(std::move(req), [&](fetch::Response&& r) {
                    CHECK_EQ(r.status, 0);
                    CHECK_EQ(r.error, std::string("companion disconnected"));
                    ++callbacks;
                })) ++accepted;
        });
        while (started.load() != 32) std::this_thread::yield();
        go.store(true);
        for (auto& t : workers) t.join();
        CHECK_EQ(accepted.load(), 4u);
        fetch::on_link_down();
        CHECK_EQ(callbacks.load(), accepted.load());
    }
}

int main() { return minitest::run_all(); }
