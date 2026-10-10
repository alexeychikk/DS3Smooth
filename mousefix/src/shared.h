// Shared between dinput8.cpp (the proxy, mouse fix, icons, stall trace) and dlaa.cpp (DLSS anti-aliasing).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

extern wchar_t g_dllDir[MAX_PATH]; // ends with a backslash

// Appends a line to DS3MouseFix.log next to the DLL (does nothing unless logging is on).
void Log(const char* format, ...);

// Replaces vtable[index] with hook; *original receives the previous entry the first time.
bool PatchVtable(void** vtable, int index, void* hook, void** original);

// ---- dlaa.cpp ----

void DlaaLoadSettings(const wchar_t* ini);
bool DlaaEnabled(); // needs the D3D11 device and IDXGISwapChain::Present hooks
bool DlaaDebug();   // dlaa_dump or dlaa_capture: developer settings that write to the log
void DlaaOnDeviceCreated(ID3D11Device* device);
void DlaaOnPresent(IDXGISwapChain* swapChain); // called before the real Present, on the render thread
void DlaaAfterPresent();                       // called after it
