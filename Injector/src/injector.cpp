#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "injector.h"
#include "utils.h"
#include "shellcode.h"
#include "XorStr.h"
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

static const bool FINALIZE_PROTECTIONS = true;

static void stage(const char* Name) { printf("STAGE %s\n", Name); }
static void fail_stage(const char* Reason) { printf("FAIL %s\n", Reason); }

static bool validate_pe(const uint8_t* data, size_t size, IMAGE_DOS_HEADER*& dos_out, IMAGE_NT_HEADERS64*& nt_out) {
    if (size < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* dos = (IMAGE_DOS_HEADER*)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < 0 || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size) return false;

    auto* nt = (IMAGE_NT_HEADERS64*)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    if (nt->OptionalHeader.SizeOfImage == 0 || nt->OptionalHeader.SizeOfImage > 0x40000000) return false;

    IMAGE_SECTION_HEADER* first = IMAGE_FIRST_SECTION(nt);
    size_t sec_hdr_end = (uint8_t*)(first + nt->FileHeader.NumberOfSections) - data;
    if (sec_hdr_end > size) return false;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER& s = first[i];
        if (s.SizeOfRawData) {
            if ((uint64_t)s.PointerToRawData + s.SizeOfRawData > size) return false;
            if ((uint64_t)s.VirtualAddress + s.SizeOfRawData > nt->OptionalHeader.SizeOfImage) return false;
        }
    }
    dos_out = dos; nt_out = nt;
    return true;
}

static bool is_already_injected(HANDLE proc, const char* dll_path) {
    const char* base = strrchr(dll_path, '\\');
    base = base ? base + 1 : dll_path;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return false;
    DWORD n = needed / sizeof(HMODULE);
    if (n > 1024) n = 1024;
    for (DWORD i = 0; i < n; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(proc, mods[i], name, MAX_PATH) && _stricmp(name, base) == 0)
            return true;
    }
    return false;
}

bool inject_dll(HANDLE proc, const char* dll_path) {
    printf(xorstr_("injecting...\n"));

    if (is_already_injected(proc, dll_path)) {
        fail_stage("already_injected");
        printf(xorstr_("module.dll is already loaded in target — close Roblox and reinject to avoid crashes.\n"));
        return false;
    }

    std::ifstream file(dll_path, std::ios::binary | std::ios::ate);
    if (!file.is_open() || !file.good()) return false;
    std::streamsize file_size = file.tellg(); file.seekg(0, std::ios::beg);
    if (file_size <= 0 || file_size > 0x10000000) return false;
    std::vector<uint8_t> file_data((size_t)file_size, 0);
    if (!file.read((char*)file_data.data(), file_size)) return false;
    file.close();

    IMAGE_DOS_HEADER* dos = nullptr;
    IMAGE_NT_HEADERS64* nt = nullptr;
    if (!validate_pe(file_data.data(), file_data.size(), dos, nt)) {
        fail_stage("bad_pe");
        printf(xorstr_("module.dll failed PE validation — file is corrupt or not x64.\n"));
        return false;
    }

    SIZE_T image_size = nt->OptionalHeader.SizeOfImage;
    int block_count = (int)(((SIZE_T)image_size + 0xFFFF) >> 16);
    SIZE_T required_total = (SIZE_T)block_count << 16;

    std::vector<uint8_t> image_buf;
    try { image_buf.assign(image_size, 0); }
    catch (const std::bad_alloc&) { fail_stage("oom"); return false; }

    std::string cover = find_cover_dll(required_total);
    if (cover.empty()) { fail_stage("cover"); return false; }
    stage("cover");

    auto sections = reserve_sections(proc, cover, image_size);
    if (sections.empty()) { fail_stage("reserve"); return false; }
    stage("reserve");

    auto unmap_sections = [&]() {
        auto NtUnmap = (PFN_NtUnmapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtUnmapViewOfSection"));
        if (!NtUnmap) return;
        for (auto& kv : sections) NtUnmap(proc, kv.second.base);
    };

    uint64_t remote_base = (uint64_t)(uintptr_t)sections.begin()->second.base;

    if (nt->OptionalHeader.SizeOfHeaders > image_size) { fail_stage("bad_hdr"); unmap_sections(); return false; }
    memcpy(image_buf.data(), file_data.data(), nt->OptionalHeader.SizeOfHeaders);
    {
        IMAGE_SECTION_HEADER* section_array = IMAGE_FIRST_SECTION(nt);
        IMAGE_SECTION_HEADER* section_end = section_array + nt->FileHeader.NumberOfSections;
        while (section_array < section_end) {
            if (section_array->SizeOfRawData != 0) {
                void* dest_ptr = image_buf.data() + section_array->VirtualAddress;
                void* src_ptr = file_data.data() + section_array->PointerToRawData;
                memcpy(dest_ptr, src_ptr, section_array->SizeOfRawData);
            }
            ++section_array;
        }
    }

    auto NtWrite = (PFN_NtWVM)GetProcAddress(g_ntdll, xorstr_("NtWriteVirtualMemory"));
    {
        std::map<int, SectionInfo>::iterator seg_iter = sections.begin();
        std::map<int, SectionInfo>::iterator seg_end = sections.end();
        uintptr_t write_addr = remote_base;
        uint8_t* read_ptr = image_buf.data();
        SIZE_T bytes_left = image_size;

        while (seg_iter != seg_end && bytes_left > 0) {
            uintptr_t block_start = (uintptr_t)(seg_iter->second.base);
            uintptr_t block_end = block_start + seg_iter->second.size;
            if (write_addr >= block_start && write_addr < block_end) {
                SIZE_T copy_sz = (block_end > write_addr + bytes_left)
                    ? bytes_left : (SIZE_T)(block_end - write_addr);
                SIZE_T written = 0;
                NTSTATUS w_status = NtWrite(proc, (void*)write_addr, (void*)read_ptr, copy_sz, &written);
                if (w_status < 0) fail_stage("write");
                bytes_left -= copy_sz;
                read_ptr += copy_sz;
                write_addr += copy_sz;
            }
            ++seg_iter;
        }
    }
    stage("write");

    auto NtCreate = (PFN_NtCreateSection)GetProcAddress(g_ntdll, xorstr_("NtCreateSection"));
    auto NtMap = (PFN_NtMapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtMapViewOfSection"));

    struct ScMapping {
        HANDLE section;
        LPVOID view;
        SIZE_T size;
    } sc_mapping = { NULL, NULL, 0x1020 };

    {
        LARGE_INTEGER sz;
        sz.QuadPart = 0x1020;
        NTSTATUS status = NtCreate(&sc_mapping.section, 14, NULL, &sz, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
        if (status < 0) { fail_stage("shellcode"); unmap_sections(); return false; }
        status = NtMap(sc_mapping.section, proc, &sc_mapping.view, 0, 0, NULL, &sc_mapping.size, 2, 0, PAGE_EXECUTE_READWRITE);
        if (status < 0) { fail_stage("shellcode"); CloseHandle(sc_mapping.section); unmap_sections(); return false; }
        CloseHandle(sc_mapping.section);
        sc_mapping.section = NULL;
    }

    uint8_t* shellcode_ptr = (uint8_t*)sc_mapping.view;
    uint8_t* params_ptr = shellcode_ptr + 0x1000;
    ShellcodeParams sc_params = {};

    SIZE_T wrote = 0;
    if (!WriteProcessMemory(proc, shellcode_ptr, MAPPER_SC, sizeof(MAPPER_SC), &wrote) || wrote != sizeof(MAPPER_SC)) {
        fail_stage("shellcode_write"); unmap_sections(); return false;
    }

    sc_params.DllBase = remote_base;
    HMODULE k32 = GetModuleHandleA(xorstr_("kernel32.dll"));
    if (!k32) { fail_stage("k32"); unmap_sections(); return false; }
    sc_params.LoadLibrary = (uint64_t)(uintptr_t)GetProcAddress(k32, xorstr_("LoadLibraryA"));
    sc_params.GetProcAddr = (uint64_t)(uintptr_t)GetProcAddress(k32, xorstr_("GetProcAddress"));
    if (!sc_params.LoadLibrary || !sc_params.GetProcAddr) { fail_stage("k32_proc"); unmap_sections(); return false; }
    if (!WriteProcessMemory(proc, params_ptr, &sc_params, sizeof(sc_params), &wrote) || wrote != sizeof(sc_params)) {
        fail_stage("params_write"); unmap_sections(); return false;
    }
    stage("shellcode");

    struct {
        HANDLE section;
        LPVOID view;
        SIZE_T size;
    } tramp = { NULL, NULL, 0x100 };
    bool tramp_is_alloc = false;

    {
        LARGE_INTEGER sz;
        sz.QuadPart = 0x100;
        NTSTATUS status = NtCreate(&tramp.section, 14, NULL, &sz, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
        if (status >= 0) {
            status = NtMap(tramp.section, proc, &tramp.view, 0, 0, NULL, &tramp.size, 2, 0, PAGE_EXECUTE_READWRITE);
            CloseHandle(tramp.section);
        }
    }

    if (tramp.view == NULL) {
        tramp.view = VirtualAllocEx(proc, NULL, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (tramp.view == NULL) { fail_stage("tramp"); unmap_sections(); return false; }
        tramp_is_alloc = true;
    }
    stage("trampoline");

    uint8_t jmp_stub[22];
    jmp_stub[0] = 0x48; jmp_stub[1] = 0xB8;
    *(uint64_t*)(jmp_stub + 2) = (uintptr_t)shellcode_ptr;
    jmp_stub[10] = 0x48; jmp_stub[11] = 0xB9;
    *(uint64_t*)(jmp_stub + 12) = (uintptr_t)params_ptr;
    jmp_stub[20] = 0xFF; jmp_stub[21] = 0xE0;
    WriteProcessMemory(proc, tramp.view, jmp_stub, 22, NULL);

    if (!execute_via_iocp(proc, (uintptr_t)tramp.view)) { fail_stage("io"); unmap_sections(); return false; }
    stage("execute");

    {
        ShellcodeParams poll_buf = {};
        DWORD deadline = GetTickCount() + 30000;

        do {
            Sleep(150);
            SIZE_T bytes_copy = 0;
            ReadProcessMemory(proc, params_ptr, &poll_buf, sizeof(poll_buf), &bytes_copy);
            if (poll_buf.Stage == 5) {
                Sleep(100);

                auto NtUnmap = (PFN_NtUnmapViewOfSection)GetProcAddress(g_ntdll, xorstr_("NtUnmapViewOfSection"));

                {
                    std::vector<uint8_t> hdr_z(nt->OptionalHeader.SizeOfHeaders, 0);
                    SIZE_T wz = 0;
                    NtWrite(proc, (void*)remote_base, hdr_z.data(), hdr_z.size(), &wz);
                }

                if (NtUnmap) {
                    NtUnmap(proc, sc_mapping.view);
                    if (!tramp_is_alloc) NtUnmap(proc, tramp.view);
                }
                if (tramp_is_alloc) VirtualFreeEx(proc, tramp.view, 0, MEM_RELEASE);

                struct PostInitParams {
                    uint64_t pdata_va;
                    uint32_t pdata_count;
                    uint32_t _pad;
                    uint64_t dll_base;
                    uint64_t rtladd_fn;
                    uint64_t tls_cbs_va;
                    uint32_t done;
                    uint32_t _pad2[5];
                } pip = {};

                auto& exc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (exc_dir.VirtualAddress && exc_dir.Size) {
                    pip.pdata_va    = remote_base + exc_dir.VirtualAddress;
                    pip.pdata_count = exc_dir.Size / 12;
                }
                pip.dll_base   = remote_base;
                pip.rtladd_fn  = (uint64_t)(uintptr_t)GetProcAddress(g_ntdll, xorstr_("RtlAddFunctionTable"));

                pip.tls_cbs_va = 0;

                struct { HANDLE section; LPVOID view; SIZE_T size; } post2 = { NULL, NULL, 0x1000 };
                {
                    LARGE_INTEGER sz2; sz2.QuadPart = 0x1000;
                    NTSTATUS st2 = NtCreate(&post2.section, 14, NULL, &sz2, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
                    if (st2 >= 0) {
                        NtMap(post2.section, proc, &post2.view, 0, 0, NULL, &post2.size, 2, 0, PAGE_EXECUTE_READWRITE);
                        CloseHandle(post2.section);
                    }
                }

                if (post2.view) {
                    uint8_t* p2_tramp  = (uint8_t*)post2.view;
                    uint8_t* p2_sc     = p2_tramp + 0x40;
                    uint8_t* p2_params = p2_tramp + 0xC0;

                    WriteProcessMemory(proc, p2_sc,     POST_SC, sizeof(POST_SC), NULL);
                    WriteProcessMemory(proc, p2_params, &pip,    sizeof(pip),     NULL);

                    uint8_t jmp2[22];
                    jmp2[0]=0x48; jmp2[1]=0xB8; *(uint64_t*)(jmp2+2)=(uintptr_t)p2_sc;
                    jmp2[10]=0x48; jmp2[11]=0xB9; *(uint64_t*)(jmp2+12)=(uintptr_t)p2_params;
                    jmp2[20]=0xFF; jmp2[21]=0xE0;
                    WriteProcessMemory(proc, p2_tramp, jmp2, 22, NULL);

                    bool queued = execute_via_iocp(proc, (uintptr_t)p2_tramp);

                    PostInitParams pip2 = {};
                    DWORD p2_deadline = GetTickCount() + 15000;
                    bool target_died = false;
                    do {
                        Sleep(50);
                        pip2 = {}; SIZE_T nr2 = 0;
                        ReadProcessMemory(proc, p2_params, &pip2, sizeof(pip2), &nr2);
                        if (pip2.done) break;
                        DWORD ec2 = 0;
                        if (!GetExitCodeProcess(proc, &ec2) || ec2 != STILL_ACTIVE) { target_died = true; break; }
                    } while (GetTickCount() < p2_deadline);

                    if (!pip2.done) {
                        if (target_died) printf(xorstr_("post: target died (module loaded but crashed early)\n"));
                        else if (!queued) printf(xorstr_("post: could not queue IOCP work item\n"));
                        else printf(xorstr_("post: timed out waiting for done flag (15s)\n"));
                        fail_stage("post"); unmap_sections(); return false;
                    }
                    stage("post");

                    Sleep(50);
                    if (NtUnmap) NtUnmap(proc, post2.view);
                }

                if (FINALIZE_PROTECTIONS) {
                    apply_image_protections(proc, (uint64_t)remote_base, nt, (int)sections.size());
                }

                return true;
            }

            DWORD exit_check = 0;
            if (!GetExitCodeProcess(proc, &exit_check) || exit_check != STILL_ACTIVE)
                return false;
        } while (GetTickCount() < deadline);
    }
    fail_stage("timeout");
    unmap_sections();
    return false;
}
