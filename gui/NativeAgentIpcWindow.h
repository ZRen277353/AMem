#pragma once

#include "Window.h"

class NativeAgentIpcWindow final : public Window {
public:
  NativeAgentIpcWindow();

protected:
  void onDraw() override;
};
