// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the cross key guide follows the player's pad (pkp_cross_guide.h). Offsets in main of the 2.0.1a update.
//
// The cross key guide's layout holds three sets of panes, one for each place the action's button can have in the cross:
//   set 1  'Null_set_A'  the button at the right   the cross to its left, the text to the right
//   set 2  'Null_set_Y'  the button at the left    the cross to its right, the text to the left
//   set 3  'Null_set_X'  the button at the top     the cross below it, the text to the right
// Each set has its own four key panes '0000_key_1_<a|b|x|y>_000N' (the large one at the set's origin, the others around it at their
// Switch places), a backdrop and a text. The widget (class vtable main+0x3338368) fills the three texts alike, asks the input manager
// for the action's button and stores the set to show at +0xa4 (0 none, 1 for A, 2 for Y, 3 for X: 0x7100e77a08 and 0x7100e78098),
// then starts that set's animation (0x7100f626bc). It does so in every frame its prompt is up.
//
// What this module changes while a pad style shows and the pad's face buttons are not where a Switch pad has them:
//   * the set: by the place of the action's button on the pad (inline hooks on the two stores of +0xa4). Right is set 1, left set 2,
//     top set 3. The game has no set for the bottom, where an Xbox layout has A: that is set 3 drawn upside down (below).
//   * the icons: each key pane of that set shows the button that sits at the pane's place. A pane's sprite is the four corner UVs in
//     its texture record, which nothing rewrites; all four sprites are in one texture.
//   * upside down: set 3's backdrop and its three faint keys are mirrored about the row of its large key. The animation writes every
//     pane's position each frame, so the mirror is made where each of their quads is built (the quad builder's site, pkp_layout.h):
//     the quad's centre moves, in the pane's own units, and the backdrop's quads are turned over as well, the keys' are not. A window
//     pane's nine quads do not follow the pane's world matrix once the update has run (#683's first fix wrote it at the draw: the
//     keys moved, the backdrop stayed under the text).
#include "ui/pkp_cross_guide.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <atomic>
#include <cstring>

#include "nxrt/input_source.h"
#include "prompts.h"

namespace nxrt::games::pkp {

namespace {

// inline: `str w8, [x19, #0xa4]`, the widget (x19) stores the set it shows (w8), in its two fill functions
constexpr uint64_t kSetStores[] = {0xe77a30, 0xe780c0};
constexpr uint64_t kWidgetPart = 0x68;     // widget: its layout part; [part + 8] is the layout's source (layout::kLayoutOf)
// The quad builder called from a window pane's build: the return address, and the SP-relative draw object of the part that is built
// (that build's x20; layout::kDrawPane of it is the part). A window pane holds its nine parts itself.
constexpr uint64_t kWindowQuadReturn = 0x1f8172c;
constexpr size_t kQuadWindowDraw = 0x60, kWindowPaneSize = 0xcb0;

using guest::ok;
using guest::rd;
using layout::kLayFirstNode;
using layout::kLayPaneCount;
using layout::kLaySentinel;
using layout::kPaneLocal;
using layout::kPaneName;
using layout::kPaneNameLen;
using layout::kPaneParent;
using layout::kPaneTexture;
using layout::kTextureUv;

// The Switch face buttons, and the four places of a pad's face buttons (play::PadSrc's numbers).
enum Face : uint8_t { kA, kB, kX, kY, kFaces };
enum Place : uint8_t { kSouth, kEast, kWest, kNorth };
constexpr Place kSwitchPlace[kFaces] = {kEast, kSouth, kNorth, kWest};   // on a Switch pad, and of the key panes in every set
constexpr char kLetter[kFaces] = {'a', 'b', 'x', 'y'};                  // in a key pane's name
constexpr uint64_t kFaceBit[kFaces] = {hid::btn(hid::kBtnA), hid::btn(hid::kBtnB), hid::btn(hid::kBtnX), hid::btn(hid::kBtnY)};

// The widget's sets: the button the game shows each one for, and its root pane.
constexpr uint32_t kSetRight = 1, kSetLeft = 2, kSetTop = 3, kSets = 4;
constexpr Face kSetFace[kSets] = {kA, kA, kY, kX};
constexpr const char* kSetRoot[kSets] = {"", "Null_set_A", "Null_set_Y", "Null_set_X"};
constexpr char kKeyPrefix[] = "0000_key_1_";                        // + letter + "_000N" ("..._trigger_set?" is not a key)
constexpr char kTopBackdrop[] = "Window_0200_base_keyguide_cross_X";   // set 3's backdrop
constexpr size_t kUvBytes = 8 * sizeof(float);
constexpr int kMaxPanes = 256;

// The places of A, B, X and Y, two bits each, with kPlacesKnown.
constexpr uint32_t kPlacesKnown = 1u << 8;
constexpr uint32_t kSwitchPlaces = kPlacesKnown | kSwitchPlace[kA] | kSwitchPlace[kB] << 2 | kSwitchPlace[kX] << 4 | kSwitchPlace[kY] << 6;

struct State {
    uint64_t base = 0;
    // The places on the pad when its four face buttons have the four places, 0 otherwise. The main thread writes it, the game thread
    // reads it.
    std::atomic<uint32_t> places{0};
    // game thread
    bool have_uv = false;                  // something of ours is in the layout
    float uv[kFaces][8] = {};              // each button's sprite, read from the first set before anything was written
    uint64_t dressed[kSets][kFaces] = {};  // the key panes whose UVs were written, by their own letter (remove puts those back)
    // The fill handled last. The widget fills itself every frame while its prompt is up; a fill like the last one only names the set.
    struct {
        uint64_t lay = 0, first = 0;       // the layout and its first pane node
        uint32_t set = 0, places = 0;      // the game's set and the places it was handled for
    } last;
    // Set 3 upside down: the game thread sets these, the quad builds read them.
    std::atomic<uint64_t> flip_root{0};    // 'Null_set_X' while it is drawn upside down, 0 = as the game draws it
    std::atomic<uint64_t> flip_axis{0};    // its large key pane: the mirror's row
    std::atomic<uint64_t> flip_pane[kFaces] = {};   // its backdrop, then its three faint key panes
};
State g;

// The places in force now: the pad's while a pad style shows, else a Switch pad's (also when the pad's face buttons are not four
// buttons at four places).
uint32_t places_now(const State& s) {
    const uint32_t packed = s.places.load(std::memory_order_relaxed);
    if (packed == kSwitchPlaces || !(packed & kPlacesKnown)) return kSwitchPlaces;
    return play::prompt_style_is_pad(play::prompts_effective_style()) ? packed : kSwitchPlaces;
}

// A pane's name, "" when it cannot be read.
void pane_name(uint64_t pane, char out[48]) {
    out[0] = 0;
    if (!ok(pane, kPaneNameLen + 2)) return;
    const uint64_t p = rd<uint64_t>(pane + kPaneName);
    const size_t len = rd<uint16_t>(pane + kPaneNameLen) < 47 ? rd<uint16_t>(pane + kPaneNameLen) : 47;
    if (!p || !ok(p, len)) return;
    std::memcpy(out, reinterpret_cast<const void*>(p), len);
    out[len] = 0;
}

struct SetPanes {
    uint64_t root = 0, key[kFaces] = {}, backdrop = 0;
};

// One set of the layout: its root by name, then the root's children (the list has a parent before its children). False when the
// layout is not the cross key guide's.
bool find_set(uint64_t lay, uint32_t set, SetPanes* out) {
    const int n = rd<uint16_t>(lay + kLayPaneCount);
    uint64_t node = rd<uint64_t>(lay + kLayFirstNode);
    char name[48];
    for (int k = 0; k < n && k < kMaxPanes && node && node != lay + kLaySentinel && ok(node, 24); ++k) {
        const uint64_t pane = rd<uint64_t>(node + 16);
        node = rd<uint64_t>(node + 8);
        pane_name(pane, name);
        if (!out->root) {
            if (std::strcmp(name, kSetRoot[set]) == 0) out->root = pane;
            continue;
        }
        if (!name[0] || rd<uint64_t>(pane + kPaneParent) != out->root) continue;
        const size_t at = sizeof kKeyPrefix - 1;
        if (std::strncmp(name, kKeyPrefix, at) == 0 && name[at + 1] == '_') {
            for (int f = 0; f < kFaces; ++f)
                if (name[at] == kLetter[f]) out->key[f] = pane;
        } else if (std::strncmp(name, kTopBackdrop, sizeof kTopBackdrop - 1) == 0) {
            out->backdrop = pane;
        }
    }
    for (uint64_t key : out->key)
        if (!key) return false;
    return true;
}

// A key pane's eight UV floats, 0 when they cannot be read.
uint64_t uv_of(uint64_t pane) {
    const uint64_t rec = ok(pane + kPaneTexture, 8) ? rd<uint64_t>(pane + kPaneTexture) : 0;
    return rec && ok(rec + kTextureUv, kUvBytes) ? rec + kTextureUv : 0;
}

// Every key pane of the set shows the button that sits at the pane's place. Upside down, the top and the bottom pane swap places.
bool dress(State& s, uint32_t set, const SetPanes& p, const Place place[kFaces], bool upside_down) {
    uint64_t at[kFaces];
    for (int f = 0; f < kFaces; ++f)
        if (!(at[f] = uv_of(p.key[f]))) return false;
    if (!s.have_uv) {
        for (int f = 0; f < kFaces; ++f) std::memcpy(s.uv[f], reinterpret_cast<const void*>(at[f]), kUvBytes);
        s.have_uv = true;
    }
    for (int f = 0; f < kFaces; ++f) {
        Place here = kSwitchPlace[f];
        if (upside_down && here == kNorth) here = kSouth;
        else if (upside_down && here == kSouth) here = kNorth;
        for (int b = 0; b < kFaces; ++b)
            if (place[b] == here) std::memcpy(reinterpret_cast<void*>(at[f]), s.uv[b], kUvBytes);
        s.dressed[set][f] = p.key[f];
    }
    return true;
}

// The widget is about to store the set it shows: the set for the place of the action's button on the pad, dressed for that pad.
void hook_set_store(::Ctx* c, uint64_t, void*) {
    State& s = g;
    const uint32_t set = static_cast<uint32_t>(c->x[8]);
    if (set < kSetRight || set > kSetTop) return;
    const uint32_t packed = places_now(s);
    if (packed == kSwitchPlaces && !s.have_uv) return;     // a Switch pad's places and nothing of ours in the layout
    Place place[kFaces];
    for (int f = 0; f < kFaces; ++f) place[f] = static_cast<Place>((packed >> (2 * f)) & 3);
    const Place at = place[kSetFace[set]];
    const uint32_t show = at == kEast ? kSetRight : at == kWest ? kSetLeft : kSetTop;
    const bool upside_down = at == kSouth;

    const uint64_t widget = c->x[19];
    const uint64_t part = ok(widget + kWidgetPart, 8) ? rd<uint64_t>(widget + kWidgetPart) : 0;
    const uint64_t source = part && ok(part + 8, 8) ? rd<uint64_t>(part + 8) : 0;
    const uint64_t sp = (c->x[31] - 0x100) & ~uint64_t{0xf};
    const uint64_t lay = source ? guest::call(c, s.base + layout::kLayoutOf, source, 0, 0, 0, 0, sp) : 0;
    if (!lay || !ok(lay, kLayPaneCount + 2)) return;
    const uint64_t first = rd<uint64_t>(lay + kLayFirstNode);
    if (lay == s.last.lay && first == s.last.first && set == s.last.set && packed == s.last.places) {
        c->x[8] = show;
        return;
    }
    SetPanes p;
    if (!find_set(lay, show, &p) || (upside_down && !p.backdrop)) return;
    if (!dress(s, show, p, place, upside_down)) return;
    s.last = {lay, first, set, packed};
    c->x[8] = show;
    if (show != kSetTop) return;     // sets 1 and 2 leave set 3 as it is: it may still be fading out
    s.flip_root.store(0, std::memory_order_release);
    if (!upside_down) return;
    s.flip_axis.store(p.key[kX], std::memory_order_relaxed);
    s.flip_pane[0].store(p.backdrop, std::memory_order_relaxed);
    s.flip_pane[1].store(p.key[kA], std::memory_order_relaxed);
    s.flip_pane[2].store(p.key[kB], std::memory_order_relaxed);
    s.flip_pane[3].store(p.key[kY], std::memory_order_relaxed);
    s.flip_root.store(p.root, std::memory_order_release);
}

// A quad is about to be built: a quad of the upside-down set's backdrop or of one of its faint keys is mirrored about the large key's
// row. The row and the pane's own place are local to the set's root; the quad is in the pane's units, so the way up is divided by the
// pane's own height scale. A pane that is turned keeps the game's place.
int hook_quad(::Ctx* c, uint64_t addr, void*) {
    const State& s = g;
    const uint64_t root = s.flip_root.load(std::memory_order_acquire);
    if (!root) return 0;
    const uint64_t base = addr - layout::kQuadGeometry;
    const uint64_t back = rd<uint64_t>(c->x[29] + layout::kQuadReturn);
    uint64_t pane = 0;
    bool backdrop = false;
    if (back == base + layout::kPictureQuadReturn) {
        pane = rd<uint64_t>(rd<uint64_t>(c->x[31] + layout::kQuadPictureDraw) + layout::kDrawPane);
        int i = 1;
        while (i < kFaces && s.flip_pane[i].load(std::memory_order_relaxed) != pane) ++i;
        if (i == kFaces) return 0;
    } else if (back == base + kWindowQuadReturn) {
        const uint64_t part = rd<uint64_t>(rd<uint64_t>(c->x[31] + kQuadWindowDraw) + layout::kDrawPane);
        pane = s.flip_pane[0].load(std::memory_order_relaxed);
        if (part < pane || part >= pane + kWindowPaneSize) return 0;
        backdrop = true;
    } else {
        return 0;
    }
    if (rd<uint64_t>(pane + kPaneParent) != root) return 0;     // the layout is another one's now
    const uint64_t lp = rd<uint64_t>(pane + kPaneLocal);
    const uint64_t ap = rd<uint64_t>(s.flip_axis.load(std::memory_order_relaxed) + kPaneLocal);
    if (!lp || !ap) return 0;
    float l[16];
    std::memcpy(l, reinterpret_cast<const void*>(lp), sizeof l);
    if (l[1] != 0.f || l[4] != 0.f || !(l[5] > 0.f)) return 0;
    const float up = 2.f * (rd<float>(ap + 13 * sizeof(float)) - l[13]) / l[5];
    float& half_height = c->v[1].f32[0];
    float& y = c->v[3].f32[0];
    if (backdrop) {
        y = up - y;
        half_height = -half_height;
    } else {
        y += up;
    }
    return 0;
}

// main thread: where the pad's face buttons sit, from the pad remap and the face-button layout
void settings_changed(const play::Settings& set) {
    uint32_t packed = kPlacesKnown, seen = 0;
    for (int f = 0; f < kFaces; ++f) {
        play::PadSrc src = play::PadSrc::South;
        if (!play::pad_face_position(set.pad, set.pad.label_abxy, kFaceBit[f], &src)) break;
        packed |= static_cast<uint32_t>(src) << (2 * f);
        seen |= 1u << static_cast<int>(src);
    }
    g.places.store(seen == 0xf ? packed : 0, std::memory_order_relaxed);
}

void install(const FeatureInstall& in) {
    State& s = g;
    s.base = in.base;
    settings_changed(in.settings);
    for (uint64_t site : kSetStores) in.hooks.add_inline(in.base + site, &hook_set_store);
    in.hooks.add_mid(in.base + layout::kQuadGeometry, &hook_quad);
}

// The hooks are gone: the key panes show their own buttons again, and nothing is mirrored.
void remove() {
    State& s = g;
    s.flip_root.store(0, std::memory_order_release);
    for (uint32_t set = kSetRight; s.have_uv && set < kSets; ++set)
        for (int f = 0; f < kFaces; ++f) {
            const uint64_t pane = s.dressed[set][f];
            char name[48];
            pane_name(pane, name);
            const uint64_t at = std::strncmp(name, kKeyPrefix, sizeof kKeyPrefix - 1) == 0 ? uv_of(pane) : 0;
            if (at) std::memcpy(reinterpret_cast<void*>(at), s.uv[f], kUvBytes);
        }
    s.have_uv = false;
    s.last = {};
    std::memset(s.dressed, 0, sizeof s.dressed);
}

}  // namespace

const Feature kCrossGuideFeature = {
    .name = "cross_guide",
    .install = &install,
    .remove = &remove,
    .settings_changed = &settings_changed,
};

}  // namespace nxrt::games::pkp
