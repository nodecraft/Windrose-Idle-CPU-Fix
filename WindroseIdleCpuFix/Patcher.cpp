// SPDX-License-Identifier: MIT
//
// Runtime port of shipstuff/windrose-self-hosted's patch-idle-cpu.py.
// Same 9-byte signature, same 38-byte trampoline (push volatiles, shadow
// space, mov ecx,1, call [rip+Sleep_IAT], restore, jmp back). Operates on
// the live-mapped PE of the server exe instead of rewriting the file.
//
// The trampoline saves rdx in particular because the loop top reloads
// %rbx from `lea 0x28(%rdx), %rbx`; a Sleep clobber of rdx would crash.

#include "Patcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <vector>

namespace windrose::patcher {

namespace {

constexpr uint8_t kSignature[] = {0x48, 0x8B, 0x0B, 0x8B, 0xC7, 0x87, 0x41, 0x34, 0xE9};
constexpr uint8_t kTrampolinePrologue[] = {0x52, 0x51, 0x50, 0x41, 0x50, 0x41, 0x51};
constexpr size_t kTrampolineSize = 38;

struct TextSection {
    uint8_t* base;
    size_t size;
};

struct PdataRange {
    uint32_t begin_rva;
    uint32_t end_rva;
};

std::optional<TextSection> find_text_section(HMODULE mod) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return std::nullopt;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text\0\0\0", 8) == 0) {
            size_t size = sec->Misc.VirtualSize;
            if (sec->SizeOfRawData && sec->SizeOfRawData < size) {
                size = sec->SizeOfRawData;
            }
            return TextSection{reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress, size};
        }
    }
    return std::nullopt;
}

std::vector<uint8_t*> scan_signature(const TextSection& text) {
    std::vector<uint8_t*> hits;
    constexpr size_t sig_len = sizeof(kSignature);
    if (text.size < sig_len) return hits;
    uint8_t* end = text.base + text.size - sig_len;
    for (uint8_t* p = text.base; p <= end; ++p) {
        if (std::memcmp(p, kSignature, sig_len) == 0) {
            hits.push_back(p);
            if (hits.size() >= 2) return hits;
        }
    }
    return hits;
}

std::vector<PdataRange> load_pdata_ranges(HMODULE mod) {
    std::vector<PdataRange> ranges;
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.Size == 0 || dir.VirtualAddress == 0) return ranges;

    auto* pdata = reinterpret_cast<RUNTIME_FUNCTION*>(
        reinterpret_cast<uint8_t*>(mod) + dir.VirtualAddress);
    size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
    ranges.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (pdata[i].BeginAddress == 0 && pdata[i].EndAddress == 0) break;
        ranges.push_back({pdata[i].BeginAddress, pdata[i].EndAddress});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const PdataRange& a, const PdataRange& b) {
                  return a.begin_rva < b.begin_rva;
              });
    return ranges;
}

bool rva_covered_by_pdata(const std::vector<PdataRange>& ranges,
                          uint32_t rva, uint32_t size) {
    uint32_t end = rva + size;
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ranges[mid].end_rva <= rva) lo = mid + 1;
        else if (ranges[mid].begin_rva >= end) hi = mid;
        else return true;
    }
    return false;
}

// Largest CC-padding run in .text not overlapping any RUNTIME_FUNCTION. Tie
// on size breaks to the lower offset so layout reshuffles stay deterministic.
uint8_t* find_cc_cave(HMODULE mod, const TextSection& text) {
    auto ranges = load_pdata_ranges(mod);
    uint8_t* best = nullptr;
    size_t best_size = 0;

    uint8_t* p = text.base;
    uint8_t* end = text.base + text.size;
    while (p < end) {
        if (*p != 0xCC) { ++p; continue; }
        uint8_t* run_start = p;
        while (p < end && *p == 0xCC) ++p;
        size_t run_size = static_cast<size_t>(p - run_start);
        if (run_size < kTrampolineSize) continue;
        uint32_t rva = static_cast<uint32_t>(run_start - reinterpret_cast<uint8_t*>(mod));
        if (rva_covered_by_pdata(ranges, rva, static_cast<uint32_t>(kTrampolineSize))) continue;
        if (run_size > best_size || (run_size == best_size && best && run_start < best)) {
            best = run_start;
            best_size = run_size;
        }
    }
    return best;
}

// Locate the IAT slot for kernel32!Sleep. Returned pointer is the live
// address of the 8-byte slot, i.e. the operand of our `call [rip+disp32]`.
uint8_t* find_sleep_iat_slot(HMODULE mod) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.Size == 0 || imp_dir.VirtualAddress == 0) return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll_name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll_name, "kernel32.dll") != 0) continue;

        DWORD thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        auto* ilt = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + thunk_rva);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (size_t i = 0; ilt[i].u1.AddressOfData; ++i) {
            if (ilt[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + ilt[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "Sleep") == 0) {
                return reinterpret_cast<uint8_t*>(&iat[i]);
            }
        }
        return nullptr;
    }
    return nullptr;
}

bool write_bytes(void* dst, const void* src, size_t n) {
    DWORD old_protect = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    std::memcpy(dst, src, n);
    DWORD tmp = 0;
    VirtualProtect(dst, n, old_protect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

std::string hex_addr(const void* p) {
    std::ostringstream os;
    os << "0x" << std::hex << reinterpret_cast<uintptr_t>(p);
    return os.str();
}

} // namespace

Result apply() {
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return {false, "GetModuleHandleW(nullptr) failed"};

    auto text = find_text_section(mod);
    if (!text) return {false, ".text section not found"};

    auto hits = scan_signature(*text);
    if (hits.empty()) {
        return {false, "signature not found; target function was likely refactored"};
    }
    if (hits.size() > 1) {
        return {false, "signature matched multiple sites; refusing to guess"};
    }

    uint8_t* patch_site = hits[0] + 8;
    if (patch_site[0] != 0xE9) {
        return {false, "patch site is not a near JMP (E9)"};
    }

    int32_t site_rel = 0;
    std::memcpy(&site_rel, patch_site + 1, sizeof(site_rel));
    uint8_t* loop_top = patch_site + 5 + site_rel;

    // Re-entry guard: if the site already jumps into our trampoline prologue,
    // we've been here before in this process.
    if (std::memcmp(loop_top, kTrampolinePrologue, sizeof(kTrampolinePrologue)) == 0) {
        return {true, "already patched in this process"};
    }

    uint8_t* trampoline = find_cc_cave(mod, *text);
    if (!trampoline) {
        return {false, "no CC-padding cave of sufficient size in .text (outside .pdata)"};
    }

    uint8_t* sleep_iat_slot = find_sleep_iat_slot(mod);
    if (!sleep_iat_slot) {
        return {false, "kernel32!Sleep IAT slot not found"};
    }

    uint8_t tramp[kTrampolineSize];
    size_t p = 0;
    // push rdx ; push rcx ; push rax ; push r8 ; push r9  (RDX is load-bearing)
    tramp[p++] = 0x52;
    tramp[p++] = 0x51;
    tramp[p++] = 0x50;
    tramp[p++] = 0x41; tramp[p++] = 0x50;
    tramp[p++] = 0x41; tramp[p++] = 0x51;
    // sub rsp, 0x20    ; Win64 shadow space
    tramp[p++] = 0x48; tramp[p++] = 0x83; tramp[p++] = 0xEC; tramp[p++] = 0x20;
    // mov ecx, 1       ; Sleep(1)
    tramp[p++] = 0xB9; tramp[p++] = 0x01; tramp[p++] = 0x00; tramp[p++] = 0x00; tramp[p++] = 0x00;
    // call qword ptr [rip + disp32]  -> IAT slot
    tramp[p++] = 0xFF; tramp[p++] = 0x15;
    {
        uint8_t* next = trampoline + p + 4;
        int64_t rel = sleep_iat_slot - next;
        if (rel > INT32_MAX || rel < INT32_MIN) {
            return {false, "Sleep IAT slot out of rel32 range from trampoline"};
        }
        int32_t rel32 = static_cast<int32_t>(rel);
        std::memcpy(&tramp[p], &rel32, 4);
        p += 4;
    }
    // add rsp, 0x20
    tramp[p++] = 0x48; tramp[p++] = 0x83; tramp[p++] = 0xC4; tramp[p++] = 0x20;
    // pop r9 ; pop r8 ; pop rax ; pop rcx ; pop rdx
    tramp[p++] = 0x41; tramp[p++] = 0x59;
    tramp[p++] = 0x41; tramp[p++] = 0x58;
    tramp[p++] = 0x58;
    tramp[p++] = 0x59;
    tramp[p++] = 0x5A;
    // jmp rel32 -> loop_top
    tramp[p++] = 0xE9;
    {
        uint8_t* next = trampoline + p + 4;
        int64_t rel = loop_top - next;
        if (rel > INT32_MAX || rel < INT32_MIN) {
            return {false, "loop_top out of rel32 range from trampoline"};
        }
        int32_t rel32 = static_cast<int32_t>(rel);
        std::memcpy(&tramp[p], &rel32, 4);
        p += 4;
    }
    if (p != kTrampolineSize) return {false, "trampoline size mismatch"};

    uint8_t site_bytes[5];
    site_bytes[0] = 0xE9;
    {
        uint8_t* next = patch_site + 5;
        int64_t rel = trampoline - next;
        if (rel > INT32_MAX || rel < INT32_MIN) {
            return {false, "trampoline out of rel32 range from patch site"};
        }
        int32_t rel32 = static_cast<int32_t>(rel);
        std::memcpy(&site_bytes[1], &rel32, 4);
    }

    if (!write_bytes(trampoline, tramp, kTrampolineSize)) {
        return {false, "VirtualProtect/write failed for trampoline"};
    }
    if (!write_bytes(patch_site, site_bytes, sizeof(site_bytes))) {
        return {false, "VirtualProtect/write failed for patch site"};
    }

    std::ostringstream msg;
    msg << "patched: site=" << hex_addr(patch_site)
        << " trampoline=" << hex_addr(trampoline)
        << " loop_top=" << hex_addr(loop_top)
        << " sleep_iat=" << hex_addr(sleep_iat_slot);
    return {true, msg.str()};
}

} // namespace windrose::patcher
