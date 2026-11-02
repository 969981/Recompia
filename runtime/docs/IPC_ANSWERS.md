# Deliberate answers for commands Pokopia sends (no stub_result needed)

`[ipc] stub_result` / `NXRT_IPC_STUB_RESULT` defaults to `kStubService` (0xBCBD5): an unknown command fails and is logged at Warn
once per (service, command), with counts in the end-of-run summary. Every command that reached the stub path in the Pokopia run logs
(play/ and work/run*/, Nov 2026), plus the ones work/audit/hle.md flags, now has its own answer. Layouts come from Switchbrew where it
has the id, otherwise from the SDK client proxy (address given).

| Service / interface | Cmd | Answer | Why / source |
|---|---|---|---|
| `ssl:s` | all | same ISslService as `ssl` (CreateContext returns a real context; 5 SetInterfaceVersion ok) | Switch 2 name of ssl; was a StubService |
| nn::hid::IHidServer | 3110-3113, 3131, 3136-3139 | InitializeJcms / FinalizeJcms / StartJcms / StopJcms / FinalizeNmouse / SetNmouseButtonConfigJoy{Left,Right}Jcms / SetMergedNmouseSourceTypes / InitializeNmouse: success, no output; 3139 and 3110 keep the transfer memory and the sampler writes the Joy-Con 2 mouse into it | sdk proxy thunks 0x75006701b4..0x75006702e8 matched to the nn::hid::detail callers; `docs/hid_mouse_sw2.md` |
| nn::hid::IHidServer | 214 | SendVibrationValues (Switch 2 form, extra u16 = 2): stored and forwarded to the pad rumble like 206 | sdk nn::hid::detail::SendVibrationValues 0x750064d360 (vtable slot 0x2f0) |
| IApplicationFunctions | 320 | success, 2 zero bytes | sdk proxy 0x75001bd9f8 reads two u8; name unknown |
| IApplicationFunctions | 321 | success, 1 zero byte | sdk proxy 0x75001bda04 reads one u8; name unknown |
| ILibraryAppletAccessor | 90 | success, no output (input bool) | sdk proxy 0x75001bd560; name unknown |
| ILibraryAppletCreator | CreateLibraryApplet(Ex), inline swkbd | refused with 128-712 (was 128-35) | the SDK fails gracefully only on am 701..749; 128-35 made it svcBreak |
| stpl:u IUserService | 1 GetRequiredMemorySize | success, u32 0 | SDK aborts on failure (0x75006ad634); no work memory needed |
| stpl:u IUserService | 300 GetHidSharedMemory | success, {size 0x40000, offset 0} + zeroed shared memory | SDK aborts on failure (0x75006b9744); no guests connected |
| stpl:u IUserService | others | not implemented (fails, logged) | GameShare is not available |
| IManagerForApplication | 3 LoadIdTokenCacheDeprecated, 4 LoadIdTokenCache | 124-200 NetworkServiceAccountRegistrationRequired | no NSA token; SDK wrapper returns it to the game |
| IManagerForApplication | 170 LoadNetworkServiceLicenseKindAsync | async context: done, GetResult fails (110-300) | offline, no Switch Online licence |
| IAsyncNetworkServiceLicenseKindContext | 100 GetNetworkServiceLicenseKind | success, u32 0 (none) | SDK aborts if it fails (0x750018c634) |
