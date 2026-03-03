#pragma once

#include "efz_netplay_state.h"

namespace netplay::bridge {
struct NetbridgeStatus;
} // namespace netplay::bridge

namespace netplay::bridge::state_export
{
void Initialize();
void Shutdown();
void Update(const NetbridgeStatus& status);
const EFZNetplayState* GetExportedState();
} // namespace netplay::bridge::state_export
