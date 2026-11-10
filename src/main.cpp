// PortableOneRoomMag: magazines for the Portable One Room guns (M629 6 rounds, K2 30 rounds).
// Replaces the old Papyrus scripts (PORGun on the weapon + PORMagWatch effect). Those were attached to the
// weapon itself, and a weapon taken out of a container sometimes never woke its script up, so no magazine.
// Here everything is watched from the player side: whatever gun the player holds, wherever it came from.
//
// counting : TESContainerChangedEvent - the equipped ammo leaving the player with no reference = a shot
//            (the same rule the Papyrus watcher used: drops make a reference, menus are ignored)
// reload   : after <size> shots, unequip + re-equip the ammo on the main thread -> the gun's reload animation
// reset    : the player equipping the ammo (manual reload) or re-equipping the gun refills the magazine

namespace
{
	constexpr std::string_view kPlugin = "Portable One Room.esp";

	struct Gun
	{
		RE::FormID    local;
		std::int32_t  size;
		const char*   name;
		RE::FormID    id{ 0 };
		std::int32_t  fired{ 0 };
	};

	Gun g_guns[] = {
		{ 0xA90, 6, "M629" },
		{ 0xA50, 30, "K2" },
	};

	std::atomic_bool g_reloading{ false };
	int              g_animLogs = 0;

	Gun* EquippedGun()
	{
		auto pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return nullptr;
		}
		auto obj = pc->GetEquippedObject(false);
		if (!obj) {
			return nullptr;
		}
		for (auto& g : g_guns) {
			if (g.id != 0 && obj->GetFormID() == g.id) {
				return &g;
			}
		}
		return nullptr;
	}

	void Reload(Gun* a_gun)
	{
		SKSE::GetTaskInterface()->AddTask([a_gun]() {
			auto pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || EquippedGun() != a_gun) {
				return;
			}
			auto ammo = pc->GetCurrentAmmo();
			if (!ammo) {
				return;
			}
			auto em = RE::ActorEquipManager::GetSingleton();
			g_reloading = true;
			em->UnequipObject(pc, ammo, nullptr, 1, nullptr, true, false, false);
			em->EquipObject(pc, ammo, nullptr, 1, nullptr, true, false, false);
			g_reloading = false;
			SKSE::log::info("{}: {} shots -> reload", a_gun->name, a_gun->size);
		});
	}

	class Sink :
		public RE::BSTEventSink<RE::TESContainerChangedEvent>,
		public RE::BSTEventSink<RE::TESEquipEvent>,
		public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		static Sink* Get()
		{
			static Sink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_ev, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
		{
			if (!a_ev || a_ev->oldContainer != 0x14 || a_ev->newContainer != 0 || a_ev->reference) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto gun = EquippedGun();
			auto pc = RE::PlayerCharacter::GetSingleton();
			auto ammo = pc ? pc->GetCurrentAmmo() : nullptr;
			if (!gun || !ammo || a_ev->baseObj != ammo->GetFormID()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			gun->fired += a_ev->itemCount;
			if (gun->fired >= gun->size) {
				gun->fired = 0;
				Reload(gun);
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev, RE::BSTEventSource<RE::TESEquipEvent>*) override
		{
			if (!a_ev || !a_ev->actor || a_ev->actor.get() != RE::PlayerCharacter::GetSingleton() || !a_ev->equipped || g_reloading) {
				return RE::BSEventNotifyControl::kContinue;
			}
			for (auto& g : g_guns) {
				if (a_ev->baseObject == g.id) {
					g.fired = 0;  // gun (re)equipped: full magazine
				}
			}
			auto form = RE::TESForm::LookupByID(a_ev->baseObject);
			if (form && form->Is(RE::FormType::Ammo)) {
				if (auto gun = EquippedGun()) {
					gun->fired = 0;  // ammo equipped by hand = manual reload
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		// diagnostics only: the animation events a gun sends, so the counting can be switched to them if needed
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			if (a_ev && g_animLogs < 200 && EquippedGun()) {
				g_animLogs++;
				SKSE::log::info("anim event: {}", a_ev->tag.c_str());
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnDataLoaded()
	{
		auto dh = RE::TESDataHandler::GetSingleton();
		for (auto& g : g_guns) {
			auto form = dh ? dh->LookupForm(g.local, kPlugin) : nullptr;
			g.id = form ? form->GetFormID() : 0;
			SKSE::log::info("{}: form {:08X} magazine {}", g.name, g.id, g.size);
		}
		auto src = RE::ScriptEventSourceHolder::GetSingleton();
		src->AddEventSink<RE::TESContainerChangedEvent>(Sink::Get());
		src->AddEventSink<RE::TESEquipEvent>(Sink::Get());
	}

	void OnGameLoaded()
	{
		for (auto& g : g_guns) {
			g.fired = 0;
		}
		if (auto pc = RE::PlayerCharacter::GetSingleton()) {
			pc->AddAnimationGraphEventSink(Sink::Get());
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .logName = "PortableOneRoomMag", .logPattern = "[%H:%M:%S] %v" });
	SKSE::log::info("PortableOneRoomMag 1.0.0 - runtime {}", a_skse->RuntimeVersion().string());

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			OnGameLoaded();
			break;
		default:
			break;
		}
	});
	return true;
}
