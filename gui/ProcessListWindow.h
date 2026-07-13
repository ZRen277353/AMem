#pragma once

#include "Window.h"
#include <vector>
#include <string>

namespace Mem {
class IMemService;
}

class ProcessListWindow : public Window {
public:
	explicit ProcessListWindow(Mem::IMemService& memService);
	void onDraw() override;
	unsigned int getWindowFlags() const override;

private:
	Mem::IMemService& memService_;
	bool hasData = false;
	std::vector<std::pair<int, std::string>> processes;
};
