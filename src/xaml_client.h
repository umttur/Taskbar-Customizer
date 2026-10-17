#pragma once
#include <windows.h>
#include <string>
bool InitializeXamlControl();
void RequestXamlAttach(DWORD explorerPid);
void PollXaml(bool enabled, std::wstring& status);
void StopXaml();
void SetXamlBlur(int percent);
