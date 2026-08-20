#pragma once

namespace menu {
void Register();        // console commands
void OnInit();          // hooks + per-tick menu upkeep
void Tick(float dt);
}
