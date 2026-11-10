// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay: the on-screen keyboard for the game's software keyboard applet (services_api.h swkbdui), and the pad routing for it and the quit prompt.
#include "overlay_impl.h"

namespace nxrt::gfx {
using namespace overlay_detail;

namespace {
// ---- on-screen keyboard: text helpers and the key grid ----
// UTF-8 to code points (a bad byte becomes U+FFFD) and back. The applet's max_len counts characters, so the text is kept as code points.
std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        char32_t v = n == 1 ? c : (c & (0x7Fu >> n));
        bool ok = true;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            v = (v << 6) | (cc & 0x3Fu);
        }
        out.push_back(ok ? v : 0xFFFD);
        i += ok ? n : 1;
    }
    return out;
}

std::string u32_to_utf8(const std::u32string& s, size_t from = 0, size_t to = std::u32string::npos) {
    std::string out;
    to = std::min(to, s.size());
    auto put = [&](uint32_t b) { out.push_back(static_cast<char>(b)); };
    for (size_t i = from; i < to; ++i) {
        const uint32_t c = s[i];
        if (c < 0x80) put(c);
        else if (c < 0x800) { put(0xC0 | (c >> 6)); put(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { put(0xE0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
        else { put(0xF0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3F)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
    }
    return out;
}

// Typographic punctuation the overlay's font has no glyph for, as plain ASCII, for the game's header and guide text (Pokopia's name question
// is "What\u2019s your name?", drawn as "What?s" otherwise).
std::string fold_punct(const std::string& s) {
    std::u32string in = utf8_to_u32(s), out;
    for (char32_t c : in) {
        switch (c) {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += U'\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += U'"'; break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: out += U'-'; break;
            case 0x2026: out += U"..."; break;
            case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x3000: out += U' '; break;
            default: out += c;
        }
    }
    return u32_to_utf8(out);
}


const std::vector<std::vector<KbKey>>& kb_rows() {
    static const std::vector<std::vector<KbKey>> rows = [] {
        std::vector<std::vector<KbKey>> r;
        auto row = [&](const char* chars, const char* alts) {
            std::vector<KbKey> v;
            for (int i = 0; chars[i]; ++i) v.push_back({static_cast<char32_t>(chars[i]), static_cast<char32_t>(alts[i]), 1, KbOp::Char});
            r.push_back(v);
        };
        row("1234567890", "1234567890");
        row("qwertyuiop", "QWERTYUIOP");
        row("asdfghjkl-", "ASDFGHJKL_");
        row("zxcvbnm,.?", "ZXCVBNM;:/");
        r.push_back({{0, 0, 2, KbOp::Shift}, {U' ', U' ', 4, KbOp::Space}, {U'!', U'@', 1, KbOp::Char}, {U'\'', U'"', 1, KbOp::Char}, {0, 0, 2, KbOp::Back}});
        r.push_back({{0, 0, 5, KbOp::Cancel}, {0, 0, 5, KbOp::Ok}});
        return r;
    }();
    return rows;
}
// The key of a row that covers column unit u (past the end: the last key); *start = its first unit.
int kb_key_at(const std::vector<KbKey>& row, int u, int* start) {
    int x = 0;
    for (int i = 0; i < static_cast<int>(row.size()); ++i) {
        if (u < x + row[i].w || i + 1 == static_cast<int>(row.size())) {
            if (start) *start = x;
            return i;
        }
        x += row[i].w;
    }
    return 0;
}

// Pad directions the keyboard moves on: the d-pad, or the left stick past half way (set_keyboard_pad adds the stick bits).
constexpr uint64_t kKbDirBits[4] = {
    hid::btn(hid::kBtnUp) | hid::btn(hid::kBtnStickLUp), hid::btn(hid::kBtnDown) | hid::btn(hid::kBtnStickLDown),
    hid::btn(hid::kBtnLeft) | hid::btn(hid::kBtnStickLLeft), hid::btn(hid::kBtnRight) | hid::btn(hid::kBtnStickLRight)};
constexpr double kKbRepeatDelay = 0.40, kKbRepeatEvery = 0.07;   // seconds: a held direction or B repeats

}  // namespace

bool Overlay::Impl::kb_poll() {
    namespace sk = services::swkbdui;
    const uint64_t id = sk::pending(nullptr);
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (id == kb_id || (id && id <= kb_answered)) return kb_id != 0;
    }
    sk::Request r;
    uint64_t now_id = sk::pending(&r);
    std::lock_guard<std::mutex> lk(kb_mu);
    if (now_id && now_id <= kb_answered) now_id = 0;    // answered here meanwhile
    if (now_id == kb_id) return kb_id != 0;
    if (kb_id && !now_id) NXLOG_I(kTag, "on-screen keyboard: question %llu closed elsewhere", static_cast<unsigned long long>(kb_id));
    kb_steam_close();
    kb_id = now_id;
    kb_steam_want = now_id != 0 && host_steam_keyboard_wanted();
    kb_frames = 0;
    kb_keys = !kb_steam_want;
    if (now_id) {
        kb_req = std::move(r);
        for (std::string* t : {&kb_req.title, &kb_req.header, &kb_req.sub, &kb_req.guide, &kb_req.ok_text}) *t = fold_punct(*t);
        kb_text = utf8_to_u32(kb_req.initial);
        if (kb_req.max_len && kb_text.size() > kb_req.max_len) kb_text.resize(kb_req.max_len);
        kb_caret = kb_text.size();
        kb_shift = 0;
        kb_row = 1;
        kb_unit = 0;
        for (double& t : kb_rep) t = DBL_MAX;
        pad_new.store(0);
        NXLOG_I(kTag, "on-screen keyboard: question %llu (max %u characters%s)", static_cast<unsigned long long>(now_id), kb_req.max_len,
                kb_req.password ? ", password" : "");
    }
    kb_open = now_id != 0;
    return kb_open;
}

bool Overlay::Impl::kb_finish(bool ok) {
    if (!kb_id || (ok && kb_text.size() < kb_req.min_len)) return false;
    const uint64_t id = kb_id;
    kb_answered = id;
    kb_id = 0;
    kb_open = false;
    NXLOG_I(kTag, "on-screen keyboard: question %llu %s", static_cast<unsigned long long>(id), ok ? "answered" : "cancelled");
    services::swkbdui::answer(id, ok, ok ? u32_to_utf8(kb_text) : std::string());
    kb_steam_close();
    kb_steam_want = false;
    return true;
}

void Overlay::Impl::kb_steam_close() {
    if (!kb_steam_open) return;
    kb_steam_open = false;
    host_steam_keyboard_close();
}

void Overlay::Impl::kb_insert(const std::u32string& t) {
    for (const char32_t c : t) {
        if (c < 0x20 || c == 0x7F) continue;
        if (kb_req.max_len && kb_text.size() >= kb_req.max_len) break;
        kb_text.insert(kb_caret++, 1, c);
    }
}

void Overlay::Impl::kb_backspace() {
    if (kb_caret == 0) return;
    kb_text.erase(--kb_caret, 1);
}

bool Overlay::Impl::kb_press(const KbKey& k) {
    switch (k.op) {
        case KbOp::Char:
            kb_insert(std::u32string(1, kb_shift ? k.alt : k.ch));
            if (kb_shift == 1) kb_shift = 0;
            return false;
        case KbOp::Shift: kb_shift = (kb_shift + 1) % 3; return false;
        case KbOp::Space: kb_insert(U" "); return false;
        case KbOp::Back: kb_backspace(); return false;
        case KbOp::Ok: return kb_finish(true);
        case KbOp::Cancel: return kb_finish(false);
    }
    return false;
}

// The physical keyboard while the on-screen keyboard is up: typing goes straight into its text (SDL text events), Enter answers, Escape
// cancels. Every key is consumed, the game sees none.
bool Overlay::Impl::kb_key_event(const input::Event& e) {
    bool closed = false;
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (e.type == input::EventType::Text) {
            kb_insert(utf8_to_u32(e.u.text.utf8));
        } else if (e.type == input::EventType::KeyDown && kb_id) {
            const bool rep = (e.flags & input::kFlagRepeat) != 0;
            switch (e.u.key.scancode) {
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER: if (!rep) closed = kb_finish(true); break;
                case SDL_SCANCODE_ESCAPE: if (!rep) closed = kb_finish(false); break;
                case SDL_SCANCODE_BACKSPACE: kb_backspace(); break;
                case SDL_SCANCODE_DELETE: if (kb_caret < kb_text.size()) kb_text.erase(kb_caret, 1); break;
                case SDL_SCANCODE_LEFT: if (kb_caret > 0) --kb_caret; break;
                case SDL_SCANCODE_RIGHT: if (kb_caret < kb_text.size()) ++kb_caret; break;
                case SDL_SCANCODE_HOME: kb_caret = 0; break;
                case SDL_SCANCODE_END: kb_caret = kb_text.size(); break;
                case SDL_SCANCODE_V:
                    if (e.mods & SDL_KMOD_CTRL) {
                        if (char* clip = SDL_GetClipboardText()) {
                            kb_insert(utf8_to_u32(clip));
                            SDL_free(clip);
                        }
                    }
                    break;
                default: break;
            }
        }
    }
    if (closed) {
        std::lock_guard<std::mutex> lk(mu);
        clear_imgui_input(imgui);
    }
    return true;
}

void Overlay::Impl::draw_keyboard(double now_s) {
    std::lock_guard<std::mutex> lk(kb_mu);
    if (!kb_id) return;
    const auto& rows = kb_rows();
    const int nrows = static_cast<int>(rows.size());
    bool closed = false;

    // The pad: presses since the last frame; a direction or B held past the delay repeats.
    const uint64_t held = pad_held.load();
    uint64_t fresh = pad_new.exchange(0);
    // With Steam's keyboard the panel is only the question and the text field. Steam takes the pad while its keyboard is up, so a press
    // that reaches the game means it is closed: the on-screen keys come up, and that press only shows them (+ and - keep their meaning).
    const uint64_t plus_minus = hid::btn(hid::kBtnPlus) | hid::btn(hid::kBtnMinus);
    if (!kb_keys && (fresh & ~plus_minus)) {
        kb_keys = true;
        fresh &= plus_minus;
    }
    auto fired = [&](int slot, uint64_t bits) {
        bool f = false;
        if (fresh & bits) { f = true; kb_rep[slot] = now_s + kKbRepeatDelay; }
        else if ((held & bits) && now_s >= kb_rep[slot]) { f = true; kb_rep[slot] = now_s + kKbRepeatEvery; }
        if (!(held & bits)) kb_rep[slot] = DBL_MAX;
        return f;
    };
    auto move_h = [&](int d) {
        const auto& row = rows[kb_row];
        const int n = static_cast<int>(row.size());
        const int i = (kb_key_at(row, kb_unit, nullptr) + d + n) % n;
        int x = 0;
        for (int j = 0; j < i; ++j) x += row[j].w;
        kb_unit = x;
    };
    if (fired(0, kKbDirBits[0])) kb_row = (kb_row + nrows - 1) % nrows;
    if (fired(1, kKbDirBits[1])) kb_row = (kb_row + 1) % nrows;
    if (fired(2, kKbDirBits[2])) move_h(-1);
    if (fired(3, kKbDirBits[3])) move_h(1);
    const int sel = kb_key_at(rows[kb_row], kb_unit, nullptr);
    if (fresh & hid::btn(hid::kBtnA)) closed = kb_press(rows[kb_row][sel]);
    if (!closed && fired(4, hid::btn(hid::kBtnB))) kb_backspace();
    if (!closed && (fresh & hid::btn(hid::kBtnY))) kb_insert(U" ");
    if (!closed && (fresh & hid::btn(hid::kBtnX))) kb_shift = (kb_shift + 1) % 3;
    if (!closed && (fresh & hid::btn(hid::kBtnL)) && kb_caret > 0) --kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnR)) && kb_caret < kb_text.size()) ++kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnPlus))) closed = kb_finish(true);
    if (!closed && (fresh & hid::btn(hid::kBtnMinus))) closed = kb_finish(false);
    if (closed) {
        clear_imgui_input(imgui);
        return;
    }

    // A centred panel over the dimmed game, sized in units of the UI scale like the menu.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = std::max(320.f, std::min(760.f * k, disp.x - 32.f * k));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 110));
    // centred; at the top when Steam's keyboard comes up from the bottom of the screen
    if (kb_steam_want) ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, std::round(disp.y * 0.04f)), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    else ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    const std::string title = (kb_req.title.empty() ? std::string("Keyboard") : kb_req.title) + "###nxrt_swkbd";
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | (kb_keys ? 0 : ImGuiWindowFlags_NoTitleBar);
    if (ImGui::Begin(title.c_str(), nullptr, wf)) {
        ImGui::PushTextWrapPos(0.f);
        if (!kb_req.header.empty()) ImGui::TextUnformatted(kb_req.header.c_str());
        if (!kb_req.sub.empty()) ImGui::TextDisabled("%s", kb_req.sub.c_str());
        ImGui::PopTextWrapPos();

        // the text field: drawn here, not an ImGui InputText, so the keyboard, the pad and the mouse all edit the same text and caret
        const ImGuiStyle& style = ImGui::GetStyle();
        ImFont* font = ImGui::GetFont();
        const float big = ImGui::GetFontSize() * 1.25f;
        const float fh = std::round(big * 1.7f), pad = style.FramePadding.x * 1.5f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float fw = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(fw, fh));
        const ImVec2 p1(p0.x + fw, p0.y + fh);
        // Steam's keyboard, asked for once the field has its place (an auto-sized window takes its size in the frames after the first):
        // Steam keeps the field uncovered and types into the window
        if (kb_steam_want && !kb_steam_open && ++kb_frames >= 3) {
            kb_steam_open = true;
            host_steam_keyboard_open(static_cast<int>(p0.x), static_cast<int>(p0.y), static_cast<int>(fw), static_cast<int>(fh));
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
        std::string shown, before;
        if (kb_req.password) {
            shown.assign(kb_text.size(), '*');
            before.assign(kb_caret, '*');
        } else {
            shown = u32_to_utf8(kb_text);
            before = u32_to_utf8(kb_text, 0, kb_caret);
        }
        const float cx = font->CalcTextSizeA(big, FLT_MAX, 0.0f, before.c_str()).x;
        const float scroll = std::max(0.f, cx - (fw - 2.f * pad) + 2.f);   // keep the caret in view
        const float ty = p0.y + (fh - big) * 0.5f;
        dl->PushClipRect(ImVec2(p0.x + pad * 0.5f, p0.y), ImVec2(p1.x - pad * 0.5f, p1.y), true);
        if (shown.empty() && !kb_req.guide.empty()) dl->AddText(font, big, ImVec2(p0.x + pad, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), kb_req.guide.c_str());
        else dl->AddText(font, big, ImVec2(p0.x + pad - scroll, ty), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (std::fmod(now_s, 1.0) < 0.6) {
            const float x = p0.x + pad - scroll + cx;
            dl->AddLine(ImVec2(x, ty), ImVec2(x, ty + big), ImGui::GetColorU32(ImGuiCol_Text), std::max(1.f, 1.5f * k));
        }
        dl->PopClipRect();
        if (kb_req.max_len) {
            char count[32];
            std::snprintf(count, sizeof count, "%zu / %u", kb_text.size(), kb_req.max_len);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fw - ImGui::CalcTextSize(count).x);
            ImGui::TextDisabled("%s", count);
        }
        ImGui::Spacing();

        // the keys; the pad's key has a bright frame, the mouse clicks any of them
        const float gap = std::round(style.ItemSpacing.x * 0.75f);
        const float unit = (ImGui::GetContentRegionAvail().x - gap * (kKbUnits - 1)) / kKbUnits;
        const float kh = std::round(ImGui::GetFontSize() * 2.3f);
        const bool ok_allowed = kb_text.size() >= kb_req.min_len;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int r = 0; r < nrows && !closed && kb_keys; ++r) {
            int x = 0;
            for (int i = 0; i < static_cast<int>(rows[r].size()) && !closed; ++i) {
                const KbKey& key = rows[r][i];
                if (i) ImGui::SameLine();
                std::string label;
                switch (key.op) {
                    case KbOp::Char: label = u32_to_utf8(std::u32string(1, kb_shift ? key.alt : key.ch)); break;
                    case KbOp::Shift: label = kb_shift == 2 ? "CAPS" : kb_shift ? "SHIFT" : "Shift"; break;
                    case KbOp::Space: label = "Space"; break;
                    case KbOp::Back: label = "Delete"; break;
                    case KbOp::Ok: label = kb_req.ok_text.empty() ? "OK" : kb_req.ok_text; break;
                    case KbOp::Cancel: label = "Cancel"; break;
                }
                ImGui::PushID(r * 32 + i);
                const bool is_sel = r == kb_row && i == sel;
                const bool lit = key.op == KbOp::Shift && kb_shift;
                if (lit) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                ImGui::BeginDisabled(key.op == KbOp::Ok && !ok_allowed);
                if (ImGui::Button(label.c_str(), ImVec2(unit * key.w + gap * (key.w - 1), kh))) {
                    kb_row = r;
                    kb_unit = x;
                    closed = kb_press(key);
                }
                ImGui::EndDisabled();
                if (lit) ImGui::PopStyleColor();
                if (is_sel)
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 200, 40, 255), style.FrameRounding,
                                                        0, std::max(2.f, 2.5f * k));
                ImGui::PopID();
                x += key.w;
            }
        }
        ImGui::PopStyleVar();
        if (!closed) {
            ImGui::Spacing();
            // the face buttons are Switch buttons after the player's mapping: by printed label they are named so, by position they sit
            // where a Switch pad has them
            const bool lab = kb_by_label.load(std::memory_order_relaxed);
            ImGui::PushTextWrapPos(0.f);
            if (!kb_keys) {
                ImGui::TextDisabled("Type on Steam's keyboard, Enter to confirm. Steam + X brings it back; any button shows the on-screen keys.");
            } else {
                ImGui::TextDisabled("Pad: %s type, %s delete, %s space, %s shift, L/R move, + OK, - cancel", lab ? "A" : "A (right)",
                                    lab ? "B" : "B (bottom)", lab ? "Y" : "Y (left)", lab ? "X" : "X (top)");
                ImGui::TextDisabled("Keyboard: type, Enter OK, Esc cancel, Ctrl+V paste");
                if (kb_steam_want) ImGui::TextDisabled("Steam keyboard: Steam + X");
            }
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
    if (closed) clear_imgui_input(imgui);
}

bool Overlay::keyboard_open() const { return impl_->kb_poll(); }
bool Overlay::blocks_game() const { return impl_->visible || impl_->quit_open || impl_->kb_poll(); }
void Overlay::set_keyboard_abxy(bool by_label) { impl_->kb_by_label.store(by_label, std::memory_order_relaxed); }

uint64_t Overlay::set_keyboard_pad(uint64_t buttons, int lx, int ly) {
    Impl& s = *impl_;
    constexpr int kHalf = 16384;     // half of the hid stick range
    uint64_t b = buttons;
    if (lx <= -kHalf) b |= hid::btn(hid::kBtnStickLLeft);
    if (lx >= kHalf) b |= hid::btn(hid::kBtnStickLRight);
    if (ly >= kHalf) b |= hid::btn(hid::kBtnStickLUp);       // hid y: up = +
    if (ly <= -kHalf) b |= hid::btn(hid::kBtnStickLDown);
    const bool quit = s.quit_open;
    const bool page = !quit && s.visible && !s.kb_poll();   // the F1 page: the pad moves its focus, and stays away from the game
    const bool open = quit || page || s.kb_poll();
    bool close_page = false;
    if (page) {
        // Switch A / B are after the player's mapping, as in the quit prompt. + closes the page.
        struct Map { uint64_t bits; ImGuiKey key; };
        const Map keys[] = {{kKbDirBits[0], ImGuiKey_GamepadDpadUp}, {kKbDirBits[1], ImGuiKey_GamepadDpadDown}, {kKbDirBits[2], ImGuiKey_GamepadDpadLeft},
                            {kKbDirBits[3], ImGuiKey_GamepadDpadRight}, {hid::btn(hid::kBtnA), ImGuiKey_GamepadFaceDown},
                            {hid::btn(hid::kBtnB), ImGuiKey_GamepadFaceRight}};
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();
        for (const Map& m : keys)
            if (((b ^ s.pad_prev) & m.bits) != 0) io.AddKeyEvent(m.key, (b & m.bits) != 0);
        ImGui::SetCurrentContext(prev);
        close_page = (b & ~s.pad_prev & hid::btn(hid::kBtnPlus)) != 0;
    }
    if (quit) {                      // the prompt takes the pad before the keyboard does: fresh presses of A and B answer it
        const uint64_t fresh = b & ~s.pad_prev;
        if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnA))) s.quit_answer(true);
        else if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnB))) s.quit_answer(false);
        else if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnX))) s.quit_to_settings();
        s.pad_mask = b;
    } else if (page) {
        s.pad_mask = b;
    } else if (open) {
        s.pad_new.fetch_or(b & ~s.pad_prev);
        s.pad_mask = b;
    } else {
        s.pad_mask &= b;             // a button leaves the mask once released
    }
    s.pad_held.store(b);
    s.pad_prev = b;
    if (close_page) set_visible(false);   // after the lock is released: set_visible takes it too
    return open ? ~0ull : s.pad_mask;
}


}  // namespace nxrt::gfx
