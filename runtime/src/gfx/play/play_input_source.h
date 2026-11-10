// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the hid input source (PlaySource) and the pad and touch state it publishes. Internal to play*.cpp.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "nxrt/frame_limiter.h"
#include "nxrt/input_source.h"
#include "pad_orient.h"
#include "play_engine.h"

namespace nxrt::play {
namespace detail {

struct PadFrame {
    uint64_t buttons = 0;
    hid::StickValue left, right;
    hid::SixAxisSample six;
};

struct Touch {
    bool down = false;
    uint32_t x = 0, y = 0;     // on the 1280 x 720 touch screen
};

// ---- the hid input source ------------------------------------------------------------------------------------------------------------
class PlaySource : public hid::InputSource {
public:
    explicit PlaySource(Engine* e) : engine_(e) {}
    void poll(hid::InputSnapshot& out) override {
        const EngineOut kb = engine_->output(FrameLimiter::now_ns());
        PadFrame pf;
        Touch t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pf = pad_;
            t = touch_;
        }
        hid::NpadInput& n = out.npad[0];
        n.connected = true;
        n.style = hid::NpadStyle::FullKey;
        n.buttons = pf.buttons | kb.buttons;
        n.left = combine(pf.left, kb.lx, kb.ly);
        n.right = combine(pf.right, kb.rx, kb.ry);
        n.six_axis = pf.six;
        {   // the pad's orientation (the game's pointer scheme reads it): integrated from the gyro, held level by the accelerometer
            const uint64_t t = FrameLimiter::now_ns();
            const double dt = last_orient_ns_ ? static_cast<double>(t - last_orient_ns_) / 1e9 : 0.0;
            last_orient_ns_ = t;
            orient_.step(pf.six.gyro, pf.six.accel, dt);
            std::memcpy(n.six_axis.direction, orient_.direction, sizeof orient_.direction);
            std::memcpy(n.six_axis.angle, orient_.angle, sizeof orient_.angle);
        }
        right_y_.store(n.right.y, std::memory_order_relaxed);
        right_x_f_.store(static_cast<float>(n.right.x) / 32767.f, std::memory_order_relaxed);
        right_y_f_.store(static_cast<float>(n.right.y) / 32767.f, std::memory_order_relaxed);
        gyro_pitch_.store(std::fabs(pf.six.gyro[0]), std::memory_order_relaxed);
        n.six_axis.gyro[0] *= look_scale_.load(std::memory_order_relaxed);      // the game integrates the pitch rate per call: see play_lookrate.h
        out.touch.count = 0;
        if (t.down) {
            out.touch.count = 1;
            hid::TouchFinger& f = out.touch.fingers[0];
            f = hid::TouchFinger{};
            f.id = 0;
            f.x = t.x;
            f.y = t.y;
        }
    }
    void set_pad(const PadFrame& p) {
        std::lock_guard<std::mutex> lk(mu_);
        pad_ = p;
    }
    // Is a pad looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis)? Read by the aim
    // on the main thread; the values are the ones the game gets.
    bool pad_look(bool gyro_enabled) const {
        return std::abs(right_y_.load(std::memory_order_relaxed)) > 650 || (gyro_enabled && gyro_pitch_.load(std::memory_order_relaxed) > 0.02f);
    }
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up: the alternate body's camera orbits with it.
    void right_stick(float* x, float* y) const {
        *x = right_x_f_.load(std::memory_order_relaxed);
        *y = right_y_f_.load(std::memory_order_relaxed);
    }
    void set_look_scale(float k) { look_scale_.store(k, std::memory_order_relaxed); }      // the gyro's pitch rate times k (step * 60), for the game's per-call pitch
    void set_touch(const Touch& t) {
        std::lock_guard<std::mutex> lk(mu_);
        touch_ = t;
    }

private:
    static hid::StickValue combine(hid::StickValue pad, float kx, float ky) {
        double x = pad.x + static_cast<double>(kx) * 32767.0, y = pad.y + static_cast<double>(ky) * 32767.0;
        const double len = std::sqrt(x * x + y * y);
        if (len > 32767.0) { x *= 32767.0 / len; y *= 32767.0 / len; }
        hid::StickValue r;
        r.x = static_cast<int32_t>(std::lround(x));
        r.y = static_cast<int32_t>(std::lround(y));
        return r;
    }
    Engine* engine_;
    PadOrient orient_;
    uint64_t last_orient_ns_ = 0;
    std::atomic<int32_t> right_y_{0};
    std::atomic<float> right_x_f_{0.f}, right_y_f_{0.f};
    std::atomic<float> look_scale_{1.f};
    std::atomic<float> gyro_pitch_{0.f};
    std::mutex mu_;
    PadFrame pad_;
    Touch touch_;
};

}  // namespace detail
}  // namespace nxrt::play
