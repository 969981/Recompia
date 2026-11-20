// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer (main 2.0.1a): game globals, struct offsets and functions that more than one hook file uses. Main-image values are
// offsets from main's base; struct values are offsets into the object named in the group. A value one file alone uses stays in that file.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::games::pkp::layout {

// ---- globals in main (qword pointers) ----
constexpr uint64_t kPlayerManager = 0x3561928;   // the player manager (26344 bytes, made in 0x71000ae380)
constexpr uint64_t kManagerPlayer = 0x30;        // player manager: the local player's character (+0x38 the second one)
constexpr uint64_t kSaveRoot = 0x3561930;        // the save data (the save/options block)
constexpr uint64_t kSaveStory = 0x4a26a0;         // save root: m_story, the story quest history (the story quest history)
constexpr uint8_t kQuestDone = 4;                // m_story: a quest's state byte once it is completed
constexpr uint64_t kGameState = 0x3561710;       // the game state (stage, session)
constexpr uint64_t kStateStage = 0x18;           // game state: u8, the stage the player is in
constexpr uint64_t kStateSession = 0x90;         // game state: u8 Link Play session bits (kSession*); 0 offline
constexpr uint8_t kSessionOnline = 0x01, kSessionHost = 0x02, kSessionVisitor = 0x04;
constexpr uint64_t kInputMgr = 0x3561718;        // the input manager
constexpr uint64_t kTables = 0x35618f0;          // the data-table manager (the sprite sheet lookups read it too)
constexpr uint64_t kUiMgr = 0x35aebf8;           // the UI manager (UI resource manager: holds the loaded sprite sheets and the layout pools)
constexpr uint64_t kLanguage = 0x36ca680;        // -> u32 language 0..8
constexpr uint64_t kWorld = 0x34efc98;           // the world ([GOT 0x3483b10] points here); +0x6000030 = chunk map u16[64*64], +0x6000018 = packed cells
constexpr uint64_t kWorldRequests = 0x9218378;   // world: Link Play's request queue, u16 q[0..4] (visitor: q[0] sent, q[1] answers taken,
                                                 // q[4] the strict kind in flight, 0xffff none)

// ---- KTGL layouts and their panes ----
// A layout's panes are a list: u16 count, the first node (node +8 the next one, +16 its pane), and the node that ends it (layout + 64).
constexpr size_t kLayPaneCount = 146, kLayFirstNode = 32, kLaySentinel = 64;
constexpr size_t kPaneName = 176, kPaneParent = 208, kPaneWorld = 224, kPaneNameLen = 272;
// kPaneWorld and kPaneLocal point to the pane's two 4x4 float matrices (X axis at [0], Y axis at [4], position at [12]); the update
// before the draw builds them from the animation every frame, world = local x the parent's world.
constexpr size_t kPaneLocal = 232;
// A picture pane's texture record: the four corner UVs of its sprite (8 floats: u, v of top left, top right, bottom left, bottom right),
// not animated. The record is the pane's own: setting a sprite (0xa7c05c) copies the sprite's UVs into it.
constexpr size_t kPaneTexture = 0x1b0, kTextureUv = 8;
// u8, the pane's kind: 2 a text box; 1 and 3 the kinds 0xa7bba0 sets a sprite on (the tab cells' icons are 1).
constexpr size_t kPaneKind = 0x112;
constexpr uint8_t kTextPane = 2;

// ---- the quad builder (0x7101f98aa4): one quad of a pane into the vertex buffer, in the pane's own units ----
// mid: just before the vertices are written: s0 and s1 are the quad's half width and half height, s2 and s3 its centre. Every frame,
// for every quad that is drawn. Where it returns to says which pane class builds; its callers' registers are saved on its stack.
constexpr uint64_t kQuadGeometry = 0x1f98b98;
constexpr size_t kQuadReturn = 8;                  // x29-relative: the return address
constexpr uint64_t kPictureQuadReturn = 0x1f772ec;   // the return into a picture pane's build
constexpr size_t kQuadPictureDraw = 0x58;          // SP-relative: that build's draw object (its x21)
constexpr size_t kDrawPane = 8;                    // draw object: the pane

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
constexpr uint64_t kInputLock = 0xa9ec;          // u8 flags; bit kInputLockBit set: the character takes no input (verified at runtime)
constexpr uint8_t kInputLockBit = 0x20;

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
constexpr uint64_t kStageContainerStore = 0x13ea9a8; // save root: stage item slots (pkp_items.h)
constexpr uint64_t kCabinetRecords = 0xa0ee28;   // save root: stage cabinet records (m_gimmickSaveData)
constexpr uint32_t kCabinetRecordCount = 0x610;
constexpr uint64_t kCabinetRecordSize = 0x10;
constexpr uint64_t kCabinetCellX = 8, kCabinetCellY = 0xa, kCabinetCellZ = 0xc;  // u16 each
constexpr uint64_t kCabinetFlags = 0xe, kCabinetType = 0xf;  // u8 each
constexpr uint8_t kCabinetOccupied = 1;
constexpr uint64_t kCabinetCounts = 0x280bb6c, kCabinetBases = 0x280bbaa;  // main: u16 per container type
constexpr uint64_t kContainerSlotCounts = 0x280baf0, kContainerSlotBases = 0x280bb2e;  // main: u16 per container type

// ---- items in main ----
constexpr uint64_t kItemSlotVtable = 0x322c1d8;  // the vtable every item slot holds
constexpr uint64_t kItemTempFlag = 0x36bbfd8;    // u8 per player: the bag / container screen works on a temp copy (an item is held)
constexpr uint64_t kHeldItem = 0x3684310;        // per player (stride 0x18): the item held in the bag / container screen: +8 packed u32, +0xc u8
                                                 // count at pick-up, +0x10 / +0x16 u16 slot / u8 panel kind it goes back to, +0x12 / +0x17 the
                                                 // last cell it came from, +0x14 u16 count held
constexpr uint64_t kHeldItemSize = 0x18;
constexpr uint64_t kStagedBag = 0x3684340;       // per player (stride 0x838): the bag's temp copy while an item is held {data, len, cap, slots}
constexpr uint64_t kStagedBagSize = 0x838;

// ---- movement component ----
constexpr uint64_t kController = 0xbc0;          // the movement controller (embedded)

// ---- the autosave (ui/pkp_save_now.cpp): the game's own Save and Quit saves through it ----
constexpr uint64_t kStateAutosaveObj = 0x3b9590;  // game state + this: the autosave machine
constexpr uint64_t kAutosaveNow = 0x4577e4;       // (autosave, now, backup): asks it to save; does nothing unless it is on and idle
constexpr uint64_t kStateAutosave = 0x3bb5e0;     // game state: u32, the autosave's step: 0 idle, 1 snapshot, 2 writing, 3..5 finishing
constexpr uint64_t kStateAutosaveFlags = 0x3bb5e8;   // game state: u32, bit 0 the autosave is on, bit 2 a save was asked for now
constexpr uint64_t kStateEvent = 0xd0, kEventBusy = 0x6ea;   // game state: the event bus, its busy byte

// ---- the main loop ----
// mid: the frame step (0xbfc404) where its frame multiplier is joined (s0 the multiplier, s8 the frame time): the game thread, once a
// frame. The feature registry's game-frame tick (pkp_game.cpp) and the frame-rate hook (pkp_frame_rate.cpp) sit here, in that order.
constexpr uint64_t kFrameStep = 0xbfc4fc;

// ---- game functions ----
constexpr uint64_t kOnline = 0x376c4;            // () -> bit 0: Link Play session
constexpr uint64_t kIsOnline = 0x207b0;          // () -> bit 0: a Link Play session is open, host and visitor alike (kOnline is the visitor's bit only)
constexpr uint64_t kMaxStack = 0x2a7f0c;         // () -> the max stack (u16), one value for every item
constexpr uint64_t kNameOf = 0xa34e1c;           // (name id, form) -> UTF-8 name in the game's language
constexpr uint64_t kLayoutOf = 0x9132fc;         // (source) -> layout
constexpr uint64_t kChildOf = 0xa6f92c;          // (children (screen + 0x48), element id, u16* class) -> child widget
constexpr uint64_t kTextString = 0xf35f20;       // (widget, text slot, text id, const char*): a text id with a string argument
constexpr uint32_t kStringText = 0xa8;           // text id "<string;0(lstr1)>"
// The key guide widget's functions, offsets and tables are in pkp_key_guide.cpp, its one user.

// ---- story and characters (read by co-op and its test probe) ----
constexpr uint64_t kQuestWords = 0x4a3702;       // save root: u16 per quest id 1..1000 (index id - 1; 0x4a3700 + id * 2), low byte the state,
                                                 // 0x100 the flag (a flag set on quest 2 in state 3 turned 0x4a3704 0x3 -> 0x103 -> 0x4)
constexpr uint32_t kQuestWordCount = 1000;
// Ids 801..1000 (the DLC quests) are not in that run: the state reader 0x5d85c0 and setter 0x5d865c send them through 0x11e884
// (id - 0x20, low byte: id - 800) to m_story + 0x1860 + (id - 800) * 2, save root 0x4a3f00 + (id - 800) * 2.
constexpr uint32_t kQuestBaseCount = 800;
constexpr uint64_t kQuestDlcWords = 0x4a3f02;    // save root: u16 per quest id 801..1000 (index id - 801)
constexpr uint64_t kQuestWordsEnd = kQuestDlcWords + (kQuestWordCount - kQuestBaseCount) * 2;
// Save root + this: the state word of quest id 1..1000, where the game keeps it.
constexpr uint64_t quest_word(uint32_t id) {
    return id <= kQuestBaseCount ? kQuestWords + (id - 1) * 2 : kQuestDlcWords + (id - kQuestBaseCount - 1) * 2;
}
constexpr uint64_t kSaveAbilities = 0x4a26a8;    // save root: the ability bits, id 1..0xf9 at byte id >> 3, bit id & 7 (0x20 bytes)
constexpr uint64_t kSaveUniqueRequests = 0x6fabd9;   // save root + id * 4: the record of unique request id 1..200 (+0 its state)
constexpr uint32_t kUniqueRequestCount = 200;
constexpr uint64_t kSaveRecipes = 0x6cfdec;      // save root + id * kRecipeSize: m_recipeHistory, the record of recipe id 1..0x5db
constexpr uint64_t kRecipeSize = 6;              // record: +2 u16 the learn order, +4 u8 flags (kRecipeKnown: learned)
constexpr uint64_t kRecipeFlags = 4;
constexpr uint8_t kRecipeKnown = 0x02;
constexpr uint32_t kRecipeMax = 0x5db;          // the learn 0x2e7240 takes id < 0x5dc; its callers test id - 1 <= 0x5da
constexpr uint64_t kCharaStatusRoot = 0x3483b60; // **(main + this): the stage data holding the CharacterStatus records
constexpr uint64_t kCharaStatus = 0x835af0;      // + chara * kCharaStatusStride: m_position (vec4, w 1.0) at +0x10, m_id (u16, data id) at +0x24
constexpr uint64_t kCharaStatusStride = 0x30;
constexpr uint32_t kCharaCount = 1050;           // chara 0..1049 (the save's character arrays)

// ---- m_CraftRefine: a character's item-making job, per chara (< kCharaCount), kCraftWorkSize bytes (VISITOR-SERVICES-RE) ----
// Save root +: the own records (a host, offline), or a visitor's copy of the host's. Every reader selects on the visitor bit (and on
// Cloud Island, a third set at +0xab3db8). A job is recipe 1..kRecipeMax.
constexpr uint64_t kCraftWorkOwn = 0x62820, kCraftWorkHostCopy = 0x1525428, kCraftWorkSize = 0x40;
constexpr uint64_t kCraftWorkTimer = 0x18;       // f32, seconds left (only the host ticks it, 0x3b38f0)
constexpr uint64_t kCraftWorkRecipe = 0x1c;      // u16
constexpr uint64_t kCraftWorkCount = 0x1e;       // u8, items asked for
constexpr uint64_t kCraftWorkMade = 0x1f;        // u8, made so far
constexpr uint64_t kCraftWorkTaken = 0x20;       // u8, taken so far
constexpr uint64_t kCraftWorkDummyEnd = 0x21;    // u8, dummy end approach
constexpr uint64_t kCraftWorkEnd = 0x22;         // u8, finished
constexpr uint64_t kCraftWorkUser = 0x23;        // 16 bytes, the user id the job is for
// ---- user ids (save root +): this process's own, and the session's member table (kMemberCount entries) ----
constexpr uint64_t kOwnUserId = 0x13d3a89, kMemberTable = 0x14c2a48, kMemberSize = 0xb8, kMemberUserId = 0x11;
constexpr uint32_t kMemberCount = 4;
// This player's index in the member table: u8 at kOwnMemberIndex of **(main + kOwnMemberIndexRef) (0x2cfc88 / 0x31d3a0 use the own id
// when it equals the player's index).
constexpr uint64_t kOwnMemberIndexRef = 0x3484d68, kOwnMemberIndex = 0xb3381;
constexpr size_t kUserIdSize = 16;

}  // namespace nxrt::games::pkp::layout
