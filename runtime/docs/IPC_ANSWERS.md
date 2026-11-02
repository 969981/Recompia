# Deliberate answers for commands Pokopia sends (no stub_result needed)

`[ipc] stub_result` / `NXRT_IPC_STUB_RESULT` defaults to `kStubService` (0xBCBD5): an unknown command fails and is logged at Warn
once per (service, command), with counts in the end-of-run summary. Every command that reached the stub path in the Pokopia run logs
(play/ and work/run*/, Nov 2026), plus the ones work/audit/hle.md flags, now has its own answer. Layouts come from Switchbrew where it
has the id, otherwise from the SDK client proxy (address given).

| Service / interface | Cmd | Answer | Why / source |
|---|---|---|---|
| `ssl:s` | all | same ISslService as `ssl` (CreateContext returns a real context; 5 SetInterfaceVersion ok) | Switch 2 name of ssl; was a StubService |
| nn::hid::IHidServer | 3110-3113, 3130-3133, 3135-3143 | Joy-Con 2 mouse (Nmouse / Jcms), none returns data except 3133. 3139 / 3130 InitializeNmouse (both forms) and 3110 InitializeJcms keep the transfer memory, list a merged mouse and a Joy-Con 2 (R) mouse and the sampler writes them (pointer, pointer range, wheel, buttons, attributes bit 1, sampling numbers); 3111 / 3131 drop it; 3112 / 3113 Start/StopJcms accepted (the rings always sample); 3132 sets and 3133 returns the pointer range; 3135 EnableShowingCursor and 3141 EnableNmousePointer write the byte the client reads back; 3136-3138 stored; 3140 SetNmousePointerPosition logged (the PC mouse is not moved); 3142 RestrictNmousePointer and 3143 (unknown) accepted. Pokopia sends 3110-3112, 3131, 3136-3139 | sdk proxy thunks 0x75006701b4..0x75006702ec (vtable slots 0x538..0x5B8) matched to the nn::hid::detail callers; layouts from the thunks' CMIF writers; `docs/hid_mouse_sw2.md` |
| nn::hid::IHidServer | 214 | SendVibrationValues (Switch 2 form, extra u16 = 2): stored and forwarded to the pad rumble like 206 | sdk nn::hid::detail::SendVibrationValues 0x750064d360 (vtable slot 0x2f0) |
| IApplicationFunctions | 320 | success, 2 zero bytes | sdk proxy 0x75001bd9f8 reads two u8; name unknown |
| IApplicationFunctions | 321 | success, 1 zero byte | sdk proxy 0x75001bda04 reads one u8; name unknown |
| ILibraryAppletAccessor | 90 | success, no output (input bool) | sdk proxy 0x75001bd560; name unknown |
| ILibraryAppletCreator | CreateLibraryApplet(Ex), inline swkbd | refused with 128-712 (was 128-35) | the SDK fails gracefully only on am 701..749; 128-35 made it svcBreak |
| stpl:u IUserService | 1 GetRequiredMemorySize | success, u32 0 | SDK aborts on failure (0x75006ad634); no work memory needed |
| stpl:u IUserService | 300 GetHidSharedMemory | success, {size 0x84000, offset 0} + zeroed shared memory | SDK aborts on failure (0x75006b9744). 0x84000 is how far HidSharedMemoryAccessor reads (guest pads, touch screens, Jcms; layout in misc.cpp); all zero = no guest pad (style set 0) and empty rings, i.e. no GameShare session |
| stpl:u IUserService | others | not implemented (fails, logged) | GameShare is not available |
| IApplicationFunctions | 140 GetFriendInvitationStorageChannelEvent | success, an event that never fires | nn::friends::GetFriendInvitationNotificationEvent (sdk 0x75003d8968, vtable slot 0x1B0) aborts on failure; offline, no invitations arrive |
| IApplicationFunctions | 141 TryPopFromFriendInvitationStorageChannel | 128-2 (no data) | nn::friends::TryPopFriendInvitationNotificationInfo (0x75003d89f0, slot 0x1B8) reads 128-2 as "none" and returns false; any other failure aborts |
| ILibraryAppletCreator / accessor | MyPage applet 0x100001A (friend invitation) | created; Start ends it at once, GetResult 128-22 (cancelled) | nn::friends::StartFriendInvitation / StartSendingFriendInvitation (0x75003e32a0, 0x75003e33b0) run it with arg kind 8 / 9; creating must succeed (CreateMyPageApplet aborts otherwise). Cancelled makes the friends call return 121-30 (0x3C79): the game ignores it for StartFriendInvitation (main 0x7101b2a5a0) and takes its failure branch for StartSending (main 0x7101b2a7ec). Offline: no invitation sent. The friends service itself gets no invitation IPC from these calls |
| IManagerForApplication | 3 LoadIdTokenCacheDeprecated, 4 LoadIdTokenCache | 124-200 NetworkServiceAccountRegistrationRequired | no NSA token; SDK wrapper returns it to the game |
| IManagerForApplication | 170 LoadNetworkServiceLicenseKindAsync | async context: done, GetResult fails (110-300) | offline, no Switch Online licence |
| IAsyncNetworkServiceLicenseKindContext | 100 GetNetworkServiceLicenseKind | success, u32 0 (none) | SDK aborts if it fails (0x750018c634) |
