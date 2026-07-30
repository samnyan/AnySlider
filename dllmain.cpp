// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"

BOOL WINAPI DllMain(HMODULE hModule,
                     DWORD reason,
                     LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_PROCESS_DETACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
    return TRUE;
}

