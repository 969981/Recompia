// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] one_submit park (nvn_submit_park.h): every parked flush has exactly one owner (its worker job or the present), the present takes
// only the not-released suffix, a CPU wait releases (also a flush parked after it), the hold waits only while a present is coming and ends
// when the present takes the flush, and with the submit worker the queue sees every serial exactly once and in increasing order.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "nvn_submit_park.h"
#include "nvn_submit_worker.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_submit_park)

using nxrt::nvn::be::gpu::SubmitPark;
using nxrt::nvn::be::gpu::SubmitWorker;
using Park = SubmitPark<int>;
using namespace std::chrono_literals;

NXTEST(submit_park_job_takes_its_own_entry_without_hold) {
    Park p;
    p.park(1, 3, 30);
    Park::Took how = Park::Took::Present;
    auto v = p.take_for_job(3, 0ns, true, &how);
    CHECK(v.has_value());
    CHECK_EQ(*v, 30);
    CHECK(how == Park::Took::Now);
    CHECK(p.take_for_present().empty());
    CHECK_EQ(p.size(), size_t{0});
}

NXTEST(submit_park_present_takes_entries_in_order_and_the_jobs_find_them_gone) {
    Park p;
    p.park(1, 2, 20);
    p.park(3, 5, 50);
    p.park(6, 6, 60);
    auto t = p.take_for_present();
    CHECK_EQ(t.size(), size_t{3});
    CHECK_EQ(t[0].serial, uint64_t{2});
    CHECK_EQ(t[1].serial, uint64_t{5});
    CHECK_EQ(t[2].serial, uint64_t{6});
    CHECK_EQ(t[1].payload, 50);
    for (uint64_t s : {2ull, 5ull, 6ull}) {
        Park::Took how = Park::Took::Now;
        CHECK(!p.take_for_job(s, 0ns, false, &how).has_value());
        CHECK(how == Park::Took::Present);
    }
}

NXTEST(submit_park_present_never_takes_a_released_prefix) {
    Park p;
    p.park(1, 2, 20);
    p.park(3, 5, 50);
    p.park(6, 8, 80);
    p.release(4);       // a wait for serial 4: the entry 3..5 (and everything before it) must go on its own
    auto t = p.take_for_present();
    CHECK_EQ(t.size(), size_t{1});
    CHECK_EQ(t[0].serial, uint64_t{8});
    Park::Took how = Park::Took::Present;
    CHECK(p.take_for_job(2, 50ms, true, &how).has_value());    // released: no hold even with a present coming
    CHECK(how == Park::Took::Now);
    CHECK(p.take_for_job(5, 50ms, true, &how).has_value());
    CHECK(how == Park::Took::Now);
}

NXTEST(submit_park_a_release_before_the_flush_still_counts) {
    Park p;
    p.release(7);       // the waiter got there before the render thread flushed serial 7
    p.park(5, 9, 90);   // holds 7
    CHECK(p.take_for_present().empty());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(p.take_for_job(9, 200ms, true).has_value());
    CHECK(std::chrono::steady_clock::now() - t0 < 100ms);
    p.park(10, 12, 120);    // past the released serial: the present may take it
    CHECK_EQ(p.take_for_present().size(), size_t{1});
}

NXTEST(submit_park_release_all_covers_only_what_is_parked) {
    Park p;
    p.park(1, 4, 40);
    p.release_all();
    p.park(5, 6, 60);
    auto t = p.take_for_present();
    CHECK_EQ(t.size(), size_t{1});
    CHECK_EQ(t[0].serial, uint64_t{6});
    CHECK(p.take_for_job(4, 0ns, false).has_value());
}

NXTEST(submit_park_hold_waits_only_while_a_present_is_coming) {
    Park p;
    p.park(1, 1, 10);
    const auto t0 = std::chrono::steady_clock::now();
    Park::Took how = Park::Took::Present;
    CHECK(p.take_for_job(1, 200ms, false, &how).has_value());   // no present queued: at once
    CHECK(how == Park::Took::Now);
    CHECK(std::chrono::steady_clock::now() - t0 < 100ms);

    p.park(2, 2, 20);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(p.take_for_job(2, 30ms, true, &how).has_value());     // nobody takes it: the hold runs out
    CHECK(how == Park::Took::Expired);
    CHECK(std::chrono::steady_clock::now() - t1 >= 25ms);
}

NXTEST(submit_park_hold_ends_when_the_present_takes_or_a_wait_releases) {
    Park p;
    p.park(1, 3, 30);
    std::vector<Park::Entry> got;
    std::thread present([&] { std::this_thread::sleep_for(10ms); got = p.take_for_present(); });
    const auto t0 = std::chrono::steady_clock::now();
    Park::Took how = Park::Took::Now;
    CHECK(!p.take_for_job(3, 2000ms, true, &how).has_value());
    CHECK(how == Park::Took::Present);
    CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
    present.join();
    CHECK_EQ(got.size(), size_t{1});

    p.park(4, 4, 40);
    std::thread waiter([&] { std::this_thread::sleep_for(10ms); p.release(4); });
    const auto t1 = std::chrono::steady_clock::now();
    auto v = p.take_for_job(4, 2000ms, true, &how);
    CHECK(v.has_value());
    CHECK(how == Park::Took::Released);
    CHECK(std::chrono::steady_clock::now() - t1 < 1000ms);
    waiter.join();
}

// The render thread / submit worker / present model of nvn_gpu.cpp without Vulkan: flushes park and push a job; a present takes what is
// parked, then drains and pushes its own job, which submits the taken flushes ahead of its own batch; waiters release. The "queue" must see
// every serial once, in increasing order, whatever the timing.
NXTEST(submit_park_with_the_worker_keeps_the_queue_order) {
    for (int round = 0; round < 6; round++) {    // (Windows sleeps and timed waits round up to the timer tick: kept short)
        SubmitPark<std::vector<uint64_t>> park;
        SubmitWorker w;
        w.start();
        std::mutex qm;
        std::vector<uint64_t> queue;        // serials in submission order
        auto submit = [&](const std::vector<uint64_t>& s) {
            std::lock_guard<std::mutex> lk(qm);
            queue.insert(queue.end(), s.begin(), s.end());
        };
        std::atomic<bool> coming{false};
        std::mt19937 rng(static_cast<unsigned>(round * 7919 + 1));
        uint64_t next = 1, flushed = 0;
        std::vector<uint64_t> batch;
        std::atomic<uint64_t> recorded{0};
        std::atomic<bool> done{false};
        std::thread waiter([&] {    // a game thread waiting for recent serials now and then
            std::mt19937 r2(static_cast<unsigned>(round + 99));
            while (!done.load()) {
                const uint64_t s = recorded.load();
                if (s > 2) park.release(s - r2() % 3);
                std::this_thread::sleep_for(std::chrono::microseconds(200 + r2() % 400));
            }
        });
        for (int frame = 0; frame < 40; frame++) {
            const int subs = 3 + static_cast<int>(rng() % 10);
            coming.store(rng() % 2 == 0);    // the game queued its present early (or not)
            for (int i = 0; i < subs; i++) {
                batch.push_back(next);
                recorded.store(next);
                next++;
                if (rng() % 4 == 0) {   // a poll-forced flush
                    const uint64_t last = batch.back();
                    park.park(flushed + 1, last, batch);
                    flushed = last;
                    batch.clear();
                    const bool c = coming.load();
                    const auto hold = std::chrono::milliseconds(rng() % 3);
                    w.push([&park, &submit, last, c, hold] {
                        auto v = park.take_for_job(last, hold, c);
                        if (v) submit(*v);
                    });
                }
                if (rng() % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
                else std::this_thread::yield();
            }
            // the present: take, drain (the previous present), then one job with taken + batch + its own
            auto taken = park.take_for_present();
            w.drain();
            std::vector<uint64_t> all;
            for (auto& e : taken) all.insert(all.end(), e.payload.begin(), e.payload.end());
            all.insert(all.end(), batch.begin(), batch.end());
            if (!batch.empty()) flushed = batch.back();
            batch.clear();
            coming.store(false);
            w.push([&submit, all] { submit(all); });
        }
        w.drain();
        done.store(true);
        waiter.join();
        w.stop();
        CHECK_EQ(queue.size(), static_cast<size_t>(next - 1));
        bool ordered = true;
        for (size_t i = 0; i < queue.size(); i++)
            if (queue[i] != i + 1) ordered = false;
        CHECK(ordered);
    }
}
