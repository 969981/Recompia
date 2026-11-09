// SPDX-License-Identifier: GPL-3.0-or-later
// Counters and reports of the replay: occlusion queries, windowed primitive counters, report values, conditional render.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
QueryState g_q;

bool occ_ready() {
    if (g_q.occ_pool) return true;
    if (g_q.occ_tried || !device().features().occlusion_query) return false;
    g_q.occ_tried = true;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kOccSlots;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &g_q.occ_pool) != VK_SUCCESS) g_q.occ_pool = VK_NULL_HANDLE;
    return g_q.occ_pool != VK_NULL_HANDLE;
}

// ---- conditional render ---------------------------------------------------------------------------------------------------------
void note_report(uint64_t address, uint64_t counter) {
    if (!address) return;
    if (g_q.reports.size() > 65536) g_q.reports.clear();   // bounded: a forgotten address reads from memory again
    g_q.reports[address] = counter;
}

// The counter word at a report address; false when it is not known before the GPU runs.
static bool report_counter(uint64_t address, uint64_t* out) {
    auto it = g_q.reports.find(address);
    if (it != g_q.reports.end()) {
        if (it->second == kReportUnknown) return false;
        *out = it->second;
        return true;
    }
    const PoolHit hit = pool_lookup(address);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + 8 > hit.pool->size) return false;
    std::memcpy(out, host_ptr(address), 8);
    return true;
}

bool conditional_render_enabled(uint32_t mode, uint64_t address) {
    uint64_t a = 0, b = 0;
    if (!report_counter(address, &a) || !report_counter(address + 16, &b)) return true;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL) return a == b;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL) return a != b;
    return true;
}

// ---- windowed GPU counters (see QueryState::cw) ----------------------------------------------------------------------------------------
bool cw_ready(CounterWindow& w, bool xfb) {
    if (w.pool) return true;
    if (w.tried) return false;
    w.tried = true;
    const gfx::DeviceFeatures& f = device().features();
    if (xfb ? !(f.transform_feedback && f.transform_feedback_queries) : !f.pipeline_statistics_query) return false;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = xfb ? VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT : VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qi.queryCount = kCounterSlots;
    if (!xfb) qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &w.pool) != VK_SUCCESS) w.pool = VK_NULL_HANDLE;
    return w.pool != VK_NULL_HANDLE;
}

void cw_end_query(CounterWindow& w, bool xfb) {
    if (w.active < 0) return;
    if (xfb) vkCmdEndQueryIndexedEXT(F->cb, w.pool, static_cast<uint32_t>(w.active), 0);
    else vkCmdEndQuery(F->cb, w.pool, static_cast<uint32_t>(w.active));
}

// Does the window that a ResetCounter of `type` opens (up to the next report or reset of that type in the same list) hold a draw whose
// primitives the CPU cannot count: an indirect draw or a DrawTransformFeedback? The record reader is copied; the list itself is not touched.
bool counter_window_has_gpu_draw(RecordReader look, uint32_t type) {
    while (look.next()) {
        const Op op = look.op();
        if ((op == Op::ReportCounter || op == Op::ResetCounter) && look.payload<P_Counter>().type == type) return false;
        if (op == Op::DrawIndirect || op == Op::DrawXfb) return true;
    }
    return false;
}

void QueryState::end_submit() {
    if (occ_active >= 0) {
        vkCmdEndQuery(F->cb, occ_pool, static_cast<uint32_t>(occ_active));
        occ_active = -1;
    }
    for (int k = 0; k < 2; ++k) {
        cw_end_query(cw[k], k == 1);
        cw[k].active = -1;
    }
}

void QueryState::shutdown() {
    for (CounterWindow& w : cw) {
        if (w.pool) vkDestroyQueryPool(vkdev(), w.pool, nullptr);
        w = CounterWindow();
    }
    if (occ_pool) vkDestroyQueryPool(vkdev(), occ_pool, nullptr);
    occ_pool = VK_NULL_HANDLE;
    occ_tried = false;
    occ_active = occ_last = -1;
    prims_generated = prims_xfb = 0;
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
