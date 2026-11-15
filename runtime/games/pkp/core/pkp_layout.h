// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia (main 2.0.1a): game globals, struct offsets and functions that more than one hook file uses. Main-image values are
// offsets from main's base; struct values are offsets into the object named in the group. A value one file alone uses stays in that file.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::games::pkp::layout {

// ---- globals in main (qword pointers) ----
constexpr uint64_t kPlayerManager = 0x3561928;   // the player manager (26344 bytes, made in 0x71000ae380)
constexpr uint64_t kManagerPlayer = 0x30;        // player manager: the local player's character (+0x38 the second one)
constexpr uint64_t kSaveRoot = 0x3561930;        // the save data (the save/options block)
constexpr uint64_t kGameState = 0x3561710;       // the game state (stage, session)
constexpr uint64_t kStateStage = 0x18;           // game state: u8, the stage the player is in
constexpr uint64_t kStateSession = 0x90;         // game state: u8 Link Play session bits (kSession*); 0 offline
constexpr uint8_t kSessionOnline = 0x01, kSessionHost = 0x02, kSessionVisitor = 0x04;
constexpr uint64_t kInputMgr = 0x3561718;        // the input manager
constexpr uint64_t kTables = 0x35618f0;          // the data-table manager (the sprite sheet lookups read it too)
constexpr uint64_t kUiMgr = 0x35aebf8;           // the UI manager (UI resource manager: holds the loaded sprite sheets and the layout pools)
constexpr uint64_t kLanguage = 0x36ca680;        // -> u32 language 0..8
constexpr uint64_t kWorld = 0x34efc98;           // the world ([GOT 0x3483b10] points here); +0x6000030 = chunk map u16[64*64], +0x6000018 = packed cells

// ---- KTGL layouts and their panes ----
// A layout's panes are a list: u16 count, the first node (node +8 the next one, +16 its pane), and the node that ends it (layout + 64).
constexpr size_t kLayPaneCount = 146, kLayFirstNode = 32, kLaySentinel = 64;
constexpr size_t kPaneName = 176, kPaneParent = 208, kPaneWorld = 224, kPaneNameLen = 272;
// kPaneWorld and kPaneLocal point to the pane's two 4x4 float matrices (X axis at [0], Y axis at [4], position at [12]); the update
// before the draw builds them from the animation every frame, world = local x the parent's world.
constexpr size_t kPaneLocal = 232;
// A picture pane's texture record: the four corner UVs of its sprite (8 floats), not animated.
constexpr size_t kPaneTexture = 0x1b0, kTextureUv = 8;
// mid: the draw of a picture pane and of a window pane, at its flag store (pane in x0), before it reads the pane's world matrix. Two of
// the menu pointer's sites (pkp_menu_internal.h kDrawSites).
constexpr uint64_t kPictureDraw = 0x1f76168, kWindowDraw = 0x1f7fe48;

// ---- the data-table manager: table i at +0x10 + its offset (pkp_tables.h looks a row up) ----
// u32: how many tables it holds (8-byte slots from +0x10; a table at off is there while this is above off / 8, pkp_tables.h).
constexpr uint64_t kTableCount = 0x2b70;
constexpr uint64_t kItemTable = 0x638;
constexpr uint64_t kItemNameId = 0x22;           // item record: u16 name id (1..kNameIdMax, for kNameOf)
constexpr uint16_t kNameIdMax = 3000;

// ---- the input manager ----
// Its vector at +0xcd8 (end at +0xce0) maps an action to its button (u32 per action, low byte: 0..3 the d-pad, 4 Y, 5 A, 6 X, 7 B, 8 (-),
// 9 (+), 0xa L, 0xb R, 0xc ZL, 0xd ZR, 0xe / 0xf the stick clicks; 0x7101126e18 reads it for the key guide icons).
constexpr uint64_t kActionButtons = 0xcd8;

// ---- character (a player's) ----
constexpr uint64_t kPosition = 0x20c0;           // world position (vec4; 3 floats, Y up; vtable slot 2 0x71002e40d8 returns it)
constexpr uint64_t kHeading = 0x2404;            // heading (radians, atan2(x, z)), a copy of the movement controller's (vtable slot 3 0x71002e4478)
// The local player's class (vtable main+0x322fda8) overrides both getters (0x71002e4084, 0x71002e4420): while its mode word at +0x5a34 is 0
// and the byte at +0x5a39 is 0 (vtable slots 504/512), the position and heading come from the save data instead (+0x7419f0, +0x741a10).
constexpr uint64_t kPlayerMode = 0x5a34, kPlayerModeByte = 0x5a39;
constexpr uint64_t kMouseModeOn = 0xaa81;        // the game's Joy-Con 2 mouse mode is on (u8)
constexpr uint64_t kMouseHitValid = 0xaa82;      // the pointer's hit is valid (u8)
constexpr uint64_t kMouseSwingPos = 0xaa50, kMouseSwingValid = 0xaa84;   // the swing tools' position under the pointer (vec4), its flag
constexpr uint64_t kAction = 0x18b8;             // the action component (pointer)
// A player's form is a character number. The form asked for is read and written through the virtuals 0x71006671b0 / 0x71006671bc. A
// change is asked for through the request block; its writer 0x71002f14e0 stores the form, the motion played for the change and the go flag.
constexpr uint64_t kFormNow = 0x5a44;            // u16: the form the player is in
constexpr uint64_t kFormWanted = 0x5a46;         // u16: the form asked for
constexpr uint16_t kFormHuman = 1, kFormMax = 1000;   // the writer takes forms 1..1000
constexpr uint64_t kFormRequest = 0x3778;        // the form request block
constexpr uint64_t kRequestForm = 0x10, kRequestMotion = 0x12, kRequestGo = 0x14;   // request block: u16 form, u16 motion, u8 go
constexpr uint64_t kGroundState = 0x2ac0;        // a player's ground state block (0x71004050a4 and 0x710040a12c store its flag word at +0xbe2)

// ---- the actor pool: the Pokemon in the world ----
// [main + kActorPool] is a holder whose first pointer is the pool; its actors are kPoolSize pointers from +kPoolSlots (0: a free slot).
// An actor is a character (kPosition, the virtuals below) and carries what it is: the id of its species name.
constexpr uint64_t kActorPool = 0x3483bd0, kPoolSlots = 0x50;
constexpr uint32_t kPoolSize = 64;
constexpr uint64_t kActorSpecies = 0x39f0;       // u16: its id in the species name table (for kSpeciesName); the resident row - 4 where seen
constexpr uint64_t kCharacterNumber = 0x1d0;     // character vtable: () -> its character number (a player's: the form it is in)
constexpr uint64_t kSpeciesName = 0xa34cc8;      // (species id 1..kSpeciesMax) -> UTF-8 name in the game's language
constexpr uint16_t kSpeciesMax = 1023;

// ---- motion packs and motion sets ----
// The motion packs are [main + kPackOwner] + kMotionPacks: a 6-byte record per pack id (1..1023) with its requests (u16 at +8) and
// loads (u16 at +10), and from +kPackOfCharacter a u16 per character number, the pack that holds its motions (0: none).
constexpr uint64_t kPackOwner = 0x3561738, kMotionPacks = 0x8c830, kPackOfCharacter = 0x4e08;
// The motion sets are the list at [main + kMotionSets]: entry pointers at [+kSetsEntries], its slots (u16) and how many are in use (u16).
// An entry is the set's pointer, then the character number it is for (s32, negative: a free slot). A set counts its users.
constexpr uint64_t kMotionSets = 0x35619e8, kSetsEntries = 0x18, kSetsSlots = 0x20, kSetsUsed = 0x22;
constexpr uint64_t kSetEntryNumber = 0x8;
constexpr uint64_t kSetUsers = 0x1074, kSetLoaded = 0x1076;   // motion set: u16 users (the first binds the motions), u8 its motions are loaded
constexpr uint64_t kSetMotions = 0xc8, kSetMotionCount = 0xd0;   // motion set: its motions (pointers at [+0xc8]), how many (u32)
constexpr uint64_t kMotionData = 0x8;            // motion: its data (pointer; the first byte is the motion's kind)

// ---- action component ([character + kAction]) ----
// u16: the action running now. StartAction stores the started id at 0x710027e5d8 (its only non-zero writer); every end path writes 0.
constexpr uint64_t kActionId = 0x984;

// ---- save data ----
constexpr uint64_t kSaveHeading = 0x741a10;      // the local player's heading while kPlayerMode and kPlayerModeByte are 0
constexpr uint64_t kSavePlayTime = 0x4a5418;     // double: the save's play time in seconds (0x71011358e0 reads it for "PlayHour")
// The items (pkp_items.h has a slot's fields and how they were measured).
constexpr uint64_t kSaveCommon = 0x13d4920;      // the common block
constexpr uint64_t kBagOffline = 0x830;          // common block: the bag's slots while offline (m_useItemInventoryOffline)
constexpr uint32_t kBagSlots = 120;

// ---- items in main ----
constexpr uint64_t kItemSlotVtable = 0x322c1d8;  // the vtable every item slot holds
constexpr uint64_t kItemTempFlag = 0x36bbfd8;    // u8 per player: the bag / container screen works on a temp copy (an item is held)

// ---- movement component ----
constexpr uint64_t kController = 0xbc0;          // the movement controller (embedded)
constexpr uint64_t kControllerRise = 0xc;        // controller: the speed it rises with (float, negative: sinks)
constexpr uint64_t kControllerY = 0x24;          // controller: its position's height (the position is the vec at +0x20)
constexpr uint64_t kMoveMotion = 0x228;          // the motion the component plays (pointer)
constexpr uint64_t kMoveMotions = 0x260;         // the motion set of its character (pointer)
// The components by actor index: the pointers of [main + kMovements] from +kMovementSlots (index 0: the local player's).
constexpr uint64_t kMovements = 0x356a778, kMovementSlots = 0x58;

// ---- the save manager (pkp_story_guard.cpp, ui/pkp_save_now.cpp) ----
constexpr uint64_t kSaveManager = 0x35619d0;      // main: -> the save manager
constexpr uint64_t kSaveRequest = 0x241324;       // (manager, request, mode, flag) -> 1 when queued
constexpr uint64_t kSaveDone = 0x2412b0;          // (manager) -> 1 when the save manager is idle (its last request finished)
constexpr uint64_t kSaveRootHome = 0xa4;          // save root: u8, the request's second byte
constexpr uint64_t kStateAutosave = 0x3bb5e0;     // game state: u32, 1..4 while the autosave works
constexpr uint64_t kStateEvent = 0xd0, kEventBusy = 0x6ea;   // game state: the event bus, its busy byte

// ---- the main loop ----
// mid: the frame step (0xbfc404) where its frame multiplier is joined (s0 the multiplier, s8 the frame time): the game thread, once a
// frame. The feature registry's game-frame tick (pkp_game.cpp) and the frame-rate hook (pkp_frame_rate.cpp) sit here, in that order.
constexpr uint64_t kFrameStep = 0xbfc4fc;

// ---- game functions ----
constexpr uint64_t kOnline = 0x376c4;            // () -> bit 0: Link Play session
constexpr uint64_t kMaxStack = 0x2a7f0c;         // () -> the max stack (u16), one value for every item
constexpr uint64_t kNameOf = 0xa34e1c;           // (name id, form) -> UTF-8 name in the game's language
constexpr uint64_t kLayoutOf = 0x9132fc;         // (source) -> layout
constexpr uint64_t kChildOf = 0xa6f92c;          // (children (screen + 0x48), element id, u16* class) -> child widget
constexpr uint64_t kTextString = 0xf35f20;       // (widget, text slot, text id, const char*): a text id with a string argument
constexpr uint32_t kStringText = 0xa8;           // text id "<string;0(lstr1)>"
// The key guide widget's functions, offsets and tables are in pkp_key_guide.cpp, its one user.

}  // namespace nxrt::games::pkp::layout
