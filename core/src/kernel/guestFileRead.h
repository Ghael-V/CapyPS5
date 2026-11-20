// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors (wuboo)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_KERNEL_GUESTFILEREAD_H_
#define EMULATOR_SRC_KERNEL_GUESTFILEREAD_H_

#include "common/file.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Libs::LibKernel::FileSystem {

// Reads up to `want` bytes into a guest buffer from the file, which is positioned at `pos`.
// Returns 0, EFAULT or EIO; *got holds the bytes delivered in every case. A short read at end of
// file returns 0.
//
// A guest buffer may be write-protected by the GPU resource tracker. A large read goes from the
// kernel straight into the buffer and then fails with EFAULT instead of faulting into the
// tracker's handler, so the read came back short without an error. The rest of the read now goes
// through a host bounce buffer: the copy into the guest takes the fault and the tracker unprotects
// the page. A store the handler does not recover would end the emulator, so the copy needs a
// destination the caller knows is `recoverable`; any other destination fails the read with EFAULT,
// and the file offset then covers only the delivered bytes. `buf` is a host address.
inline int ReadIntoGuest(Common::File& f, void* buf, uint32_t want, uint64_t pos, uint32_t* got,
                         bool recoverable) {
	int err = 0;
	*got    = 0;
	f.ReadEx(buf, want, got, &err);
	if (*got >= want || err == 0) {
		return 0;
	}
	if (err != EFAULT) {
		return EIO;
	}

	constexpr uint32_t   Chunk = 1u << 20u;
	std::vector<uint8_t> bounce;
	uint32_t             done   = *got;
	int                  result = 0;
	if (!f.Seek(pos + done)) {
		return EIO;
	}
	while (done < want) {
		const uint32_t count = std::min<uint32_t>(want - done, Chunk);
		bounce.resize(count);
		uint32_t r  = 0;
		int      e2 = 0;
		f.ReadEx(bounce.data(), count, &r, &e2);
		if (e2 != 0) {
			result = EIO;
			break;
		}
		auto* dst = static_cast<uint8_t*>(buf) + done;
		if (r != 0) {
			if (!recoverable) {
				result = EFAULT;
				break;
			}
			std::memcpy(dst, bounce.data(), r);
		}
		done += r;
		if (r < count) {
			break;
		}
	}
	*got = done;
	if (result != 0) {
		(void)f.Seek(pos + done);
	}
	return result;
}

} // namespace Libs::LibKernel::FileSystem

#endif // EMULATOR_SRC_KERNEL_GUESTFILEREAD_H_
