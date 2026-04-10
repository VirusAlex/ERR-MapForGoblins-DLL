#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <filesystem>
#include <windows.h>

namespace tracker
{

using SetEventFlagFn = void(__fastcall)(uint64_t event_man, uint32_t *event_id, bool state);
using IsEventFlagFn = bool(__fastcall)(uint64_t event_man, uint32_t *event_id);

void initialize(const std::filesystem::path &mod_folder);
void hotkey_loop();

}
