#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "utils.h"
#include "XorStr.h"
#include <psapi.h>
#include <tlhelp32.h>

HMODULE g_ntdll = nullptr;
static std::vector<std::string> g_used_temps;

void random_temp_name(char* out) {
    static const char charset[] = "0123456789abcdef";
    bool unique = false;
    while (!unique) {
        for (int idx = 0; idx < 8; ++idx) out[idx] = charset[rand() % 16];
        memcpy(out + 8, ".tmp", 5);
        out[13] = 0;
        unique = true;
        for (auto& used : g_used_temps) {
            if (used == out) { unique = false; break; }
        }
    }
    g_used_temps.push_back(std::string(out));
}

std::string find_cover_dll(SIZE_T required_total) {
    auto NtCreate = (PFN_NtCreateSection)GetProcAddress(g_ntdll, xorstr_("NtCreateSection"));
    auto NtMap = (PFN_NtMapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtMapViewOfSection"));
    auto NtUnmap = (PFN_NtUnmapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtUnmapViewOfSection"));
    WIN32_FIND_DATAA fd = {};
    HANDLE hf = FindFirstFileA(xorstr_("C:\\Windows\\System32\\*.dll"), &fd);
    if (hf == INVALID_HANDLE_VALUE) return "";
    std::vector<std::string> exact, fallback;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint64_t fsz = fd.nFileSizeLow | ((uint64_t)fd.nFileSizeHigh << 32);
        if (!fsz || fsz > 0xFFFF) continue;
        std::string full = std::string(xorstr_("C:\\Windows\\System32\\")) + fd.cFileName;
        HANDLE hfile = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hfile == INVALID_HANDLE_VALUE) continue;
        HANDLE sec = NULL;
        if (NtCreate(&sec, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hfile) < 0) { CloseHandle(hfile); continue; }
        CloseHandle(hfile);
        PVOID base = NULL; SIZE_T vsz = 0;
        if (NtMap(sec, GetCurrentProcess(), &base, 0, 0, NULL, &vsz, 2, 0, PAGE_READONLY) < 0) { CloseHandle(sec); continue; }
        CloseHandle(sec);
        NtUnmap(GetCurrentProcess(), base);
        if (vsz == required_total) exact.push_back(full);
        else if (vsz == 0x10000) fallback.push_back(full);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    auto& pool = exact.empty() ? fallback : exact;
    if (pool.empty()) return "";
    return pool[rand() % pool.size()];
}

std::map<int, SectionInfo> reserve_sections(HANDLE proc, const std::string& cover, SIZE_T needed) {
    std::map<int, SectionInfo> sections;
    char tmp_dir[MAX_PATH] = {}, name_buf[16] = {};

    GetTempPathA(MAX_PATH, tmp_dir);
    random_temp_name(name_buf);
    strcat_s(tmp_dir, name_buf);

    if (!CopyFileA(cover.c_str(), tmp_dir, FALSE)) return sections;

    HANDLE hf = CreateFileA(tmp_dir, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) { DeleteFileA(tmp_dir); return sections; }

    int count = (int)(((needed + 0xFFFF) >> 16));
    SIZE_T total = (SIZE_T)count << 16;

    MEMORY_BASIC_INFORMATION mbi = {}; uint64_t base = 0;
    if (VirtualQueryEx(proc, (LPCVOID)0x10000, &mbi, sizeof(mbi))) {
        for (;;) {
            if (mbi.State == MEM_FREE && mbi.RegionSize >= total) {
                uint64_t aligned = ((uint64_t)mbi.BaseAddress + 0xFFFF) & ~0xFFFFULL;
                if ((PVOID)(aligned + total) <= (char*)mbi.BaseAddress + mbi.RegionSize) { base = aligned; break; }
            }
            if ((char*)mbi.BaseAddress + mbi.RegionSize >= (PVOID)0x7FFFFFFFFFFull) break;
            if (!VirtualQueryEx(proc, (char*)mbi.BaseAddress + mbi.RegionSize, &mbi, sizeof(mbi))) break;
        }
    }
    if (!base) { CloseHandle(hf); DeleteFileA(tmp_dir); return sections; }

    auto NtCreate = (PFN_NtCreateSection)GetProcAddress(g_ntdll, xorstr_("NtCreateSection"));
    auto NtMap = (PFN_NtMapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtMapViewOfSection"));
    auto NtWrite = (PFN_NtWVM)GetProcAddress(g_ntdll, xorstr_("NtWriteVirtualMemory"));

    auto NtUnmap = (PFN_NtUnmapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtUnmapViewOfSection"));

    for (int idx = 0; idx < count; ++idx) {
        HANDLE sec = NULL;
        if (NtCreate(&sec, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hf) < 0) {
            for (auto& kv : sections) if (NtUnmap) NtUnmap(proc, kv.second.base);
            sections.clear();
            CloseHandle(hf); DeleteFileA(tmp_dir);
            return sections;
        }
        LPVOID addr = (LPVOID)(base + ((uint64_t)(unsigned)idx << 16)); SIZE_T vsz = 0;
        if (NtMap(sec, proc, &addr, 0, 0, NULL, &vsz, 2, 0, PAGE_EXECUTE_READWRITE) < 0) {
            CloseHandle(sec);
            for (auto& kv : sections) if (NtUnmap) NtUnmap(proc, kv.second.base);
            sections.clear();
            CloseHandle(hf); DeleteFileA(tmp_dir);
            return sections;
        }
        CloseHandle(sec);
        DWORD old = 0; VirtualProtectEx(proc, addr, 0x10000, PAGE_EXECUTE_READWRITE, &old);
        std::vector<uint8_t> zeros(0x10000, 0); SIZE_T w = 0;
        NtWrite(proc, addr, zeros.data(), 0x10000, &w);
        sections[idx] = { idx, addr, 0x10000 };
    }
    CloseHandle(hf); DeleteFileA(tmp_dir);
    return sections;
}

HANDLE find_io_completion(HANDLE proc) {
    auto NtQO = (PFN_NtQO)GetProcAddress(g_ntdll, xorstr_("NtQueryObject"));
    if (!NtQO) return NULL;
    BYTE* buf = (BYTE*)malloc(0x2000); if (!buf) return NULL;
    HANDLE found = NULL;
    ULONG random_start = 1 + (rand() % 17);
    const wchar_t iocp_name[] = L"IoCompletion";
    for (char phase = 0; phase < 2 && found == NULL; ++phase) {
        ULONG low = (phase == 0) ? random_start : 1;
        ULONG high = (phase == 0) ? 0x4000 : random_start;
        for (ULONG hval = low; hval < high && found == NULL; ++hval) {
            HANDLE dup = NULL;
            if (!DuplicateHandle(proc, (HANDLE)(ULONG_PTR)hval, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
            memset(buf, 0, 0x2000);
            if (NtQO(dup, 2, buf, 0x2000, NULL) >= 0) {
                PWSTR name = *(PWSTR*)(buf + 8);
                if (name) {
                    bool match = true;
                    for (int k = 0; k < 12; k++) if (name[k] != iocp_name[k]) { match = false; break; }
                    if (match && name[12] == 0) { found = dup; break; }
                }
            }
            if (!found) CloseHandle(dup);
        }
    }
    free(buf); return found;
}

bool execute_via_iocp(HANDLE proc, uint64_t function_addr) {
    HANDLE io = find_io_completion(proc); if (!io) return false;

    PVOID slot = NULL;
    for (int attempt = 0; attempt < 4 && !slot; ++attempt) {
        slot = VirtualAllocEx(proc, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!slot) Sleep(25);
    }
    if (!slot) {
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQueryEx(proc, NULL, &mbi, sizeof(mbi));
        uint8_t scan[4096] = {};
        do {
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x48) {
                SIZE_T offset = 0;
                while (offset <= mbi.RegionSize - 72 && !slot) {
                    SIZE_T read_size = mbi.RegionSize - offset - 71; if (read_size > 4096) read_size = 4096;
                    SIZE_T nr = 0; if (!ReadProcessMemory(proc, (BYTE*)mbi.BaseAddress + offset, scan, read_size, &nr)) break;
                    for (SIZE_T j = 0; j + 72 <= nr; j++) {
                        bool zero = true;
                        for (SIZE_T k = 0; k < 72; k++) if (scan[j + k]) { zero = false; break; }
                        if (zero) { slot = (BYTE*)mbi.BaseAddress + offset + j; break; }
                    }
                    offset += 4096;
                }
            }
            if (slot) break;
        } while (VirtualQueryEx(proc, (char*)mbi.BaseAddress + mbi.RegionSize, &mbi, sizeof(mbi)));
    }

    if (!slot) { CloseHandle(io); return false; }
    uint8_t work[0x48] = {}; *(uint64_t*)(work + 0x38) = function_addr;
    WriteProcessMemory(proc, slot, work, 0x48, NULL);

    auto ZwSetIo = (PFN_ZwSetIo)GetProcAddress(g_ntdll, xorstr_("ZwSetIoCompletion"));
    if (!ZwSetIo) { CloseHandle(io); return false; }
    NTSTATUS st = ZwSetIo(io, slot, NULL, 0, 0); CloseHandle(io);
    return st >= 0;
}

DWORD getpid() {
    DWORD pids[2048], needed = 0;
    if (!EnumProcesses(pids, sizeof(pids), &needed)) return 0;
    DWORD proc_count = needed / sizeof(DWORD);
    for (DWORD scan_idx = 0; scan_idx < proc_count; ++scan_idx) {
        HANDLE proc_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[scan_idx]);
        if (proc_handle == NULL) continue;
        char image_name[MAX_PATH] = {}; DWORD name_sz = sizeof(image_name);
        QueryFullProcessImageNameA(proc_handle, 0, image_name, &name_sz);
        CloseHandle(proc_handle);
        if (strstr(image_name, xorstr_("RobloxPlayerBeta.exe")) != NULL) return pids[scan_idx];
    }
    return 0;
}

void apply_image_protections(HANDLE proc, uint64_t base, const IMAGE_NT_HEADERS64* nt, int block_count) {
    for (int i = 0; i < block_count; ++i) {
        uint64_t block_start = base + ((uint64_t)i << 16);
        uint64_t block_end = block_start + 0x10000;
        bool exec = false, write = false;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        IMAGE_SECTION_HEADER* sec_end = sec + nt->FileHeader.NumberOfSections;
        for (; sec < sec_end; ++sec) {
            uint64_t sec_start = base + sec->VirtualAddress;
            uint64_t sec_size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            uint64_t sec_end_va = sec_start + sec_size;
            if (sec_start < block_end && sec_end_va > block_start) {
                if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) exec = true;
                if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) write = true;
            }
        }
        DWORD prot = (exec && write) ? PAGE_EXECUTE_READWRITE
                  : exec               ? PAGE_EXECUTE_READ
                  : write              ? PAGE_READWRITE
                                       : PAGE_READONLY;
        DWORD old = 0;
        VirtualProtectEx(proc, (void*)block_start, 0x10000, prot, &old);
    }
}
