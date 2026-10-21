// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/debugFlags.h"
#include "common/profiler.h"
#include "common/telemetry.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <string>
#include <unwind.h>
#include <pthread.h>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#if defined(__ANDROID__) || defined(ANDROID)
#include <android/log.h>
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "CapyPS5-Gpu", __VA_ARGS__)
#else
#define ALOGI(...) std::printf(__VA_ARGS__)
#endif

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

// Ring for per-draw data and fresh copies of CPU-modified buffers. Asterix XXL2 fills 64 MiB
// every frame and waits for the GPU (20 FPS); 128+ MiB reaches 30 FPS but breaks its floor and
// distant buildings (cause unknown, see HANDOFF). `setprop debug.capy.stream_mb N` (read at start).
uint64_t StreamBufferSize() {
	Common::DebugFlags::CachedValue mb("debug.capy.stream_mb", 64);
	return static_cast<uint64_t>(std::clamp<int64_t>(mb.Get(), 16, 1024)) * MiB;
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
	// A buffer that BDA shaders can now reach must be synchronized at the next PrepareBda.
	const auto& buffer = m_slot_buffers[id];
	m_memory_tracker.LogCpuModified(buffer.CpuAddress(), buffer.Size());
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugFlags::FrameTrace("    download %zu ranges, %llu bytes", copies.size(),
	                               static_cast<unsigned long long>(total_size));

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
		if constexpr (async) {
			std::scoped_lock lock {m_async_download_lock};
			for (const auto& copy: copies) {
				m_async_download_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
			}
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, StreamBufferSize()),
      m_descriptor_buffer(graphics, scheduler, MemoryUsage::Stream, 16 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	// Little Nightmares places shader code with GPU copies; read it only once they landed.
	ShaderSetCodeSync([this](uint64_t vaddr, uint64_t size) {
		return GuestGpu::IsGpuThread() && SyncForRead(vaddr, size);
	});
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Gpu", "BufferCache: stream buffer coherent=%d, staging coherent=%d",
	                    m_stream_buffer.IsCoherent() ? 1 : 0, m_staging_buffer.IsCoherent() ? 1 : 0);
#endif
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

bool BufferCache::FillMemory(uint64_t vaddr, uint64_t size, uint32_t value) {
	if (!GuestGpu::IsGpuThread() || !GuestRange {vaddr, size}.Valid() ||
	    vaddr % TRACKER_PAGE_SIZE != 0 || size % TRACKER_PAGE_SIZE != 0) {
		return false;
	}
	std::vector<uint32_t> words(size / sizeof(uint32_t), value);
	// The fill supersedes earlier GPU writes to the range; none may be downloaded over it.
	DiscardGpuWrites(vaddr, size);
	if (!LibKernel::Memory::TryWriteBacking(vaddr, words.data(), size)) {
		EXIT("BufferCache: failed to fill guest memory 0x%016" PRIx64 "\n", vaddr);
	}
	// The next GPU use uploads the new contents.
	m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	return true;
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
#if defined(__ANDROID__)
	// `setprop debug.capy.fault_log 1`: log every CPU access that forces a GPU readback (tag
	// CapyPS5-Readback) with the accessing thread and how long it waited.
	static Common::DebugFlags::CachedValue fault_log("debug.capy.fault_log", 0);
	// `fault_log 2` also logs the GPU thread, with the host frames that touched the memory.
	const bool log_fault = fault_log.Get() == 2 || (fault_log.Get() != 0 && !GuestGpu::IsGpuThread());
	const auto fault_start = log_fault ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
#endif
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		if (!m_scheduler.Active()) {
			// Nothing can be downloaded without a recording scheduler (startup/shutdown); drop the
			// protection so the faulting access can proceed.
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
			return;
		}
		// Always download every GPU-written range on the faulting pages before clearing GPU
		// ownership. This code used to skip the download when the faulting byte itself was
		// clean and then clear the whole page, discarding compute results that shared it, so
		// sprite batches rendered from stale CPU data. Clearing after an empty download (unlike
		// upstream) avoids an endless fault loop on pages with no dirty bytes left.
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		// UE4 keeps small GPU-written buffers on pages full of CPU data; waiting for the GPU on
		// every access to those pages serialized the CPU and the GPU. An access at least
		// ReadGuard bytes away from every GPU-written byte proceeds at once, and those bytes reach
		// guest memory when the GPU finishes. `setprop debug.capy.async_readback 0` disables it.
		// Writes always wait: the page stays writable afterwards, and the late download overwrote
		// whatever the CPU had written there since (Little Nightmares copies shader code into such
		// pages, which then decoded as code for 0x100 bytes and vertex data after).
		static Common::DebugFlags::CachedValue async_readback("debug.capy.async_readback", 1);
		constexpr uint64_t ReadGuard = 64;
		const uint64_t     guard_low = vaddr >= ReadGuard ? vaddr - ReadGuard : 0;
		if (async_readback.Get() != 0 && !is_write &&
		    !m_gpu_modified_ranges.Intersects(guard_low, size + 2 * ReadGuard)) {
			const auto page_low  = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
			const auto page_high = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
			{
				std::scoped_lock lock {m_async_download_lock};
				m_gpu_modified_ranges.ForEachInRange(page_low, page_high - page_low,
				                                     [&](uint64_t start, uint64_t end) {
					                                     m_async_download_ranges.Add(start, end - start);
				                                     });
			}
			(void)DownloadBufferMemory<true>(buffer, vaddr, size);
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
			if (is_write) {
				m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			}
			return;
		}
		Common::DebugFlags::FrameTrace("  cpu fault download 0x%llx size=%llu write=%d buffer=0x%llx+%llu",
		    static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size), is_write ? 1 : 0,
		    static_cast<unsigned long long>(buffer.CpuAddress()), static_cast<unsigned long long>(buffer.Size()));
		// Stray's render thread reads GPU-written buffers page after page, and each page fault
		// waited for the GPU on its own. Download the GPU-written pages that follow in the same
		// buffer too, so one wait serves the whole sequential read.
		// `setprop debug.capy.read_ahead_kb N` sets the window (0 downloads the faulting pages only).
		static Common::DebugFlags::CachedValue read_ahead_kb("debug.capy.read_ahead_kb", 256);
		const uint64_t read_ahead   = static_cast<uint64_t>(std::max<int64_t>(read_ahead_kb.Get(), 0)) * 1024u;
		const uint64_t buffer_end   = buffer.CpuAddress() + buffer.Size();
		const uint64_t download_low = std::max(Common::AlignDown(vaddr, TRACKER_PAGE_SIZE), buffer.CpuAddress());
		const uint64_t download_end =
		    std::max(std::min(Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE) + read_ahead, buffer_end), vaddr + size);
		const uint64_t download_size = read_ahead != 0 ? download_end - download_low : size;
		const uint64_t download_addr = read_ahead != 0 ? download_low : vaddr;
		(void)DownloadBufferMemory<false>(buffer, download_addr, download_size);
		m_memory_tracker.UnmarkRegionAsGpuModified(download_addr, download_size);
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
#if defined(__ANDROID__)
	if (log_fault) {
		char name[32] = {};
		pthread_getname_np(pthread_self(), name, sizeof(name));
		const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
		                        std::chrono::steady_clock::now() - fault_start)
		                        .count();
		std::string frames;
		if (fault_log.Get() == 2) {
			struct Trace {
				uintptr_t frames[16];
				int       count;
			} trace {{}, 0};
			_Unwind_Backtrace(
			    [](_Unwind_Context* context, void* arg) {
				    auto* t = static_cast<Trace*>(arg);
				    if (t->count >= 16) {
					    return _URC_END_OF_STACK;
				    }
				    t->frames[t->count++] = _Unwind_GetIP(context);
				    return _URC_NO_REASON;
			    },
			    &trace);
			for (int i = 0; i < trace.count; i++) {
				Dl_info info {};
				const auto base = dladdr(reinterpret_cast<void*>(trace.frames[i]), &info) != 0
				                      ? reinterpret_cast<uintptr_t>(info.dli_fbase)
				                      : 0;
				char frame[24];
				std::snprintf(frame, sizeof(frame), " +0x%llx",
				              static_cast<unsigned long long>(trace.frames[i] - base));
				frames += frame;
			}
		}
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Readback",
		                    "%s %s 0x%llx size=%llu waited %lld us%s", name, is_write ? "write" : "read",
		                    static_cast<unsigned long long>(vaddr),
		                    static_cast<unsigned long long>(size), static_cast<long long>(waited),
		                    frames.c_str());
	}
#endif
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		LOGF("BufferCache: invalid buffer discovery request vaddr=0x%016" PRIx64 " size=%" PRIu64 ", returning NULL_BUFFER_ID\n", vaddr, size);
		return NULL_BUFFER_ID;
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	RangeSet                    pending;
	{
		// Copied so the lock is not held while staging memory may wait for the GPU.
		std::scoped_lock lock {m_async_download_lock};
		pending = m_async_download_ranges;
	}
	const auto add_copy = [&](uint64_t address, uint64_t bytes) {
		copies.emplace_back(total_size, buffer.Offset(address), bytes);
		total_size += bytes;
	};
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    // The GPU holds the current value of bytes still being downloaded.
		    uint64_t cursor = address;
		    pending.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    if (start > cursor) {
				    add_copy(cursor, start - cursor);
			    }
			    cursor = std::max(cursor, end);
		    });
		    if (address + bytes > cursor) {
			    add_copy(cursor, address + bytes - cursor);
		    }
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			if (!Libs::LibKernel::Memory::ReadGuestMemory(address, mapped + copy.srcOffset, copy.size)) {
				std::memset(mapped + copy.srcOffset, 0, copy.size);
			}
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		if (!Libs::LibKernel::Memory::ReadGuestMemory(address, temporary->Mapped().data() + copy.srcOffset, copy.size)) {
			std::memset(temporary->Mapped().data() + copy.srcOffset, 0, copy.size);
		}
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
#if defined(__ANDROID__)
	// `setprop debug.capy.watch_gpu_write <vaddr>`: log every GPU write binding that covers that
	// address (tag CapyPS5-GpuWrite) with the caller, to find what makes CPU reads stall.
	static Common::DebugFlags::CachedValue watch_gpu_write("debug.capy.watch_gpu_write", 0);
	const auto watch_page = static_cast<uint64_t>(watch_gpu_write.Get()) & ~uint64_t {0xfff};
	if (is_written && watch_page != 0 && vaddr < watch_page + 0x1000 && watch_page < vaddr + size) {
		Dl_info info {};
		const auto* caller = __builtin_return_address(0);
		const auto  base   = dladdr(caller, &info) != 0 ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-GpuWrite", "0x%llx size=%llu texel=%d caller=+0x%llx",
		                    static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size),
		                    is_texel_buffer ? 1 : 0,
		                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(caller) - base));
	}
#endif
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		static std::atomic<uint32_t> invalid_buf_log{0};
		if (invalid_buf_log.fetch_add(1) < 16) {
			LOGF("BufferCache: invalid buffer request (cmd_invalid=%d, vaddr=0x%016" PRIx64 ", size=%" PRIu64 "), returning null buffer\n",
			     command.IsInvalid(), vaddr, size);
		}
		Common::Telemetry::g_null_buffer_fallbacks.fetch_add(1, std::memory_order_relaxed);
		return {&m_slot_buffers[NULL_BUFFER_ID], 0};
	}

	if (is_written) {
		Common::DebugFlags::WatchWrite("shader-written buffer", vaddr, size, 0);
	}
	// CPU write tracking misses some guest writes on Android (cause still under investigation),
	// so a cached copy goes stale and whole sprite batches render empty. Until that is fixed,
	// treat GPU-read buffers up to 4 MiB as CPU-modified on every use so they are re-uploaded.
	// Visually verified in Dead Cells (prisoner body and props present, 60 FPS).
	// `setprop debug.capy.force_sync_max <bytes>` tunes the limit (0 disables).
#if defined(__ANDROID__)
	static Common::DebugFlags::CachedValue force_sync_max("debug.capy.force_sync_max", 0);
#else
	static Common::DebugFlags::CachedValue force_sync_max("debug.capy.force_sync_max", 0);
#endif
	const bool force_sync = Common::DebugFlags::Enabled(Common::DebugFlags::ForceBufferSync) ||
	                        size <= static_cast<uint64_t>(force_sync_max.Get());
	if (!is_written && force_sync && !m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
		const uint64_t page_addr = vaddr & ~4095ULL;
		m_memory_tracker.MarkRegionAsCpuModified(page_addr, (vaddr + size - page_addr + 4095ULL) & ~4095ULL);
	}
	// `setprop debug.capy.stream_max <bytes>`: copy CPU-owned read-only buffers up to that size
	// fresh from guest memory on every use, bypassing page write tracking.
	static Common::DebugFlags::CachedValue stream_max("debug.capy.stream_max", 0);
	// `debug.capy.stream_lo/stream_hi`: same, but only for buffers starting in [lo, hi) (bisection).
	static Common::DebugFlags::CachedValue stream_lo("debug.capy.stream_lo", 0);
	static Common::DebugFlags::CachedValue stream_hi("debug.capy.stream_hi", 0);
	const bool in_window = static_cast<int64_t>(vaddr) >= stream_lo.Get() &&
	                       static_cast<int64_t>(vaddr) < stream_hi.Get() && size <= 4u * 1024 * 1024;
	// Syncing a cached buffer needs a transfer, which ends the open render pass. On the Adreno the
	// pass that resumes afterwards loses its depth writes (Dead Cells: wall depth vanished every
	// third frame and light shafts leaked through). Inside a pass, serve CPU-modified read-only
	// data fresh from the stream buffer instead; the cached copy syncs later outside a pass.
	// `debug.capy.in_pass_stream 0` disables this.
	static Common::DebugFlags::CachedValue in_pass_stream("debug.capy.in_pass_stream", 1);
	// `debug.capy.in_pass_stream_max <bytes>`: largest buffer served fresh inside a pass.
	static Common::DebugFlags::CachedValue in_pass_stream_max("debug.capy.in_pass_stream_max", 16 * 1024 * 1024);
	const bool in_pass_fresh = in_pass_stream.Get() != 0 && command.IsRendering() &&
	                           !is_texel_buffer && size <= static_cast<uint64_t>(in_pass_stream_max.Get()) &&
	                           m_memory_tracker.IsRegionCpuModified(vaddr, size);
	// Bytes of an asynchronous readback are no longer marked GPU-modified, but they reach guest
	// memory only when the GPU finishes: a copy from guest memory would read the old contents.
	// `setprop debug.capy.fresh_skip_async 0` ignores pending readbacks as before.
	static Common::DebugFlags::CachedValue fresh_skip_async("debug.capy.fresh_skip_async", 1);
	const bool async_pending = fresh_skip_async.Get() != 0 && [&] {
		std::scoped_lock lock {m_async_download_lock};
		return m_async_download_ranges.Intersects(vaddr, size);
	}();
	const bool fresh_copy = !is_written && !async_pending &&
	                        ((stream_max.Get() > 0 && size <= static_cast<uint64_t>(stream_max.Get())) ||
	                         in_window || in_pass_fresh) &&
	                        !m_memory_tracker.IsRegionGpuModified(vaddr, size);
	// Asterix XXL2 reads the same CPU-modified buffers (up to MiBs) in every draw of a pass; a
	// copy per draw filled the stream ring each frame (CPU waits), and when a copy did not fit
	// the fallback upload ended the pass, which on the Adreno loses depth (distant buildings
	// blinked as the sky drew over them). Reusing the pass's copy made the building vanish for
	// good (data changes between draws of a pass), so it is off by default.
	// `setprop debug.capy.pass_fresh_reuse 1` reuses the copy while the ring has not wrapped.
	static Common::DebugFlags::CachedValue pass_fresh_reuse("debug.capy.pass_fresh_reuse", 0);
	if (in_pass_fresh && fresh_copy && pass_fresh_reuse.Get() != 0) {
		if (m_pass_fresh_serial != CommandBuffer::PassSerial() ||
		    m_pass_fresh_wraps != m_stream_buffer.Wraps()) {
			m_pass_fresh.clear();
			m_pass_fresh_serial = CommandBuffer::PassSerial();
			m_pass_fresh_wraps  = m_stream_buffer.Wraps();
		}
		if (const auto it = m_pass_fresh.find(vaddr); it != m_pass_fresh.end() && it->second.size >= size) {
			return {&m_stream_buffer, it->second.offset};
		}
	}
	if (fresh_copy || (!is_written && !async_pending && size <= CACHING_PAGESIZE &&
	    !Common::DebugFlags::Enabled(Common::DebugFlags::NoStreamBuffer) &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size))) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			// `debug.capy.split_on_fresh 1`: still serve the fresh copy but end the render pass
			// first (tells the pass split apart from the data it serves).
			static Common::DebugFlags::CachedValue split_on_fresh("debug.capy.split_on_fresh", 0);
			if (in_pass_fresh && split_on_fresh.Get() != 0) {
				command.EndRendering();
			}
			if (!Libs::LibKernel::Memory::ReadGuestMemory(vaddr, mapped, size)) {
				std::memset(mapped, 0, size);
			}
			m_stream_buffer.Commit();
			if (in_pass_fresh && fresh_copy) {
				// A ring wrap may overwrite earlier copies of the pass.
				if (m_pass_fresh_wraps != m_stream_buffer.Wraps()) {
					m_pass_fresh.clear();
					m_pass_fresh_wraps = m_stream_buffer.Wraps();
				}
				m_pass_fresh[vaddr] = {size, offset};
			}
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	// `debug.capy.probe_addr <vaddr>`: log tracker state and content hash of that buffer per use.
	static Common::DebugFlags::CachedValue probe_addr("debug.capy.probe_addr", 0);
	const bool probe = probe_addr.Get() != 0 && static_cast<uint64_t>(probe_addr.Get()) == vaddr;
	uint64_t probe_hash = 0;
	bool     probe_dirty = false;
	if (probe) {
		std::vector<uint64_t> words((size + 7) / 8);
		(void)Libs::LibKernel::Memory::ReadGuestMemory(vaddr, words.data(), size);
		probe_hash = 1469598103934665603ull;
		for (const auto w: words) {
			probe_hash = (probe_hash ^ w) * 1099511628211ull;
		}
		probe_dirty = m_memory_tracker.IsRegionCpuModified(vaddr, size);
	}
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (probe) {
		static uint64_t last_hash = 0;
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Probe",
		    "buf 0x%llx size=%llu id=%u base=0x%llx bsize=%llu dirty=%d hash=%016llx changed=%d",
		    static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size),
		    static_cast<uint32_t>(id.index), static_cast<unsigned long long>(buffer.CpuAddress()),
		    static_cast<unsigned long long>(buffer.Size()), probe_dirty ? 1 : 0,
		    static_cast<unsigned long long>(probe_hash), probe_hash != last_hash ? 1 : 0);
		last_hash = probe_hash;
	}
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		LOGF("BufferCache: invalid image source vaddr=0x%016" PRIx64 " size=%" PRIu64 ", returning null buffer\n", vaddr, size);
		return {&m_slot_buffers[NULL_BUFFER_ID], 0};
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		LOGF("BufferCache: failed to read mapped guest image backing vaddr=0x%016" PRIx64 " size=%" PRIu64 "\n", vaddr, size);
		if (staging != nullptr) {
			std::memset(staging, 0, size);
		} else {
			return {&m_slot_buffers[NULL_BUFFER_ID], 0};
		}
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		LOGF("BufferCache: fill range must be dword aligned vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
		return;
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			LOGF("BufferCache: GDS fill range is out of bounds\n");
			return;
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		LOGF("BufferCache: invalid fill memory address\n");
		return;
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Write through the guest mapping (as upstream does) so write faults invalidate cached
		// buffers and images. The backing store alone misses guest heap memory.
		if (!Libs::LibKernel::Memory::IsHostRangeMapped(vaddr, size)) {
			LOGF("BufferCache: fill of unmapped range vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 " skipped\n", vaddr, size);
			return;
		}
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		LOGF("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
		return;
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		// Copy through the guest mappings (as upstream does) so caches see the write.
		if (!Libs::LibKernel::Memory::IsHostRangeMapped(src_vaddr, size) ||
		    !Libs::LibKernel::Memory::IsHostRangeMapped(dst_vaddr, size)) {
			LOGF("BufferCache: copy with unmapped range src=0x%016" PRIx64 " dst=0x%016" PRIx64
			     " size=0x%016" PRIx64 " skipped\n", src_vaddr, dst_vaddr, size);
			return;
		}
		std::memmove(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr), size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::SyncForRead(uint64_t vaddr, uint64_t size) {
	if (!m_scheduler.Active()) {
		return false;
	}
	bool pending = false;
	if (HasGpuDirtyBytes(vaddr, size)) {
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		pending |= DownloadBufferMemory<false>(buffer, vaddr, size);
		m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	}
	// A fault elsewhere on the page may have queued these bytes for an asynchronous download
	// and made the page readable: wait until they are published.
	if (HasAsyncDownloadIn(vaddr, size)) {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		pending = true;
	}
	return pending;
}

bool BufferCache::HasAsyncDownloadIn(uint64_t vaddr, uint64_t size) {
	std::scoped_lock lock {m_async_download_lock};
	return m_async_download_ranges.Intersects(vaddr, size);
}

void BufferCache::DiscardGpuWrites(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	m_gpu_modified_ranges.Subtract(vaddr, size);
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
