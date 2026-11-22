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
// save root: the areas' progress (coop-v2 ENVIRONMENT-RE 3, AREA-GATE-RE; m_lastGate and m_playSkitInGateNum are likely, from
// save_fields.tsv). The getter 0x58042c(stage) reads the mirror on a visitor (0x376c4) and the Cloud Island byte for its stage; the
// setter is 0x5804ec(stage, level). Gates: setter 0x5c059c(id) sets bit 0 of history+id, but writes nothing while a session is open
// (0x207b0); getter 0x5c0504(id) reads the visitor mirror on a visitor, else the history.
constexpr uint64_t kSaveAreaLevel = 0x741494;         // u8[32] m_developmentLevel by stage (this save's own)
constexpr uint64_t kSaveAreaLevelMax = 0x7414b4;      // u8[32] m_maxDevelopmentLevel by stage
constexpr uint64_t kSaveAreaLevelMirror = 0x1718490;  // u8[32] the host's levels as a visitor sees them
constexpr uint64_t kSaveCloudLevel = 0xfd84a7;        // u8 Cloud Island's level
constexpr uint8_t kCloudStage = 0x11;
constexpr size_t kAreaStages = 32;
constexpr size_t kGateHistoryBytes = 32;
constexpr uint64_t kSaveLastGate = 0x13d3a59;         // u8 STGDAT m_lastGate (the current stage's)
constexpr uint64_t kSaveSkitInGate = 0x7416d1;        // u8 m_playSkitInGateNum
constexpr uint64_t kSaveTrainerLevel = 0x4a4400;      // u8 m_trainerLevel (the rank a gate asks for; own save, no visitor copy)
constexpr uint64_t kSaveCleanCount = 0x4a4401;        // u8 the sludge cleanings counted (1..99; unique request type 0x2c reads it)
constexpr uint64_t kGameState = 0x3561710;       // the game state (stage, session)
constexpr uint64_t kStateStage = 0x18;           // game state: u8, the stage the player is in
constexpr uint64_t kStateSession = 0x90;         // game state: u8 Link Play session bits (kSession*); 0 offline
constexpr uint64_t kStateOwnBag = 0x11;          // game state: u8, bit 0 set: the bag selectors 0x2ecc04 / 0x2e70e0 take the offline bag
                                                 // even in a visit (coop-v2 P0-RE 0x2e70e0)
constexpr uint8_t kSessionOnline = 0x01, kSessionHost = 0x02, kSessionVisitor = 0x04;
constexpr uint64_t kInputMgr = 0x3561718;        // the input manager
constexpr uint64_t kTables = 0x35618f0;          // the data-table manager (the sprite sheet lookups read it too)
constexpr uint64_t kUiMgr = 0x35aebf8;           // the UI manager (UI resource manager: holds the loaded sprite sheets and the layout pools)
constexpr uint64_t kLanguage = 0x36ca680;        // -> u32 language 0..8
constexpr uint64_t kWorld = 0x34efc98;           // the world ([GOT 0x3483b10] points here)
// world: -> the chunk map, u16[64*64], the chunk slot of each 32 x 32 column at (x >> 5) + (z >> 5) * 64 (256 or more: none), and
// -> the cells, kChunkBytes per slot, 3 bytes per cell at (y << 10) | ((z & 31) << 5) | (x & 31), block id = u16 & 0x7ff
constexpr uint64_t kWorldChunkMap = 0x6000030, kWorldCells = 0x6000018;
constexpr uint64_t kChunkBytes = 0x60000;
constexpr uint32_t kChunkSlots = 256, kChunkColumns = 64 * 64;
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
constexpr uint64_t kBagOnline = 0x10;           // common block: the bag's slots in a Link Play visit and on Cloud Island (R+0x13d4930)
constexpr uint32_t kBagWholeSlots = 130;        // a whole bag: the 120 slots, then inventory kind 4's 10 (bag + 0x780); 0x820 bytes, as
                                                // the game copies one (its per-user table, the host's visitor mirror)
constexpr uint64_t kCommonFlags = 0xc074;       // common block: u8; bit 1 set while a second local player (GameShare) has the online bag
constexpr uint8_t kCommonFlagShare = 2;
// The host's working copy of each visitor's bag (save root +, kMemberCount whole bags by member table index; its getter 0x5bc840 has
// none for the index at kOwnMirrorIndex): the host's request checks read and change it.
constexpr uint64_t kBagMirror = 0x17154a8;
constexpr uint64_t kOwnMirrorIndex = 0xb3380;   // u8 of **(main + kOwnMemberIndexRef): the member index 0x5bc840 has no mirror for
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
constexpr uint64_t kBagScreenVtable = 0x333ba70; // the vtable pointer the bag / container screen object (UI class 0x8d) holds
constexpr uint64_t kItemBarRefresh = 0xa8e724;   // (local player slot, 0, 0): the HUD's item bar reads the bag again
constexpr uint64_t kResourceWaitEnd = 0x46aa6c;  // (): clears the two wait bytes of the object at main+0x35650f8 that a request kind 0xb
                                                 // reply releases on either answer

// ---- movement component ----
constexpr uint64_t kController = 0xbc0;          // the movement controller (embedded)

// ---- the autosave (ui/pkp_save_now.cpp): the game's own Save and Quit saves through it ----
constexpr uint64_t kStateAutosaveObj = 0x3b9590;  // game state + this: the autosave machine
constexpr uint64_t kAutosaveNow = 0x4577e4;       // (autosave, now, backup): asks it to save; does nothing unless it is on and idle
constexpr uint64_t kStateAutosave = 0x3bb5e0;     // game state: u32, the autosave's step: 0 idle, 1 snapshot, 2 writing, 3..5 finishing
constexpr uint64_t kStateAutosaveFlags = 0x3bb5e8;   // game state: u32, bit 0 the autosave is on, bit 2 a save was asked for now
constexpr uint64_t kWorldDirty = 0x3558bc4;       // main + : u8, "the world changed since the last save"; the autosave's step 1 stops
                                                  // without writing while it is 0 (0x5adc94); saves clear it (pkp_coop_takeout.cpp)
constexpr uint64_t kSaveManager = 0x35619d0;      // main + : the save manager (its job queue; the autosave's step 2 waits on it)
constexpr uint64_t kSaveMgrJob = 0xb8, kSaveJobResult = 0x608;   // manager + : the last job; job + : u32 its result, 0 written
                                                  // (the autosave's step 2 / 3, 0x5add04 / 0x5add68: null job or 0 = written)
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
// The role getters as co-op's role answers name them (pkp_coop_role.cpp; kOnline above is IsVisitor under its older name).
constexpr uint64_t kIsVisitor = 0x376c4;         // () -> GM+0x90 bit 2: a Link Play visitor (0 with no game state)
constexpr uint64_t kIsHost = 0xa40bc;            // () -> GM+0x90 bit 1: the Link Play host (0 with no game state)
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
// ---- m_itemHistory: save root + item id, one byte per item (0..0x11f7; modding-docs coop-v2/HAIRSTYLES-RE.md) ----
// Bit 0 obtained (0x2e51d8 on a pickup, 0x2f6700 / 0x2f5f20 for a learned hairstyle / look); bit 1 the first obtain's mark and bit 6
// (0x2e51d8), bit 4 tested with bit 0 (`& 0x11`) by the known tests 0x2f6580 / 0x2f5da0. A learned hairstyle is bit 0 of its hidden
// hair item (item kind 20, items kHairItemFirst..+kHairItemCount-1), a learned look (outfit) of its look item (kind 19).
constexpr uint64_t kSaveItemHistory = 0x6d6179;
constexpr uint32_t kHairItemFirst = 2460, kHairItemCount = 13;
constexpr uint32_t kLookItemFirst = 2400, kLookItemCount = 25;
// ---- area gates (modding-docs coop-v2/AREA-GATE-RE.md) ----
// Save root + id: m_gateHistory, bit 0 per gate id 0..kGateCount-1 (opened; the door's second press enters). Setter 0x5c059c refuses
// whenever a Link Play session is open; getter 0x5c0504 reads kSaveGateMirror + id on a visitor (0x376c4), which no game code fills.
constexpr uint64_t kSaveGateHistory = 0x73b1aa;
constexpr uint64_t kSaveGateMirror = 0x1718470;
constexpr uint32_t kGateCount = 0x20;
// ---- Life Coins and challenges (town goals; modding-docs coop-v2/MONEY-RE.md) ----
constexpr uint64_t kSaveMoney = 0x7416ac;        // save root: u32 Life Coins (CMNDAT; getter 0x645ee8, setter 0x645ebc caps it)
constexpr uint32_t kMoneyMax = 999999;
constexpr uint64_t kChallengeManager = 0x3565170;   // *(main + this): CM, the challenge manager (a visitor's mirror: CM + id * 0x12)
constexpr uint64_t kSaveChallenges = 0x6fb0ae;   // save root + id * kChallengeSize: u8 the state of challenge id 1..kChallengeMax,
                                                 // the player's own (m_challengeStateHistory, CMNDAT; stage 0x11 has its own set)
constexpr uint64_t kChallengeSize = 0xd8;
constexpr uint32_t kChallengeMax = 0x4b0;
constexpr uint8_t kChallengeClaimed = 4;         // 1..2 open, 3 done (to claim), 4 claimed: the coins were paid
constexpr uint8_t kStageCloudIsland = 0x11;      // kStateStage: Cloud Island (0x77ecb0), whose challenges are its own
// the per-player claims (pkp_coop_money.cpp): CM + id * kChallengeMirrorSize is a visitor's mirror of challenge id
constexpr uint8_t kChallengeDone = 3;
constexpr uint64_t kChallengeMirrorSize = 0x12;
constexpr uint64_t kMirrorState = 0xc;           // u8 this player's view (the PC's list 0x645f20 offers a claim at 3)
constexpr uint64_t kMirrorHostState = 0xd;       // u8 the host's own state, as its goal update 0x6355c0 last sent it (@0x635964)
// CM + kChallengeAreas + area * kChallengeAreaSize, area 0..31: the per-area entry the refresh 0x62f62c clears (0x140 bytes) and
// 0x62f7e0 then fills, on the host only (it returns at once on a visitor). +0 / +4 s16 pairs (first, count) into the area's challenge
// lists, +8 u8 the area is open (the host's current area, or its visited bit; all of 1..31 on Cloud Island), which the area gate
// 0x630fb0 reads for area 1..kChallengeAreaMax on the host (shop and challenge UI). modding-docs coop-v2/SHOP-CATALOG-RE.md
constexpr uint64_t kChallengeAreas = 0x17dd6, kChallengeAreaSize = 10, kChallengeAreaOpen = 8;
constexpr uint32_t kChallengeAreaMax = 31;
constexpr uint64_t kChallengeList = 0x18114;     // CM +: u16 ids of the listed challenges (the PC's; the kind 7 raise needs the id here)
constexpr uint64_t kChallengeListCount = 0x18600;   // CM +: s16 how many
constexpr int16_t kChallengeListMax = 0x276;
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

// ---- the story records (pkp_coop_records.cpp; modding-docs coop-v2/role-census/followup/STORY-RECORDS.md) ----
// Save root +: m_effectCommandSaveData (STGDAT, m_saveStage R+0x7419b0 + 0x2311f0 = R+0x972ba0, serializer 0x5f82ec): its fields
// m_flags +0x8 .. m_sRealTimer +0x1c4..+0x1eb; +0x0..+0x7 is the field object's vtable pointer, not data.
constexpr uint64_t kSaveEffectCommands = 0x972ba8;
constexpr size_t kEffectCommandsBytes = 0x1e4;
// CMNDAT m_story (R+0x4a26a0) +0x12c m_charaTownFlag and +0xc40 m_targetStageForEnableMoveStage (u8 per chara index, raw 0x400 each),
// +0x2c m_storyStageUniqueFlag (8 bytes per stage 0..31); the hint-group flags (128 group bits).
constexpr uint64_t kSaveTownFlags = 0x4a27cc, kSaveTownTargets = 0x4a32e0;
constexpr size_t kTownRecordBytes = 0x400;
constexpr uint64_t kSaveStageFlags = 0x4a26cc;
constexpr size_t kStageFlagsBytes = 0x100;
constexpr uint64_t kSaveHintGroups = 0x7416bc;
constexpr size_t kHintGroupsBytes = 0x10;
// ---- co-op friendship (pkp_coop_friendship.cpp): a creature's 0x180-byte record, per chara (< kFriendCount), save root + ----
// The own records (a host, offline), a visitor's copy of the host's, Cloud Island's own set; every reader selects on the visitor bit
// and on Cloud Island (0x3b7708, 0x3b7e48). The once-gates: a gain's caller skips it while its byte is set and sets it after.
constexpr uint64_t kFriendOwn = 0x120, kFriendHostCopy = 0x14c2d28, kFriendCloud = 0xa516b8, kFriendSize = 0x180;
constexpr uint32_t kFriendCount = 0x41a;
constexpr uint64_t kFriendPoints = 0x2c;     // u16, the friendship points (0x31fb00 reads, 0x3bb660 writes)
constexpr uint64_t kFriendMeter = 0x2e;      // u16, filled at a friendship level-up (0x3bc034), emptied by the reward list
constexpr uint64_t kFriendGift = 0x175;      // u8 once-gate of a gift's friendship (0x46e308)
constexpr uint64_t kFriendDaily = 0x177;     // u8 once-gate of the talk / play friendship (0x4aae54 slot 174 and five more callers)
constexpr uint64_t kFriendFlag17a = 0x17a;   // u8 set by script callback slot 175 (0x4ab268)
constexpr uint64_t kFriendOnce17d = 0x17d;   // u8 once-gate of 0x3a83a0's friendship (0x3a8954 reads, 0x3a8d44 sets)
// ---- gimmick ownership (pkp_coop_owner.cpp; modding-docs coop-v2/role-census/build/owner.md) ----
constexpr uint64_t kOwnIndexOf = 0x25a480;          // () -> u8 this process's member index (NM+0xb3381), the owner byte's value
constexpr uint64_t kGimmickOwnerApply = 0x538f60;   // (&kind, x, y, z, &idx, take, quiet, slot): an ownership take or release
constexpr uint64_t kGimmickOwnerMsg = 0x74fd48;     // (msg): builds a SyncGimmickOwnership message
constexpr uint64_t kSendToHostForced = 0x30c69c;    // (msg): sends a message to the host, past the NM+0xb3348 send gate
constexpr uint64_t kSendGate = 0x25af7c;            // () -> u8 NM+0xb3348, the gate 0x2cb584 drops a message at
// ---- the session's doors and the start (pkp_coop_doors.cpp, pkp_coop_title.cpp) ----
// The PC's Link Play screen: u32 +kLinkMenuState its state; kLinkMenuEnter(screen, state) enters one (it calls the entry 0xf282a8).
constexpr uint64_t kLinkMenuEnter = 0xf266d0, kLinkMenuState = 0xa4;
// The pause menu (UI class 0xba): u32 +kPauseMenuState its state, u8 +kPauseMenuMember the member a choice names (0xff: none);
// kPauseMenuEnter(menu, state, 1) enters one (it calls the entry 0xfb9ea0).
constexpr uint64_t kPauseMenuEnter = 0xfb8f58, kPauseMenuState = 0xa4, kPauseMenuMember = 0x108;
// The UI objects: *(main+kUiOwner) +kUiList the list (8-byte entries), +kUiCount their number; the title is entry kTitleClass. The
// title object: u32 +kTitleState, u32 +kTitleResult (-1 until answered; 1 continue), u8 +kTitleInput (input on).
constexpr uint64_t kUiOwner = 0x3561780, kUiList = 0x18, kUiCount = 0x20, kTitleClass = 0x65;
constexpr uint64_t kTitleState = 0xa4, kTitleResult = 0xa8, kTitleInput = 0xb8;
constexpr uint64_t kTitleNameMissing = 0xf71250;   // () -> bit 0: the player's name needs the name entry before the save continues
// Save root + kSaveFlags (the CMNDAT header copy): bit kSaveUncleanExit = the last save was written online ("did not disconnect").
constexpr uint64_t kSaveFlags = 0xa0;
constexpr uint8_t kSaveUncleanExit = 0x2;
// ---- gimmick ownership, the tester log (pkp_coop_owner.cpp) ----
constexpr uint64_t kPlacedObjects = 0x35616f0;      // [main+]: the placed-object manager (its map at +0x46d0)
constexpr uint64_t kPlacedObjectAt = 0x753200;      // (manager, x, y, z) -> the placed object at the cell, 0 when none
// ---- the story's materials drop (pkp_coop_deposit.cpp, pkp_coop_records.cpp; modding-docs coop-v2/role-census/build/
// materials-deposit.md): class 0x326dc98, placed kind 0x1d9, container type 0x11 (one record, slots 1295..1297 of the common store) ----
constexpr uint64_t kDropClass = 0x326dc98;          // main +: the class's vtable
constexpr uint16_t kDropKind = 0x1d9;
constexpr uint8_t kDropType = 0x11;                 // its container type (gimmick +0x4b)
constexpr uint64_t kDropStage = 0x4a, kDropScreen = 0x4c, kDropTimer = 0x50;   // gimmick +: u8 stage, u8 screen up, f32 open timer
constexpr uint64_t kDropUse = 0x7e77a4;             // (g, player selector): the class's use after the press's free check
constexpr uint64_t kDropBlock = 0x7e7f20;           // (g, open): the block's open state, as the use and the close set it
constexpr uint64_t kPlayerEvent = 0x74fb80;         // (event, on, ptr, selector, 0): the use's and the close's event 0xf
constexpr uint64_t kDropEventPtr = 0x3483b38;       // GOT: the pointer the close passes to kPlayerEvent as x2
constexpr uint64_t kSceneQueue = 0x2e6ce0;          // (selector, scene, &u16, -1, 0, 0, 0, 0; stack u8 +0, u16 +8, u8 +0x10)
constexpr uint64_t kSceneActorPtr = 0x3482ef8;      // GOT: the u16 the close passes by address as x2 of kSceneQueue
constexpr uint64_t kStoreSlice = 0x5b1c84;          // (holder, type, id, temp, &first, &count) -> the store the slice is in
constexpr uint64_t kStoreHolder = 0x13d4920;        // save root +: the item holder (common store at +0x1050)
constexpr uint64_t kFlagTest = 0x377cc;             // (flag) -> bit 0: the save's flag is set
constexpr uint64_t kDropTest = 0x5b8e34, kDropTestArg = 0x1718bb0;   // (save root + arg) -> bit 0: stage 8's condition (category 0x13)
constexpr uint64_t kSaveDrop = 0x73b258;            // save root +: m_rocketHouse (2 x 16 B): +0 u32 x | y << 16, +4 u16 z,
constexpr uint64_t kSaveCampaignHistory = 0x6cec38; // save root +: m_eventCampaignHistory, 15 slots x 6 B (slot 0 unused, areas 1..14):
constexpr size_t kCampaignHistoryBytes = 0x5a;      //   byte 0 bit 0 / bit 3 read by the campaign creatures' talk test 0x29d910 / 0x284660
constexpr size_t kDropRecordBytes = 0x20;           //   +8 u32 electricity (category 1), +0xc u16 the stage
constexpr uint64_t kDropStagesTable = 0xa20;        // data-table manager +0x10 +: #1108, the stages (16 B rows)
constexpr uint64_t kDropItemsTable = 0x3d0;         // data-table manager +0x10 +: #514, u16 item and u8 category per row (101 rows)
constexpr uint64_t kPlayedScenes = 0x4a2cec;        // save root +: m_playedContentsSkitFlag, scene s at bit s & 7 of byte s / 8
// ---- co-op, a creature's talk record (pkp_coop_talkrec.cpp): 0x38 bytes per chara (< kFriendCount), save root + ----
constexpr uint64_t kTalkRecOwn = 0x93ba0, kTalkRecHostCopy = 0x15567a8, kTalkRecCloud = 0xae5138, kTalkRecSize = 0x38;
constexpr uint64_t kTalkRecArmed = 0x10;   // u8, set by script callback slot 195 (0x4ae68c), read by conditions 29 / 31 of main+0x34c3060
// ---- a dropped player is waited for (pkp_coop_drop.cpp; modding-docs coop-session/LEAVE-DOORS-RE.md, DISCONNECT-RE.md) ----
// The trip filers (main offsets of their entries): 0xbd45c(record, reason, member, flag) files a leave or a send-home, 0xbdc68(record)
// the host's Close the Connection; both file only while the record's u32 phase (game state + kTripRecord) is >= kTripRecordReady.
constexpr uint64_t kFileTrip = 0xbd45c, kCloseTrip = 0xbdc68;
constexpr uint64_t kTripRecord = 0x3bb600;
constexpr uint32_t kTripRecordReady = 5;
// Return addresses into the filers' callers: the pause menu's 0xbd45c call (state 0x10: reason 0 the host disbands, 4 a teammate
// leaves; doors refuse reason 2) and the kick handler 0x5483a0's (RequestRejectMember: reason 1, kind 8 home).
constexpr uint64_t kPauseFileReturn = 0xfba740, kKickFileReturn = 0x54841c;
constexpr uint32_t kTripDisband = 0, kTripLeave = 4;
// kStateAutosaveFlags: a save was asked for now (the autosave starts it at its next step).
constexpr uint32_t kAutosaveAskedNow = 0x4;
// The network manager NM = *(main + kNetManager); NM + kNetQualityTimer: f32 seconds a member's ping has been over 1499 (0x53ca60;
// at 20 s it self-disconnects, reason 0).
constexpr uint64_t kNetManager = 0x3561a20, kNetQualityTimer = 0xb3374;
// The same step's results: the level (u32; 0 good, 1 and 2 "unstable", 3 the self-disconnect), the worst member's loss (10000 minus
// its received / sent ratio in 1/10000, i32) and ping (ms, i32).
constexpr uint64_t kNetQualityLevel = 0xb336c, kNetQualityLoss = 0xb3378, kNetQualityPing = 0xb337c;
// Error 3103: KTGL_ONLINE_E_SESSIONP2P_NETWORK_LOST, the result 0x547278 shows when the session's network is gone.
constexpr uint32_t kErrorSessionLost = 0x80401103;

}  // namespace nxrt::games::pkp::layout
