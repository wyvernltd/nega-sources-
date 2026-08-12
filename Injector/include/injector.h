#pragma once
#include <windows.h>

bool inject_dll(HANDLE proc, const char* dll_path);
