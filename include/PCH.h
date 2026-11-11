#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>



using namespace std::literals;
namespace logger = SKSE::log;
