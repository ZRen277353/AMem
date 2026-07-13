#pragma once

#include "Window.h"
#include <string>

namespace Mem {
class IMemService;
}

class VersionWindow : public Window {
public:
	explicit VersionWindow(Mem::IMemService& memService);
	void onDraw() override;
	unsigned int getWindowFlags() const override;

private:
	Mem::IMemService& memService_;
	bool hasData = false;
	int version = 0;
	std::string versionString;
};
