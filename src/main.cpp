#include <chrono>
#include "phantom.h"
#include <cstdio>
// PortableOneRoomMag: magazines for the Portable One Room guns (K2 30 rounds, Colt New Agent 8, Colt SAA 6).
// Replaces the old Papyrus scripts (PORGun on the weapon + PORMagWatch effect). Those were attached to the
// weapon itself, and a weapon taken out of a container sometimes never woke its script up, so no magazine.
// Here everything is watched from the player side: whatever gun the player holds, wherever it came from.
//
// counting : TESContainerChangedEvent - the equipped ammo leaving the player with no reference = a shot
//            (the same rule the Papyrus watcher used: drops make a reference, menus are ignored)
// reload   : after <size> shots the ammo is taken off at once (no extra attack can slip in), and once the attack is
//            over it is put back on the main thread -> the gun's reload animation; checked 0.35 s later
// reset    : the player equipping the ammo (manual reload) or re-equipping the gun refills the magazine
// action   : on every counted shot the gun's own parts move (nodes in the gun meshes, both 1st and 3rd person):
//            Colt SAA - hammer falls and re-cocks while the cylinder turns one chamber; Colt New Agent - slide cycles
//            and the hammer falls / is pushed back; K2 - bolt carrier + charging handle.
//            Driven from PlayerCharacter::Update (vtable 0xAD).
// view     : per-view position / scale of the gun (PORAdjust node), 1st and 3rd person separately
// phantom  : Phantom Sword (B00) phantom-sword show and combo stage - phantom.cpp

namespace
{
	constexpr std::string_view kPlugin = "Portable One Room.esp";

	struct Gun
	{
		RE::FormID    local;
		std::int32_t  size;
		const char*   name;
		int           mech;  // 0 none, 1 revolver (6 chambers), 2 pistol with slide, 3 rifle bolt
		float         fp[3]{ 0, 0, 0 };  // PORAdjust offset in 1st person view (gun's own axes: x side, y muzzle)
		float         tp[3]{ 0, 0, 0 };  // ... and in 3rd person view
		float         fs{ 1.0f };        // PORAdjust scale, 1st person
		float         ts{ 1.0f };        // ... 3rd person
		RE::FormID    id{ 0 };
		std::int32_t  fired{ 0 };
	};

	void ReloadTick(RE::PlayerCharacter* a_pc, float a_dt);
	void ViewTick(RE::PlayerCharacter* a_pc);

	Gun g_guns[] = {
		{ 0xA50, 30, "K2", 3, {}, { -0.8f, 0.0f, 0.0f } },        // K2: 3rd person a bit to the gun's right
		{ 0xAA0, 8, "M1911", 2, { 0.0f, -1.4f, 0.0f }, { 0.1983f, -0.7519f, 0.3826f }, 1.0f, 0.90f },  // Colt New Agent (WAP ALSID): 1st back 1.4; 3rd 90% about the hand
		{ 0xAB0, 6, "SAA", 1 },                                   // Colt Single Action Army
	};

	Gun* EquippedGun();

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
			if (g_kind == 1) {  // revolver: done by 0.30 s (FNV Lucky: 0.36 s between shots)
				constexpr float H = 30.0f, step = 60.0f;
				a_hammer = t < 0.03f ? H * t / 0.03f : t < 0.08f ? H : H * (1.0f - Ease((t - 0.08f) / 0.22f));
				a_cyl = t < 0.30f ? step * Ease((t - 0.08f) / 0.22f) : 0.0f;  // 6-fold symmetric: one step == 0
			} else if (g_kind == 2) {
				constexpr float H = 50.0f, L = 2.5f;
				a_hammer = t < 0.025f ? H * t / 0.025f : H * (1.0f - Ease((t - 0.025f) / 0.075f));
				a_slide = t < 0.06f ? -L * t / 0.06f : -L * (1.0f - Ease((t - 0.06f) / 0.14f));
			} else if (g_kind == 3) {  // K2: bolt carrier + charging handle, short and fast (fits full-auto)
				constexpr float L = 2.2f;
				a_slide = t < 0.025f ? -L * t / 0.025f : -L * (1.0f - Ease((t - 0.025f) / 0.065f));
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
			if (!a_pc || g_t < 0.0f) {
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
				ReloadTick(a_pc, a_dt);
				ViewTick(a_pc);
				Phantom::Tick(a_pc);
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

	// Magazine empty -> reload. Re-equipping the ammo straight after the last shot broke when the player kept clicking:
	// the next attack slipped in, ran with no bolt (a bare swing) and the queued re-equip got dropped ("no bolts").
	// Now: ammo off at once, wait for the attack to finish (max 1.5 s), put it back in one task, then check 0.35 s
	// later that it is really there (re-equip up to 3x).
	Gun*           g_pend{ nullptr };     // waiting to reload
	float          g_pendT{ 0.0f };
	Gun*           g_check{ nullptr };    // reloaded, verifying
	RE::TESAmmo*   g_checkAmmo{ nullptr };
	float          g_checkT{ 0.0f };
	int            g_checkTries{ 0 };
	RE::TESAmmo*   g_unloaded{ nullptr }; // ammo taken off at the last shot

	// all of the ammo in the inventory (equipping only 1 would leave exactly one round equipped)
	std::int32_t AmmoCount(RE::PlayerCharacter* a_pc, RE::TESAmmo* a_ammo)
	{
		auto inv = a_pc->GetInventoryCounts([a_ammo](RE::TESBoundObject& o) { return &o == a_ammo; });
		auto it = inv.find(a_ammo);
		return it != inv.end() && it->second > 0 ? it->second : 1;
	}

	void EquipAmmo(RE::TESAmmo* a_ammo)
	{
		SKSE::GetTaskInterface()->AddTask([a_ammo]() {
			auto pc = RE::PlayerCharacter::GetSingleton();
			if (pc && !pc->GetCurrentAmmo()) {
				g_reloading = true;
				RE::ActorEquipManager::GetSingleton()->EquipObject(pc, a_ammo, nullptr, AmmoCount(pc, a_ammo), nullptr, false, false, false);
				g_reloading = false;
			}
		});
	}

	void Reload(Gun* a_gun)
	{
		g_pend = a_gun;
		g_pendT = 0.0f;
	}

	void Unload(Gun* a_gun)
	{
		SKSE::GetTaskInterface()->AddTask([a_gun]() {
			auto pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || EquippedGun() != a_gun) {
				return;
			}
			if (auto ammo = pc->GetCurrentAmmo()) {
				g_unloaded = ammo;
				g_reloading = true;
				RE::ActorEquipManager::GetSingleton()->UnequipObject(pc, ammo, nullptr, AmmoCount(pc, ammo), nullptr, false, false, false);
				g_reloading = false;
				SKSE::log::info("{}: magazine empty - ammo off until the reload", a_gun->name);
			}
		});
	}

	void DoReload(Gun* a_gun)
	{
		SKSE::GetTaskInterface()->AddTask([a_gun]() {
			auto pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || EquippedGun() != a_gun) {
				return;
			}
			auto ammo = pc->GetCurrentAmmo();
			const bool unloaded = !ammo && g_unloaded;
			if (unloaded) {
				ammo = g_unloaded;
			}
			g_unloaded = nullptr;
			if (!ammo) {
				SKSE::log::info("{}: reload - no ammo equipped", a_gun->name);
				return;
			}
			auto em = RE::ActorEquipManager::GetSingleton();
			g_reloading = true;
			if (!unloaded) {
				em->UnequipObject(pc, ammo, nullptr, 1, nullptr, false, false, false);
			}
			em->EquipObject(pc, ammo, nullptr, unloaded ? AmmoCount(pc, ammo) : 1, nullptr, false, false, false);
			g_reloading = false;
			g_check = a_gun;
			g_checkAmmo = ammo;
			g_checkT = 0.0f;
			g_checkTries = 0;
			SKSE::log::info("{}: {} shots -> reload", a_gun->name, a_gun->size);
		});
	}

	void ReloadTick(RE::PlayerCharacter* a_pc, float a_dt)
	{
		if (g_pend) {
			if (EquippedGun() != g_pend) {
				g_pend = nullptr;
				if (auto ammo = g_unloaded) {   // gun put away while empty: give the ammo back (no animation needed)
					g_unloaded = nullptr;
					EquipAmmo(ammo);
				}
			} else {
				g_pendT += a_dt;
				const auto st = a_pc->AsActorState()->GetAttackState();
				const bool idle = st == RE::ATTACK_STATE_ENUM::kNone;
				const float limit = g_unloaded ? 1.5f : 0.4f;   // ammo off meanwhile, so waiting is safe
				if ((idle && g_pendT >= 0.05f) || g_pendT >= limit) {
					SKSE::log::info("{}: reload after {:.2f}s ({})", g_pend->name, g_pendT, idle ? "attack done" : "timeout");
					DoReload(g_pend);
					g_pend = nullptr;
				}
			}
		}
		if (g_check) {
			g_checkT += a_dt;
			if (g_checkT >= 0.35f) {
				g_checkT = 0.0f;
				auto gun = g_check;
				auto ammo = g_checkAmmo;
				if (EquippedGun() != gun || !ammo) {
					g_check = nullptr;
				} else if (a_pc->GetCurrentAmmo() == ammo) {
					SKSE::log::info("{}: reload ok, ammo equipped ({} in the inventory)", gun->name, AmmoCount(a_pc, ammo));
					g_check = nullptr;
				} else if (++g_checkTries > 3) {
					SKSE::log::info("{}: ammo still missing after 3 re-equips - giving up", gun->name);
					g_check = nullptr;
				} else {
					SKSE::log::info("{}: ammo missing after reload -> re-equip ({})", gun->name, g_checkTries);
					EquipAmmo(ammo);
				}
			}
		}
	}

	// Per-view placement: the same nif can need a slightly different spot in 1st and 3rd person (different hand
	// animations). The gun nifs have a PORAdjust node; its local translation / scale is set here for each view's 3D.
	void ViewTick(RE::PlayerCharacter* a_pc)
	{
		auto gun = EquippedGun();
		if (!gun || !a_pc) {
			return;
		}
		static const RE::BSFixedString adj{ "PORAdjust" };
		for (bool fp : { false, true }) {
			auto root = a_pc->Get3D(fp);
			if (!root) {
				continue;
			}
			auto n = root->GetObjectByName(adj);
			if (!n) {
				continue;
			}
			const float* o = fp ? gun->fp : gun->tp;
			const float sc = fp ? gun->fs : gun->ts;
			RE::NiPoint3 want{ o[0], o[1], o[2] };
			if (n->local.translate.x != want.x || n->local.translate.y != want.y || n->local.translate.z != want.z || n->local.scale != sc) {
				n->local.translate = want;
				n->local.scale = sc;
				RE::NiUpdateData ud{};
				n->Update(ud);
			}
		}
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
			{
				using clock = std::chrono::steady_clock;
				static clock::time_point last{};
				static Gun* lastGun = nullptr;
				const auto now = clock::now();
				const float dt = lastGun == gun ? std::chrono::duration<float>(now - last).count() : -1.0f;
				last = now;
				lastGun = gun;
				char gap[32];
				if (dt >= 0.0f && dt < 5.0f) {
					std::snprintf(gap, sizeof(gap), "+%.3fs", dt);
				} else {
					std::snprintf(gap, sizeof(gap), "(first)");
				}
				SKSE::log::info("{}: shot {}/{} {}", gun->name, gun->fired, gun->size, gap);
			}
			if (gun->fired >= gun->size) {
				gun->fired = 0;
				Unload(gun);
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
		g_pend = nullptr;
		g_check = nullptr;
		g_unloaded = nullptr;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .logName = "PortableOneRoomMag", .logPattern = "[%H:%M:%S.%e] %v" });
	SKSE::log::info("PortableOneRoomMag 1.9.0 - runtime {}", a_skse->RuntimeVersion().string());
	Mech::Install();

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			Phantom::OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			OnGameLoaded();
			Phantom::OnGameLoaded();
			break;
		default:
			break;
		}
	});
	return true;
}
