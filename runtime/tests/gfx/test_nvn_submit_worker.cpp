// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] submit_thread worker (nvn_submit_worker.h): jobs run in FIFO order on one thread, wait_submitted returns only after the job that
// publishes the serial has run (never early), drain waits for everything pushed before it, and stop runs what is queued.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "nvn_submit_worker.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_submit_worker)

using nxrt::nvn::be::gpu::SubmitWorker;

NXTEST(submit_worker_runs_jobs_in_fifo_order) {
    SubmitWorker w;
    w.start();
    std::vector<int> order;     // only the worker writes it; read after drain
    for (int i = 0; i < 200; i++) w.push([&order, i] { order.push_back(i); });
    w.drain();
    CHECK_EQ(order.size(), size_t{200});
    for (int i = 0; i < 200; i++) CHECK_EQ(order[i], i);
    w.stop();
}

NXTEST(submit_worker_wait_submitted_never_returns_before_the_publish) {
    SubmitWorker w;
    w.start();
    std::atomic<uint64_t> ran{0};       // the "vkQueueSubmit2" of each serial: set right before its publish
    for (uint64_t s = 1; s <= 50; s++)
        w.push([&w, &ran, s] {
            if (s % 10 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ran.store(s);
            w.publish(s);
        });
    bool early = false;
    std::thread waiter([&] {
        for (uint64_t s = 1; s <= 50; s++) {
            if (!w.wait_submitted(s, UINT64_MAX, [] { return false; })) { early = true; return; }
            if (ran.load() < s) early = true;     // returned before the job of `s` had run
        }
    });
    waiter.join();
    CHECK(!early);
    CHECK_EQ(w.submitted(), uint64_t{50});
    w.stop();
}

NXTEST(submit_worker_wait_submitted_times_out_and_honours_the_other_way) {
    SubmitWorker w;
    w.start();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!w.wait_submitted(7, 20ull * 1000 * 1000, [] { return false; }));     // nothing publishes 7: times out (20 ms)
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(19));
    std::atomic<bool> lost{false};
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); lost.store(true); });
    CHECK(w.wait_submitted(7, UINT64_MAX, [&] { return lost.load(); }));       // completed another way (device lost): released
    t.join();
    w.stop();
}

NXTEST(submit_worker_drain_and_stop_run_everything_pushed) {
    SubmitWorker w;
    w.start();
    std::atomic<int> n{0};
    for (int i = 0; i < 20; i++) w.push([&n] { std::this_thread::sleep_for(std::chrono::microseconds(200)); n.fetch_add(1); });
    w.drain();
    CHECK_EQ(n.load(), 20);
    for (int i = 0; i < 20; i++) w.push([&n] { n.fetch_add(1); });
    w.stop();
    CHECK_EQ(n.load(), 40);
    w.push([&n] { n.fetch_add(1); });     // after stop: inline on the caller
    CHECK_EQ(n.load(), 41);
    w.drain();                            // no worker: returns at once
}
