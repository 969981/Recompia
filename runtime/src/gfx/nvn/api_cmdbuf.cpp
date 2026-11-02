// SPDX-License-Identifier: GPL-3.0-or-later
// NVNcommandBuffer lifecycle: initialize/finalize, command and control memory bookkeeping, Begin/EndRecording, handles,
// Call/CopyCommands. The recorders of the commands themselves are api_cmd_state.cpp and api_cmd_work.cpp.
//
// Command memory model (idea from SuperMarioWonderRecomp docs/nvn/DEBUG.md #1, #26, DEBUG-2 #90: the code is new): the game
// hands over memory (AddCommandMemory / AddControlMemory) and looks at how much a recording used right after EndRecording (a
// list that used nothing is dropped) and at how much is free (to add memory in time). The back end records into host lists, so
// usage is an estimate: 16 bytes of command memory and 4 bytes of control memory per record, growing across recordings, given
// back when memory is added, never reaching the total. The memory callback is never called: a recording that finds the block
// used up gets a region in the buffer's overflow ring instead (CommandBuffer::overflow_base), so every list has a region and the
// overlap rule of nvn_cmd_handles.cpp ends it. Pokopia adds one block per buffer and relies on the callback for the rest; without
// the ring, every list after the block filled up had no region and lived until its buffer was finalized, i.e. forever.
#include <algorithm>
#include <atomic>

#include "nvn_cmd.h"
#include "nvn_impl.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr uint64_t kCmdBytesPerRecord = 16, kCtlBytesPerRecord = 4, kReserve = 64;

CommandBuffer* cb_of(NVNcommandBuffer* c) { return host_of<CommandBuffer>(c); }
const CommandBuffer* cb_of(const NVNcommandBuffer* c) { return host_of<CommandBuffer>(c); }

// Live counts (nxrt/gauge.h). A command buffer counts from its Initialize to its Finalize. Command memory counts the bytes the game handed over
// and that are still in the buffer's block list: AddCommandMemory adds a block, the oldest one is dropped past 8 blocks, and leaving the
// buffer (Finalize, or Initialize over it) drops the rest.
nxrt::GaugeCounter g_command_buffers("nvn.command_buffers");
nxrt::GaugeCounter g_command_memory("nvn.command_memory.bytes");
std::atomic<uint64_t> g_overflow_serial{0};   // numbers the overflow rings (CommandBuffer::overflow_base)

// Caller holds the buffer's lock (or has exclusive use of it).
void command_buffer_gone(const CommandBuffer* cb) {
    g_command_buffers.sub(1);
    for (const CommandBuffer::Block& b : cb->command_blocks) g_command_memory.sub(static_cast<int64_t>(b.size));
}

NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer* c, NVNdevice* device) {
    if (CommandBuffer* old = cb_of(c)) command_buffer_gone(old);   // initialised over a live one: the old one is replaced
    auto* cb = new CommandBuffer();
    cb->device = host_of<Device>(device);
    bind_guest(c, cb);
    g_command_buffers.add(1);
    return 1;
}

void nvnCommandBufferFinalize(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    {
        std::lock_guard<std::mutex> lk(cb->lock);
        for (uint64_t h : cb->handles) handle_release_unbacked(h);   // lists in command memory outlive the buffer (nvn_cmd_handles.cpp)
        cb->handles.clear();
        command_buffer_gone(cb);
    }
    unbind_guest(c);
}

void nvnCommandBufferSetDebugLabel(NVNcommandBuffer* c, const char* label) {
    if (CommandBuffer* cb = cb_of(c)) cb->label = label ? label : "";
}

// NVN2 only (not in the NVN1 table): nvnCommandBufferSetRecordingMode(cmd, mode). The sdk's body is one store, mode -> cmdbuf+0x18 (the game passes
// 0, 1 and 2). What reads it (nvnCommandBufferBarrier, BindProgram, BeginDeviceCommands, ... in the sdk): every command writer takes the word
// from a 5-entry table indexed by the mode and puts it in front of the packet it writes (the command stream's engine or queue header), and
// nvnCommandBufferBarrier does nothing at all in mode 2. So the mode changes how the driver encodes the stream, not what the commands do;
// Barrier in mode 2 is the one visible difference and the replay keeps the barrier (an extra barrier is never wrong). The value is
// kept on the buffer; each mode is logged the first time it is set, so a run shows which ones the game uses.
void nvnCommandBufferSetRecordingMode(NVNcommandBuffer* c, int mode) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    {
        std::lock_guard<std::mutex> lk(cb->lock);
        cb->recording_mode = mode;
    }
    static std::atomic<uint32_t> seen{0};
    const uint32_t bit = 1u << (static_cast<uint32_t>(mode) & 31u);
    if (!(seen.fetch_or(bit) & bit)) NXLOG_I("nvn", "nvnCommandBufferSetRecordingMode(%d): kept on the buffer, the replay records the same commands in every mode (first)", mode);
}

void nvnCommandBufferSetMemoryCallback(NVNcommandBuffer* c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb_fn) {
    if (CommandBuffer* cb = cb_of(c)) cb->callback = reinterpret_cast<uint64_t>(cb_fn);
}
void nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer* c, void* data) {
    if (CommandBuffer* cb = cb_of(c)) cb->callback_data = reinterpret_cast<uint64_t>(data);
}
PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb ? reinterpret_cast<PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC>(cb->callback) : nullptr;
}
void* nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb ? reinterpret_cast<void*>(cb->callback_data) : nullptr;
}

void nvnCommandBufferAddCommandMemory(NVNcommandBuffer* c, const NVNmemoryPool* pool, ptrdiff_t offset, size_t size) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    const MemoryPool* mp = host_of<MemoryPool>(pool);
    std::lock_guard<std::mutex> lk(cb->lock);
    const uint64_t addr = mp ? mp->gpu_base + static_cast<uint64_t>(offset) : static_cast<uint64_t>(offset);
    cb->command_blocks.push_back({addr, static_cast<uint64_t>(offset), size});
    g_command_memory.add(static_cast<int64_t>(size));
    if (cb->command_blocks.size() > 8) {
        g_command_memory.sub(static_cast<int64_t>(cb->command_blocks.front().size));
        cb->command_blocks.erase(cb->command_blocks.begin());
    }
    cb->command_total = size;            // the block the next recording uses
    cb->command_used = 0;                // memory handed over again: its usage is given back
    cb->base_used = 0;
    cb->last_block_addr = addr;
}

void nvnCommandBufferAddControlMemory(NVNcommandBuffer* c, void* memory, size_t size) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    std::lock_guard<std::mutex> lk(cb->lock);
    (void)memory;
    cb->control_total = size;
    cb->control_used = 0;
    cb->control_base = 0;
}

size_t nvnCommandBufferGetCommandMemorySize(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_total : 0; }
size_t nvnCommandBufferGetCommandMemoryUsed(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_used : 0; }
size_t nvnCommandBufferGetCommandMemoryFree(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->command_total - std::min(cb->command_used, cb->command_total) : 0; }
size_t nvnCommandBufferGetControlMemorySize(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_total : 0; }
size_t nvnCommandBufferGetControlMemoryUsed(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_used : 0; }
size_t nvnCommandBufferGetControlMemoryFree(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? cb->control_total - std::min(cb->control_used, cb->control_total) : 0; }

NVNboolean nvnCommandBufferIsRecording(const NVNcommandBuffer* c) {
    const CommandBuffer* cb = cb_of(c);
    return cb && cb->recording ? 1 : 0;
}

void nvnCommandBufferBeginRecording(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return;
    std::lock_guard<std::mutex> lk(cb->lock);
    cb->rec = Ref<CmdList>(new CmdList());
    cb->recording = true;
    cb->tpool = nullptr;
    cb->base_used = cb->command_used;
    cb->control_base = cb->control_used;
}

NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer* c) {
    CommandBuffer* cb = cb_of(c);
    if (!cb) return 0;
    Ref<CmdList> list;
    {
        std::lock_guard<std::mutex> lk(cb->lock);
        if (!cb->recording || !cb->rec) return 0;
        list = std::move(cb->rec);
        cb->recording = false;
        cb->tpool = nullptr;
        const uint64_t cmd_cap = cb->command_total > kReserve ? cb->command_total - kReserve : 0;
        const uint64_t ctl_cap = cb->control_total > kReserve ? cb->control_total - kReserve : 0;
        const uint64_t want = static_cast<uint64_t>(list->record_count) * kCmdBytesPerRecord;
        cb->command_used = std::max(cb->base_used, std::min<uint64_t>(cb->base_used + want, cmd_cap));
        cb->control_used = std::max(cb->control_base, std::min<uint64_t>(cb->control_base + list->record_count * kCtlBytesPerRecord, ctl_cap));
        if (cb->last_block_addr && cb->base_used < cmd_cap) {
            // in the memory the game added; an empty list takes one byte (its handle still names control memory), which the
            // next recording, starting at the same place, overlaps
            list->mem_start = cb->last_block_addr + cb->base_used;
            list->mem_end = cb->last_block_addr + std::max(cb->command_used, cb->base_used + 1);
        } else {
            // no room left (or none added): on the console the out-of-memory callback would have handed over more memory.
            // The list goes in the buffer's overflow ring, as large as the last block (at least 1 MiB), oldest first.
            const uint64_t ring = std::max<uint64_t>(cb->command_total, 1ull << 20);
            const uint64_t size = std::clamp<uint64_t>(want, 1, ring);
            if (!cb->overflow_base) cb->overflow_base = kOverflowAddrBase + ((g_overflow_serial.fetch_add(1) & 0xFFFFF) << 36);
            if (cb->overflow_pos + size > ring) cb->overflow_pos = 0;
            list->mem_start = cb->overflow_base + cb->overflow_pos;
            list->mem_end = list->mem_start + size;
            cb->overflow_pos += size;
        }
    }
    const uint64_t h = handle_register(list);
    std::lock_guard<std::mutex> lk(cb->lock);
    cb->handles.push_back(h);
    if (cb->handles.size() > 4096) cb->handles.erase(cb->handles.begin(), cb->handles.begin() + 2048);   // old ones are dropped by region anyway
    return h;
}

void nvnCommandBufferCallCommands(NVNcommandBuffer* c, int count, const NVNcommandHandle* handles) {
    CommandBuffer* cb = cb_of(c);
    if (!cb || !cb->recording || !cb->rec || count <= 0 || !handles) return;
    P_CallCommands p{static_cast<uint32_t>(count), 0};
    cb->rec->emit(Op::CallCommands, p, handles, static_cast<uint32_t>(count) * 8u);
}

// CopyCommands: the other lists' records are copied into this one now (the console copies the command words).
void nvnCommandBufferCopyCommands(NVNcommandBuffer* c, int count, const NVNcommandHandle* handles) {
    CommandBuffer* cb = cb_of(c);
    if (!cb || !cb->recording || !cb->rec || count <= 0 || !handles) return;
    for (int i = 0; i < count; ++i) {
        Ref<CmdList> src = handle_lookup(handles[i]);
        if (!src) continue;
        cb->rec->bytes.insert(cb->rec->bytes.end(), src->bytes.begin(), src->bytes.end());
        cb->rec->record_count += src->record_count;
        cb->rec->draw_count += src->draw_count;
        for (auto& h : src->held) cb->rec->held.push_back(h);
    }
}

void nvnCommandBufferSetCopyRowStride(NVNcommandBuffer* c, ptrdiff_t stride) { if (CommandBuffer* cb = cb_of(c)) cb->copy_row_stride = stride; }
void nvnCommandBufferSetCopyImageStride(NVNcommandBuffer* c, ptrdiff_t stride) { if (CommandBuffer* cb = cb_of(c)) cb->copy_image_stride = stride; }
ptrdiff_t nvnCommandBufferGetCopyRowStride(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? static_cast<ptrdiff_t>(cb->copy_row_stride) : 0; }
ptrdiff_t nvnCommandBufferGetCopyImageStride(const NVNcommandBuffer* c) { const CommandBuffer* cb = cb_of(c); return cb ? static_cast<ptrdiff_t>(cb->copy_image_stride) : 0; }

}  // namespace

void register_api_cmdbuf() {
    NVN_REG(nvnCommandBufferInitialize);
    NVN_REG(nvnCommandBufferFinalize);
    NVN_REG(nvnCommandBufferSetDebugLabel);
    NVN_REG(nvnCommandBufferSetMemoryCallback);
    NVN_REG(nvnCommandBufferSetMemoryCallbackData);
    NVN_REG(nvnCommandBufferGetMemoryCallback);
    NVN_REG(nvnCommandBufferGetMemoryCallbackData);
    NVN_REG(nvnCommandBufferAddCommandMemory);
    NVN_REG(nvnCommandBufferAddControlMemory);
    NVN_REG(nvnCommandBufferGetCommandMemorySize);
    NVN_REG(nvnCommandBufferGetCommandMemoryUsed);
    NVN_REG(nvnCommandBufferGetCommandMemoryFree);
    NVN_REG(nvnCommandBufferGetControlMemorySize);
    NVN_REG(nvnCommandBufferGetControlMemoryUsed);
    NVN_REG(nvnCommandBufferGetControlMemoryFree);
    NVN_REG(nvnCommandBufferIsRecording);
    NVN_REG(nvnCommandBufferBeginRecording);
    NVN_REG(nvnCommandBufferEndRecording);
    NVN_REG(nvnCommandBufferCallCommands);
    NVN_REG(nvnCommandBufferCopyCommands);
    NVN_REG(nvnCommandBufferSetCopyRowStride);
    NVN_REG(nvnCommandBufferSetCopyImageStride);
    NVN_REG(nvnCommandBufferGetCopyRowStride);
    NVN_REG(nvnCommandBufferGetCopyImageStride);
    NVN2_REG(nvnCommandBufferSetRecordingMode);
}

}  // namespace nxrt::nvn
