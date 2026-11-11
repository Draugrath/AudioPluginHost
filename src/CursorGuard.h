#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace CursorGuard
{
	std::size_t Install();

	void Protect(std::uint32_t a_thread);
	void Unprotect(std::uint32_t a_thread);
	void SetActive(bool a_active);

	bool OverProtected();

	int Tick();
	void ShowOnThisThread(bool a_show);
	int  RealShowCount();
	std::string Report();

	std::uint64_t Ignored();
	std::uint32_t Returned();
	bool ShouldReturn(bool a_frontIsPlugin, bool a_underIsOurs, bool a_underIsPlugin, bool a_pluginBusy, bool a_buttonDown, std::uint32_t a_restMs);
	void          ForceOver(int a_value);
}
