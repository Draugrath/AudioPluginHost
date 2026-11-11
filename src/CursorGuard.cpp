#include "CursorGuard.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstring>

namespace CursorGuard
{
	namespace
	{
		using SetCursor_t = HCURSOR(WINAPI*)(HCURSOR);

		constexpr int              kThreads = 8;
		std::atomic<std::uint32_t> threads[kThreads]{};
		std::atomic<bool>          active{ false };
		std::atomic<bool>          installed{ false };
		std::atomic<int>           forced{ -1 };
		std::atomic<std::uint64_t> ignored{ 0 };
		SetCursor_t                original = nullptr;

		bool Protected(std::uint32_t a_thread)
		{
			for (const auto& thread : threads) {
				if (thread.load(std::memory_order_relaxed) == a_thread) {
					return true;
				}
			}
			return false;
		}

		HCURSOR WINAPI Guarded(HCURSOR a_cursor)
		{
			if (active.load(std::memory_order_relaxed) && !Protected(GetCurrentThreadId()) && OverProtected()) {
				ignored.fetch_add(1, std::memory_order_relaxed);
				return GetCursor();
			}
			return original(a_cursor);
		}

		std::size_t Redirect(HMODULE a_module, void* a_from, void* a_to)
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return 0;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
				return 0;
			}
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!directory.VirtualAddress || !directory.Size) {
				return 0;
			}
			std::size_t count = 0;
			for (const auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name; ++import) {
				if (!import->FirstThunk) {
					continue;
				}
				for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<std::uint8_t*>(base) + import->FirstThunk); thunk->u1.Function; ++thunk) {
					if (reinterpret_cast<void*>(thunk->u1.Function) != a_from) {
						continue;
					}
					DWORD old = 0;
					if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
						thunk->u1.Function = reinterpret_cast<ULONG_PTR>(a_to);
						VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
						++count;
					}
				}
			}
			return count;
		}
	}

	std::size_t Install()
	{
		if (installed.exchange(true)) {
			return 0;
		}
		HMODULE user = GetModuleHandleW(L"user32.dll");
		original = user ? reinterpret_cast<SetCursor_t>(GetProcAddress(user, "SetCursor")) : nullptr;
		if (!original) {
			return 0;
		}
		std::size_t  count = 0;
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}
		MODULEENTRY32W entry{ sizeof(entry) };
		for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
			if (entry.hModule != user) {
				count += Redirect(entry.hModule, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&Guarded));
			}
		}
		CloseHandle(snapshot);
		return count;
	}

	void Protect(std::uint32_t a_thread)
	{
		if (Protected(a_thread)) {
			return;
		}
		for (auto& thread : threads) {
			std::uint32_t none = 0;
			if (thread.compare_exchange_strong(none, a_thread)) {
				return;
			}
		}
	}

	void Unprotect(std::uint32_t a_thread)
	{
		for (auto& thread : threads) {
			std::uint32_t same = a_thread;
			thread.compare_exchange_strong(same, 0);
		}
	}

	void SetActive(bool a_active) { active = a_active; }

	bool OverProtected()
	{
		const int pretend = forced.load(std::memory_order_relaxed);
		if (pretend >= 0) {
			return pretend != 0;
		}
		POINT point{};
		if (!GetCursorPos(&point)) {
			return false;
		}
		const HWND window = WindowFromPoint(point);
		if (!window) {
			return false;
		}
		DWORD       process = 0;
		const DWORD thread = GetWindowThreadProcessId(window, &process);
		return process == GetCurrentProcessId() && Protected(thread);
	}

	std::uint64_t Ignored() { return ignored.load(); }
	void          ForceOver(int a_value) { forced = a_value; }
}
