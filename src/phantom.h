#pragma once

namespace Phantom
{
	void OnDataLoaded();   // forms + equip event sink
	void OnGameLoaded();   // animation event sink, leftovers cleared
	void Tick(RE::PlayerCharacter* a_pc);   // from PlayerCharacter::Update
}
