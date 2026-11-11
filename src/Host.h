#pragma once

#include "PCH.h"

namespace Host
{
	void Listen();
	void Attach();

	std::string LoadText();
	std::string Status();
	void        SaveSettings();

	void        ChooseFolder();
	void        ClearFolder();
	bool        Choosing();
	bool        PointerOnPlugin();
	std::string Folder();

	bool        StereoAvailable();
	std::string StereoNote();
}
