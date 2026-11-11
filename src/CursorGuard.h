#pragma once

#include <cstddef>
#include <cstdint>

namespace CursorGuard
{
	std::size_t Install();

	void Protect(std::uint32_t a_thread);
	void Unprotect(std::uint32_t a_thread);
	void SetActive(bool a_active);

	bool OverProtected();

	std::uint64_t Ignored();
	void          ForceOver(int a_value);
}
