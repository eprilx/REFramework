#include <optional>
#include <vector>
#include <array>
#include <stdexcept>
#include <unordered_set>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <utility/String.hpp>
#include "Relocate.hpp"

using namespace std;

namespace utility {
    namespace detail {

        BOOL IsBadMemPtr(BOOL write, void* ptr, size_t size) {
            MEMORY_BASIC_INFORMATION mbi;
            BOOL ok;
            DWORD mask;
            BYTE* p = (BYTE*)ptr;
            BYTE* maxp = p + size;
            BYTE* regend = NULL;

            if (size == 0) {
                return FALSE;
            }

            if (p == NULL) {
                return TRUE;
            }

            if (write == FALSE) {
                mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            } else {
                mask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            }

            do {
                if (p == ptr || p == regend) {
                    if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) {
                        return TRUE;
                    } else {
                        regend = ((BYTE*)mbi.BaseAddress + mbi.RegionSize);
                    }
                }

                ok = (mbi.Protect & mask) != 0;

                if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) {
                    ok = FALSE;
                }

                if (!ok) {
                    return TRUE;
                }

                if (maxp <= regend) {
                    return FALSE;
                } else if (maxp > regend) {
                    p = regend;
                }
            } while (p < maxp);

            return FALSE;
        }

        void relocate_pointers(uint8_t* scan_start, uintptr_t old_start, uintptr_t old_end, uintptr_t new_start, int32_t depth, uint32_t skip_length, uint32_t scan_size, std::unordered_set<uint8_t*>& seen) {
            try {
                if (seen.find(scan_start) != seen.end()) {
                    return;
                }

                seen.insert(scan_start);

                if (IsBadMemPtr(false, scan_start, skip_length)) {
                    return;
                }

                spdlog::info("[relocate_pointers] Scanning {:x} for range <{:x}, {:x}> (size {:x})", (uintptr_t)scan_start, old_start, old_end, scan_size);

                for (auto i = 0; i < scan_size; i += skip_length) {
                    if (IsBadMemPtr(false, scan_start + i, sizeof(void*))) {
                        break;
                    }

                    auto& ptr = *(uintptr_t*)(scan_start + i);
                    seen.insert(scan_start + i);

                    if (ptr >= old_start && ptr < old_end) {
                        const auto new_ptr = new_start + (ptr - old_start);
                        spdlog::info("[relocate_pointers] {:x}+{:x}, {:x} -> {:x}", (uintptr_t)scan_start, (uintptr_t)i, (uintptr_t)ptr, (uintptr_t)new_ptr);
                        const auto prev = ptr;
                        ptr = new_ptr;

                        if (depth > 0 && !IsBadMemPtr(false, (void*)prev, sizeof(void*))) {
                            detail::relocate_pointers((uint8_t*)prev, old_start, old_end, new_start, depth - 1, skip_length, 0x1000, seen);
                        }
                    } else if (depth > 0 && !IsBadMemPtr(false, (void*)ptr, sizeof(void*))) {
                        detail::relocate_pointers((uint8_t*)ptr, old_start, old_end, new_start, depth - 1, skip_length, 0x1000, seen);
                    }
                }
            } catch(...) {
                // We reached the end of readable memory.
            }
        }
    }

    void relocate_pointers(uint8_t* scan_start, uintptr_t old_start, uintptr_t old_end, uintptr_t new_start, int32_t depth, uint32_t skip_length, uint32_t scan_size) {
        if (skip_length == 0) {
            throw std::runtime_error("relocate_pointers: skip_length must be greater than 0");
        }

        std::unordered_set<uint8_t*> seen{};
        detail::relocate_pointers(scan_start, old_start, old_end, new_start, depth, skip_length, scan_size, seen);
    }
}
