#include <codecvt>
#include <filesystem>
#include <locale>
#include <span>
#include <stdexcept>
#include <string>

#include <MinHook.h>
#include <Pattern16.h>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>

#include "modutils.hpp"

using namespace std;

static span<unsigned char> memory;

void modutils::initialize()
{
    HMODULE module_handle = GetModuleHandleA(nullptr); // main module = the game exe
    if (!module_handle)
    {
        throw runtime_error("Failed to get base module handle");
    }

    wstring_convert<codecvt_utf8_utf16<wchar_t>, wchar_t> convert;

    wchar_t exe_filename[MAX_PATH] = {0};
    GetModuleFileNameW(module_handle, exe_filename, MAX_PATH);
    spdlog::info("Running in: {}", convert.to_bytes(exe_filename));

    MEMORY_BASIC_INFORMATION memory_info;
    if (VirtualQuery((void *)module_handle, &memory_info, sizeof(memory_info)) == 0)
    {
        throw runtime_error("Failed to get virtual memory information");
    }

    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)module_handle;
    IMAGE_NT_HEADERS *nt_headers =
        (IMAGE_NT_HEADERS *)((ULONG64)memory_info.AllocationBase + (ULONG64)dos_header->e_lfanew);

    if ((dos_header->e_magic == IMAGE_DOS_SIGNATURE) &&
        (nt_headers->Signature == IMAGE_NT_SIGNATURE))
    {
        memory = {(unsigned char *)memory_info.AllocationBase,
                  nt_headers->OptionalHeader.SizeOfImage};
    }

    auto mh_status = MH_Initialize();
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("init failed: ") + MH_StatusToString(mh_status));
    }
}

void modutils::deinitialize()
{
    MH_Uninitialize();
}

void *modutils::scan(const ScanArgs &args)
{
    unsigned char *match;
    if (args.address != nullptr)
    {
        match = reinterpret_cast<unsigned char *>(args.address);
    }
    else if (!args.aob.empty())
    {
        match = reinterpret_cast<unsigned char *>(
            Pattern16::scan(&memory.front(), memory.size(), args.aob));
    }
    else
    {
        match = &memory.front();
    }

    if (match != nullptr)
    {
        match += args.offset;

        for (auto [first, second] : args.relative_offsets)
        {
            ptrdiff_t offset = *reinterpret_cast<const int *>(&match[first]) + second;
            match += offset;
        }

        return match;
    }

    return nullptr;
}

void modutils::hook(void *function, void *detour, void **trampoline)
{
    auto mh_status = MH_CreateHook(function, detour, trampoline);
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error creating handler: ") + MH_StatusToString(mh_status));
    }
    mh_status = MH_QueueEnableHook(function);
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error queueing handler: ") + MH_StatusToString(mh_status));
    }
}

void modutils::enable_hooks()
{
    auto mh_status = MH_ApplyQueued();
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error enabling handlers: ") + MH_StatusToString(mh_status));
    }
}
