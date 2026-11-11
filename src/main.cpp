// PortableOneRoomMag: magazines for the Portable One Room guns (Colt Single Action Army 6 rounds, Colt New Agent 8 rounds, K2 30 rounds).
// Replaces the old Papyrus scripts (PORGun on the weapon + PORMagWatch effect). Those were attached to the
// weapon itself, and a weapon taken out of a container sometimes never woke its script up, so no magazine.
// Here everything is watched from the player side: whatever gun the player holds, wherever it came from.
//
// counting : TESContainerChangedEvent - the equipped ammo leaving the player with no reference = a shot
//            (the same rule the Papyrus watcher used: drops make a reference, menus are ignored)
// reload   : after <size> shots, unequip + re-equip the ammo on the main thread -> the gun's reload animation
// reset    : the player equipping the ammo (manual reload) or re-equipping the gun refills the magazine
// action   : on every counted shot the gun's own parts move (nodes in the gun meshes, both 1st and 3rd person):
//            Colt SAA - hammer falls and re-cocks while the cylinder turns one chamber; Colt New Agent - slide cycles and
//            the hammer falls / is pushed back. Driven from PlayerCharacter::Update (vtable 0xAD).

namespace
{
	constexpr std::string_view kPlugin = "Portable One Room.esp";

	struct Gun
	{
		RE::FormID    local;
		std::int32_t  size;
		const char*   name;
		int           mech;  // 0 none, 1 revolver, 2 pistol with slide
		RE::FormID    id{ 0 };
		std::int32_t  fired{ 0 };
	};

	Gun g_guns[] = {
		{ 0xA90, 6, "SAA", 1 },
		{ 0xAA0, 8, "NewAgent", 2 },
		{ 0xA50, 30, "K2", 0 },
	};

	namespace Mech
	{
		float g_t = -1.0f;  // seconds since the last shot, <0 = idle
		int   g_kind = 0;

		float Ease(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

		RE::NiMatrix3 Rot(int a_axis, float a_deg)
		{
			const float r = a_deg * 0.01745329252f, c = std::cos(r), s = std::sin(r);
			RE::NiMatrix3 m;
			m.entry[0][0] = 1; m.entry[0][1] = 0; m.entry[0][2] = 0;
			m.entry[1][0] = 0; m.entry[1][1] = 1; m.entry[1][2] = 0;
			m.entry[2][0] = 0; m.entry[2][1] = 0; m.entry[2][2] = 1;
			if (a_axis == 0) {  // gun side axis: hammer
				m.entry[1][1] = c; m.entry[1][2] = -s; m.entry[2][1] = s; m.entry[2][2] = c;
			} else {            // bore axis: cylinder
				m.entry[0][0] = c; m.entry[0][2] = s; m.entry[2][0] = -s; m.entry[2][2] = c;
			}
			return m;
		}

		// hammer angle (deg, + = falls forward), cylinder angle (deg), slide offset (units, - = back)
		void Pose(float t, float& a_hammer, float& a_cyl, float& a_slide)
		{
			a_hammer = a_cyl = a_slide = 0.0f;
			if (t < 0.0f) {
				return;
			}
			if (g_kind == 1) {
				constexpr float H = 22.0f;
				a_hammer = t < 0.03f ? H * t / 0.03f : t < 0.12f ? H : H * (1.0f - Ease((t - 0.12f) / 0.28f));
				a_cyl = t < 0.40f ? 60.0f * Ease((t - 0.12f) / 0.28f) : 0.0f;  // 6-fold symmetric: 60 deg == 0
			} else if (g_kind == 2) {
				constexpr float H = 50.0f, L = 1.9f;
				a_hammer = t < 0.012f ? H * t / 0.012f : H * (1.0f - Ease((t - 0.012f) / 0.033f));
				a_slide = t < 0.03f ? -L * t / 0.03f : -L * (1.0f - Ease((t - 0.03f) / 0.07f));
			}
		}

		void Apply(RE::NiAVObject* a_root, float t)
		{
			static const RE::BSFixedString hammer{ "PORHammer" }, cyl{ "PORCylinder" }, slide{ "PORSlide" };
			float h, c, sl;
			Pose(t, h, c, sl);
			RE::NiUpdateData ud{};
			if (auto n = a_root->GetObjectByName(hammer)) {
				n->local.rotate = Rot(0, h);
				n->Update(ud);
			}
			if (auto n = a_root->GetObjectByName(cyl)) {
				n->local.rotate = Rot(1, c);
				n->Update(ud);
			}
			if (auto n = a_root->GetObjectByName(slide)) {
				n->local.translate = RE::NiPoint3(0.0f, sl, 0.0f);
				n->Update(ud);
			}
		}

		void Tick(RE::PlayerCharacter* a_pc, float a_dt)
		{
			if (g_t < 0.0f || !a_pc) {
				return;
			}
			g_t += a_dt;
			const bool done = g_t > 0.5f;
			for (bool fp : { false, true }) {
				if (auto root = a_pc->Get3D(fp)) {
					Apply(root, done ? -1.0f : g_t);
				}
			}
			if (done) {
				g_t = -1.0f;
			}
		}

		void Shot(int a_kind)
		{
			if (a_kind) {
				g_kind = a_kind;
				g_t = 0.0f;
			}
		}

		struct Update
		{
			static void thunk(RE::PlayerCharacter* a_pc, float a_dt)
			{
				func(a_pc, a_dt);
				Tick(a_pc, a_dt);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
			Update::func = vtbl.write_vfunc(0xAD, Update::thunk);
		}
	}

	std::atomic_bool g_reloading{ false };

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
		public RE::BSTEventSink<RE::TESEquipEvent>
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
			Mech::Shot(gun->mech);
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
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .logName = "PortableOneRoomMag", .logPattern = "[%H:%M:%S] %v" });
	SKSE::log::info("PortableOneRoomMag 1.2.3 - runtime {}", a_skse->RuntimeVersion().string());
	Mech::Install();

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
