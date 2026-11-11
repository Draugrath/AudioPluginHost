#include "CursorGuard.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstring>
#include <format>

namespace CursorGuard
{
	namespace
	{
		using SetCursor_t = HCURSOR(WINAPI*)(HCURSOR);
		using ShowCursor_t = int(WINAPI*)(BOOL);

		constexpr int              kThreads = 8;
		std::atomic<std::uint32_t> threads[kThreads]{};
		std::atomic<bool>          active{ false };
		std::atomic<bool>          installed{ false };
		std::atomic<int>           forced{ -1 };
		std::atomic<std::uint64_t> ignored{ 0 };
		SetCursor_t                originalSet = nullptr;
		ShowCursor_t               originalShow = nullptr;

		struct Caller
		{
			std::atomic<HMODULE>       module{ nullptr };
			std::atomic<std::uint32_t> thread{ 0 };
			std::atomic<std::uint32_t> setIgnored{ 0 };
			std::atomic<std::uint32_t> setPassed{ 0 };
			std::atomic<std::uint32_t> shows{ 0 };
			std::atomic<std::uint32_t> hides{ 0 };
			std::atomic<int>           lastCount{ 0 };
		};
		constexpr int kCallers = 12;
		Caller        callers[kCallers];

		std::uint32_t     polls = 0, hiddenPolls = 0, noCursorPolls = 0, foregroundPolls = 0;
		std::atomic<bool> wanted{ false };

		std::atomic<bool>          holding{ false };
		std::atomic<std::uint32_t> holdThread{ 0 };
		std::atomic<int>           raised{ 0 };
		std::atomic<int>           heldBack{ 0 };
		std::atomic<int>           believed{ 0 };

		constexpr std::uint32_t    kRestMs = 150;
		std::atomic<std::uint32_t> returned{ 0 };
		ULONGLONG                  offSince = 0;

		bool Protected(std::uint32_t a_thread)
		{
			for (const auto& thread : threads) {
				if (thread.load(std::memory_order_relaxed) == a_thread) {
					return true;
				}
			}
			return false;
		}

		void ReturnFront()
		{
			if (forced.load(std::memory_order_relaxed) >= 0) {
				return;
			}
			const auto now = GetTickCount64();
			if (!offSince) {
				offSince = now;
			}
			const HWND  front = GetForegroundWindow();
			const DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
			if (!frontThread || !Protected(frontThread)) {
				return;
			}
			POINT point{};
			if (!GetCursorPos(&point)) {
				return;
			}
			const HWND under = WindowFromPoint(point);
			DWORD      process = 0;
			const auto underThread = under ? GetWindowThreadProcessId(under, &process) : 0;
			GUITHREADINFO gui{ sizeof(gui) };
			const bool    busy = GetGUIThreadInfo(frontThread, &gui) &&
			                  ((gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)) || gui.hwndCapture || gui.hwndCaret);
			const bool button = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
			if (!ShouldReturn(true, process == GetCurrentProcessId(), Protected(underThread), busy, button, static_cast<std::uint32_t>(now - offSince))) {
				return;
			}
			if (SetForegroundWindow(GetAncestor(under, GA_ROOT))) {
				returned.fetch_add(1, std::memory_order_relaxed);
			}
		}
		Caller* Find(void* a_return)
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_return), &module);
			for (auto& caller : callers) {
				HMODULE seen = caller.module.load(std::memory_order_relaxed);
				if (seen == module) {
					return &caller;
				}
				if (!seen) {
					HMODULE none = nullptr;
					if (caller.module.compare_exchange_strong(none, module) || none == module) {
						return &caller;
					}
				}
			}
			return nullptr;
		}

		HCURSOR WINAPI GuardedSet(HCURSOR a_cursor)
		{
			if (!active.load(std::memory_order_relaxed)) {
				return originalSet(a_cursor);
			}
			const auto thread = GetCurrentThreadId();
			const bool over = OverProtected();
			const bool ignore = over && !Protected(thread);
			if (over) {
				if (auto* caller = Find(_ReturnAddress())) {
					caller->thread.store(thread, std::memory_order_relaxed);
					(ignore ? caller->setIgnored : caller->setPassed).fetch_add(1, std::memory_order_relaxed);
				}
			}
			if (ignore) {
				ignored.fetch_add(1, std::memory_order_relaxed);
				return GetCursor();
			}
			return originalSet(a_cursor);
		}

		int WINAPI GuardedShow(BOOL a_show)
		{
			const auto thread = GetCurrentThreadId();
			int        count = 0;
			if (holding.load(std::memory_order_relaxed) && holdThread.load(std::memory_order_relaxed) == thread) {
				if (!a_show) {
					heldBack.fetch_add(1, std::memory_order_relaxed);
					count = believed.fetch_sub(1, std::memory_order_relaxed) - 1;
				} else {
					count = believed.fetch_add(1, std::memory_order_relaxed) + 1;
					if (heldBack.load(std::memory_order_relaxed) > 0) {
						heldBack.fetch_sub(1, std::memory_order_relaxed);
					} else {
						originalShow(TRUE);
					}
				}
			} else {
				count = originalShow(a_show);
			}
			if (active.load(std::memory_order_relaxed) && wanted.load(std::memory_order_relaxed)) {
				if (auto* caller = Find(_ReturnAddress())) {
					caller->thread.store(thread, std::memory_order_relaxed);
					(a_show ? caller->shows : caller->hides).fetch_add(1, std::memory_order_relaxed);
					caller->lastCount.store(count, std::memory_order_relaxed);
				}
			}
			return count;
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

		std::string Name(HMODULE a_module)
		{
			char path[MAX_PATH]{};
			if (!a_module || !GetModuleFileNameA(a_module, path, MAX_PATH)) {
				return "unknown";
			}
			const char* slash = std::strrchr(path, '\\');
			return slash ? slash + 1 : path;
		}
	}

	std::size_t Install()
	{
		if (installed.exchange(true)) {
			return 0;
		}
		HMODULE user = GetModuleHandleW(L"user32.dll");
		originalSet = user ? reinterpret_cast<SetCursor_t>(GetProcAddress(user, "SetCursor")) : nullptr;
		originalShow = user ? reinterpret_cast<ShowCursor_t>(GetProcAddress(user, "ShowCursor")) : nullptr;
		if (!originalSet || !originalShow) {
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
				count += Redirect(entry.hModule, reinterpret_cast<void*>(originalSet), reinterpret_cast<void*>(&GuardedSet));
				count += Redirect(entry.hModule, reinterpret_cast<void*>(originalShow), reinterpret_cast<void*>(&GuardedShow));
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

	int Tick()
	{
		if (!originalShow || !installed.load(std::memory_order_relaxed)) {
			return 0;
		}
		const bool over = active.load(std::memory_order_relaxed) && OverProtected();
		if (over) {
			CURSORINFO info{ sizeof(info) };
			if (GetCursorInfo(&info)) {
				++polls;
				const HWND front = GetForegroundWindow();
				foregroundPolls += front && Protected(GetWindowThreadProcessId(front, nullptr));
				noCursorPolls += info.hCursor == nullptr;
				hiddenPolls += !(info.flags & CURSOR_SHOWING);
			}
		}
		if (over || !active.load(std::memory_order_relaxed)) {
			offSince = 0;
		} else {
			ReturnFront();
		}
		const bool was = wanted.exchange(over);
		return over == was ? 0 : over ? 1 : -1;
	}

	void ShowOnThisThread(bool a_show)
	{
		if (!originalShow) {
			return;
		}
		const auto thread = GetCurrentThreadId();
		if (a_show) {
			if (holding.load()) {
				return;
			}
			int count = originalShow(TRUE);
			int calls = 1;
			while (count < 0 && calls < 64) {
				count = originalShow(TRUE);
				++calls;
			}
			raised = calls;
			heldBack = 0;
			believed = count - calls;
			holdThread = thread;
			holding = true;
		} else {
			if (!holding.load() || holdThread.load() != thread) {
				return;
			}
			holding = false;
			for (int i = raised.exchange(0) + heldBack.exchange(0); i > 0; --i) {
				originalShow(FALSE);
			}
		}
	}

	int RealShowCount()
	{
		if (!originalShow) {
			return 0;
		}
		const int count = originalShow(TRUE) - 1;
		originalShow(FALSE);
		return count;
	}

	std::string Report()
	{
		if (!polls) {
			return {};
		}
		std::string text = std::format("Pointer over a plugin window: {} checks, hidden in {}, no pointer shape in {}, plugin window in front in {}. Game thread {}: {}, raised {}, hides held back {}.",
			polls, hiddenPolls, noCursorPolls, foregroundPolls, holdThread.load(), holding.load() ? "showing the pointer" : "not asked yet", raised.load(), heldBack.load());
		polls = hiddenPolls = noCursorPolls = foregroundPolls = 0;
		for (auto& caller : callers) {
			HMODULE module = caller.module.load(std::memory_order_relaxed);
			if (!module) {
				continue;
			}
			const auto setIgnored = caller.setIgnored.exchange(0), setPassed = caller.setPassed.exchange(0);
			const auto shows = caller.shows.exchange(0), hides = caller.hides.exchange(0);
			if (setIgnored || setPassed || shows || hides) {
				text += std::format(" {} (thread {}): SetCursor ignored {} passed {}, ShowCursor show {} hide {} (count now {}).", Name(module), caller.thread.load(), setIgnored, setPassed, shows,
					hides, caller.lastCount.load());
			}
		}
		return text;
	}

	std::uint64_t Ignored() { return ignored.load(); }
	std::uint32_t Returned() { return returned.load(); }

	bool ShouldReturn(bool a_frontIsPlugin, bool a_underIsOurs, bool a_underIsPlugin, bool a_pluginBusy, bool a_buttonDown, std::uint32_t a_restMs)
	{
		return a_frontIsPlugin && a_underIsOurs && !a_underIsPlugin && !a_pluginBusy && !a_buttonDown && a_restMs >= kRestMs;
	}
	void          ForceOver(int a_value) { forced = a_value; }
}
