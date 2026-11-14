#include <chrono>
#include <cstdio>
// PortableOneRoomMag: magazines for the Portable One Room guns (S&W M29 6 rounds, Colt M1911 8 rounds, K2 30 rounds,
// M1 Garand 8-round en-bloc clip: last shot -> clip "ping", operating rod stays back until the reload closes it).
// Replaces the old Papyrus scripts (PORGun on the weapon + PORMagWatch effect). Those were attached to the
// weapon itself, and a weapon taken out of a container sometimes never woke its script up, so no magazine.
// Here everything is watched from the player side: whatever gun the player holds, wherever it came from.
//
// counting : TESContainerChangedEvent - the equipped ammo leaving the player with no reference = a shot
//            (the same rule the Papyrus watcher used: drops make a reference, menus are ignored)
// reload   : after <size> shots, unequip + re-equip the ammo on the main thread -> the gun's reload animation
// reset    : the player equipping the ammo (manual reload) or re-equipping the gun refills the magazine
// action   : on every counted shot the gun's own parts move (nodes in the gun meshes, both 1st and 3rd person):
//            S&W M29 - hammer falls and re-cocks while the cylinder turns one chamber; Colt M1911 - slide cycles and
//            the hammer falls / is pushed back. Driven from PlayerCharacter::Update (vtable 0xAD).

namespace
{
	constexpr std::string_view kPlugin = "Portable One Room.esp";

	struct Gun
	{
		RE::FormID    local;
		std::int32_t  size;
		const char*   name;
		int           mech;  // 0 none, 1 revolver (6), 2 pistol with slide, 3 rifle bolt, 4 revolver (5), 5 Garand op rod (locks back)
		float         fp[3]{ 0, 0, 0 };  // PORAdjust offset in 1st person view (gun's own axes: x side, y muzzle)
		float         tp[3]{ 0, 0, 0 };  // ... and in 3rd person view
		float         fs{ 1.0f };        // PORAdjust scale, 1st person
		float         ts{ 1.0f };        // ... 3rd person
		float         fr[2]{ 0, 0 };     // PORAdjust rotation in 1st person (deg): pitch about x, yaw about z  (R = Rz(yaw) Rx(pitch))
		RE::FormID    id{ 0 };
		std::int32_t  fired{ 0 };
	};

	void ReloadTick(RE::PlayerCharacter* a_pc, float a_dt);
	void ViewTick(RE::PlayerCharacter* a_pc);
	namespace ADS { void Tick(RE::PlayerCharacter* a_pc, float a_dt); void Install(); }

	Gun g_guns[] = {
		{ 0xA90, 6, "M29", 1 },
		{ 0xAA0, 8, "M1911", 2, { 0.0f, -1.4f, 0.0f }, { 0.1983f, -0.7519f, 0.3826f }, 1.0f, 0.90f },  // New Agent (old WAP ALSID): 1st back 1.4; 3rd 90% about the hand
		{ 0xA50, 30, "K2", 3, {}, { -0.8f, 0.0f, 0.0f } },        // K2: 3rd person a bit to the gun's right
		{ 0xAB0, 6, "SAA", 1 },                                   // Colt Single Action Army
		{ 0xAC0, 13, "HiPower", 2 },                              // Browning Hi-Power
		{ 0xAD0, 5, "BFR", 4, {}, { 0.0f, 0.5f, -0.5f } },          // Magnum Research BFR: 5-shot; 3rd person a bit forward and up
		// M1 Garand: 1st person sight line moved onto the DAR Rifles Kar98 / Mosin sight line (same Kar98 animations)
		{ 0xAE0, 8, "Garand", 5, { -1.919f, 0.092f, 1.799f }, {}, 1.0f, 1.0f, { -0.631f, -0.34f } },
	};

	Gun* EquippedGun();
	bool EquippedGunIsGarand();
	struct ClipSnd { static void Play(); static void Cylinder(); };
	float SpeedMult(RE::PlayerCharacter* a_pc)
	{
		auto av = a_pc ? a_pc->AsActorValueOwner() : nullptr;
		const float m = av ? av->GetActorValue(RE::ActorValue::kWeaponSpeedMult) : 0.0f;
		return m > 0.1f ? m : 1.0f;   // 0 = not set = 1x
	}

	namespace Mech
	{
		float g_t = -1.0f;  // seconds since the last shot, <0 = idle
		int   g_kind = 0;
		constexpr float kGarandL = 4.5f;       // op rod / bolt travel
		constexpr float kGarandClose = 1.95f;
		constexpr float kGarandClipSnd = kGarandClose - 0.18f;  // reload: the hand lets the op rod go (Kar98 reload, s after the reload starts)
		bool  g_locked = false;                // Garand: empty, op rod held back
		float g_closeT = -1.0f;                // >=0: reload running, seconds

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
			} else if (a_axis == 2) {  // vertical axis (yaw)
				m.entry[0][0] = c; m.entry[0][1] = -s; m.entry[1][0] = s; m.entry[1][1] = c;
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
			if (g_kind == 1 || g_kind == 4) {
				constexpr float H = 30.0f;
				const float step = g_kind == 4 ? 72.0f : 60.0f;   // 5-shot cylinder turns 72 deg per shot
				// 1.4.6: done by 0.30 s (FNV rates: Lucky 0.36 s, Mysterious Magnum 0.41 s between shots)
				a_hammer = t < 0.03f ? H * t / 0.03f : t < 0.08f ? H : H * (1.0f - Ease((t - 0.08f) / 0.22f));
				a_cyl = t < 0.30f ? step * Ease((t - 0.08f) / 0.22f) : 0.0f;  // n-fold symmetric: one step == 0
			} else if (g_kind == 2) {
				constexpr float H = 50.0f, L = 2.5f;
				a_hammer = t < 0.025f ? H * t / 0.025f : H * (1.0f - Ease((t - 0.025f) / 0.075f));
				a_slide = t < 0.06f ? -L * t / 0.06f : -L * (1.0f - Ease((t - 0.06f) / 0.14f));
			} else if (g_kind == 3) {  // K2: bolt carrier + charging handle, short and fast (fits full-auto)
				constexpr float L = 2.2f;
				a_slide = t < 0.025f ? -L * t / 0.025f : -L * (1.0f - Ease((t - 0.025f) / 0.065f));
			} else if (g_kind == 5) {  // Garand: operating rod + bolt
				a_slide = t < 0.025f ? -kGarandL * t / 0.025f : -kGarandL * (1.0f - Ease((t - 0.025f) / 0.065f));
			}
		}

		void Apply(RE::NiAVObject* a_root, float t)
		{
			static const RE::BSFixedString hammer{ "PORHammer" }, cyl{ "PORCylinder" }, slide{ "PORSlide" };
			float h, c, sl;
			Pose(t, h, c, sl);
			if (g_kind == 5 && g_locked && (t < 0.0f || t >= 0.025f)) {
				sl = -kGarandL;  // held open
				if (g_closeT >= kGarandClose) {
					sl = -kGarandL * (1.0f - Ease((g_closeT - kGarandClose) / 0.06f));  // slams home
				}
			}
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
			if (!a_pc) {
				return;
			}
			if (g_closeT >= 0.0f) {
				const float before = g_closeT;
				g_closeT += a_dt * SpeedMult(a_pc);   // Quick Shot etc. speed the reload animation up: keep the op rod/sound on it
				if (before < kGarandClipSnd && g_closeT >= kGarandClipSnd) {
					ClipSnd::Play();  // its loudest click (0.18 s in) lands on the op rod slam
				}
				if (g_closeT >= kGarandClose + 0.06f) {
					g_closeT = -1.0f;
					if (g_locked) {
						g_locked = false;
						for (bool fp : { false, true }) {
							if (auto root = a_pc->Get3D(fp)) {
								Apply(root, -1.0f);
							}
						}
					}
				}
			}
			if (g_locked && g_t < 0.0f && EquippedGunIsGarand()) {   // keep it open (the 3D can be rebuilt: view switch, redraw)
				for (bool fp : { false, true }) {
					if (auto root = a_pc->Get3D(fp)) {
						Apply(root, -1.0f);
					}
				}
				return;
			}
			if (g_t < 0.0f) {
				return;
			}
			const float before = g_t;
			g_t += a_dt;
			if (g_kind == 4 && before < 0.26f && g_t >= 0.26f) {
				ClipSnd::Cylinder();  // BFR: the cylinder locks into the next chamber (it turns 0.08-0.30 s)
			}
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
				ADS::Tick(a_pc, a_dt);
				ViewTick(a_pc);
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
	RE::BGSSoundDescriptorForm* g_ping{ nullptr };     // Garand en-bloc clip ejection
	RE::BGSSoundDescriptorForm* g_clipIn{ nullptr };
	RE::BGSSoundDescriptorForm* g_bfrCyl{ nullptr };   // BFR cylinder click   // Garand clip in + op rod slam (FO76 'This Machine' reload pt2)

	void PlaySnd(RE::BGSSoundDescriptorForm* a_snd)
	{
		auto pc = RE::PlayerCharacter::GetSingleton();
		auto am = RE::BSAudioManager::GetSingleton();
		if (!a_snd || !pc || !am) {
			return;
		}
		RE::BSSoundHandle h;
		if (am->GetSoundHandle(h, a_snd)) {
			h.SetPosition(pc->GetPosition());
			h.SetObjectToFollow(pc->Get3D());
			h.Play();
		}
	}

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

	void ClipSnd::Play() { PlaySnd(g_clipIn); }
	void ClipSnd::Cylinder() { PlaySnd(g_bfrCyl); }

	bool EquippedGunIsGarand()
	{
		auto g = EquippedGun();
		return g && g->mech == 5;
	}

	// Magazine empty -> reload. The old way (queued unequip + equip of the ammo straight after the last shot) broke
	// when the player kept clicking: the next attack slipped in between, ran with no bolt (a bare swing), and the
	// queued re-equip got dropped, leaving "no bolts". Now: wait for the attack to finish (max 0.4 s), do both
	// changes immediately in one task, then check 0.35 s later that the ammo is really back (re-equip up to 3x).
	Gun*           g_pend{ nullptr };     // waiting to reload
	float          g_pendT{ 0.0f };
	Gun*           g_check{ nullptr };    // reloaded, verifying
	RE::TESAmmo*   g_checkAmmo{ nullptr };
	float          g_checkT{ 0.0f };
	int            g_checkTries{ 0 };

	RE::TESAmmo*   g_unloaded{ nullptr };

	// all of the ammo in the inventory: equipping only 1 (1.4.1/1.4.2 after the Garand unload) left exactly one round
	// equipped, so the 1st shot of the new clip used it up -> "no bolts" + a reload motion after one shot
	std::int32_t AmmoCount(RE::PlayerCharacter* a_pc, RE::TESAmmo* a_ammo)
	{
		auto inv = a_pc->GetInventoryCounts([a_ammo](RE::TESBoundObject& o) { return &o == a_ammo; });
		auto it = inv.find(a_ammo);
		return it != inv.end() && it->second > 0 ? it->second : 1;
	}  // Garand: ammo taken off at the last shot (empty rifle: no 9th shot)

	void Reload(Gun* a_gun)
	{
		g_pend = a_gun;
		g_pendT = 0.0f;
	}

	// Empty magazine (Garand since 1.4.1, every gun since 1.4.7): take the ammo off right away. A fast click can no longer start a
	// 9th attack that the reload would then cut in half ("no bolts" + reload motion, 1.4.0 with quick clicks).
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
				SKSE::log::info("{}: clip empty - ammo off until the reload", a_gun->name);
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
			if (a_gun->mech == 5 && Mech::g_locked) {
				Mech::g_closeT = 0.0f;  // the reload animation starts now; its op rod release closes the rifle
			}
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
				if (auto ammo = g_unloaded) {   // Garand put away while empty: give the ammo back (no animation needed)
					g_unloaded = nullptr;
					SKSE::GetTaskInterface()->AddTask([ammo]() {
						auto pc = RE::PlayerCharacter::GetSingleton();
						if (pc && !pc->GetCurrentAmmo()) {
							g_reloading = true;
							RE::ActorEquipManager::GetSingleton()->EquipObject(pc, ammo, nullptr, AmmoCount(pc, ammo), nullptr, false, false, false);
							g_reloading = false;
						}
					});
				}
			} else {
				g_pendT += a_dt;
				const auto st = a_pc->AsActorState()->GetAttackState();
				const bool idle = st == RE::ATTACK_STATE_ENUM::kNone;
				const float limit = g_unloaded ? 1.5f : 0.4f;   // ammo off meanwhile, so waiting is safe
				if ((idle && g_pendT >= 0.05f) || g_pendT >= limit) {
					SKSE::log::info("{}: reload after {:.2f}s ({}, attack state {})", g_pend->name, g_pendT, idle ? "attack done" : "timeout", static_cast<std::uint32_t>(st));
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
					SKSE::GetTaskInterface()->AddTask([ammo]() {
						auto pc = RE::PlayerCharacter::GetSingleton();
						if (pc && !pc->GetCurrentAmmo()) {
							g_reloading = true;
							RE::ActorEquipManager::GetSingleton()->EquipObject(pc, ammo, nullptr, AmmoCount(pc, ammo), nullptr, false, false, false);
							g_reloading = false;
						}
					});
				}
			}
		}
	}

	// Per-view placement: the same nif can need a slightly different spot in 1st and 3rd person (different hand
	// animations). The gun nifs have a PORAdjust node; its local translation is set here for each view's 3D.
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
			RE::NiMatrix3 rot = fp ? Mech::Rot(2, gun->fr[1]) * Mech::Rot(0, gun->fr[0]) : Mech::Rot(0, 0.0f);
			bool same = n->local.translate.x == want.x && n->local.translate.y == want.y && n->local.translate.z == want.z && n->local.scale == sc;
			for (int i = 0; i < 3 && same; ++i) {
				for (int j = 0; j < 3; ++j) {
					same = same && n->local.rotate.entry[i][j] == rot.entry[i][j];
				}
			}
			if (!same) {
				n->local.translate = want;
				n->local.rotate = rot;
				n->local.scale = sc;
				RE::NiUpdateData ud{};
				n->Update(ud);
			}
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	// ADS (aim down sights), Fallout style, Garand only for now (1.5.0):
	//  - right mouse / block ("Left Attack/Block") is taken while the Garand is drawn: no bash, no block
	//  - held -> global PORAimDown (AE7) = 1; OAR submod "GarandADS" (interruptible) swaps in the shouldered set,
	//    released -> 0, the hip set (rifle low, shots fired from there)
	//  - view zooms in a bit; Eagle Eye: more zoom + 10 stamina/s (as vanilla), Steady Hand 1/2: time 75%/50%
	namespace ADS
	{
		RE::TESGlobal* g_global{ nullptr };
		RE::BGSPerk*   g_eagleEye{ nullptr };
		RE::BGSPerk*   g_steady1{ nullptr };
		RE::BGSPerk*   g_steady2{ nullptr };
		std::atomic_bool g_held{ false };
		bool  g_on = false;        // ADS state applied
		bool  g_zoomOK = true;     // false after running out of stamina (until released)
		float g_baseFOV = 0.0f;    // world FOV before zooming, 0 = not zoomed
		float g_zoom = 0.0f;       // 0..1 eased
		bool  g_slowed = false;

		bool Allowed(RE::PlayerCharacter* a_pc)
		{
			if (!a_pc || !EquippedGunIsGarand()) {
				return false;
			}
			const auto st = a_pc->AsActorState()->GetWeaponState();
			return st == RE::WEAPON_STATE::kDrawn;
		}

		struct Button
		{
			static void thunk(RE::AttackBlockHandler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
			{
				auto ue = RE::UserEvents::GetSingleton();
				if (a_event && ue && a_event->QUserEvent() == ue->leftAttack && Allowed(RE::PlayerCharacter::GetSingleton())) {
					g_held = a_event->IsPressed();
					return;  // swallowed: no block / bash with the rifle
				}
				func(a_this, a_event, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_AttackBlockHandler[0] };
			Button::func = vtbl.write_vfunc(REL::Module::IsVR() ? 0x6 : 0x4, Button::thunk);
		}

		void Load()
		{
			auto dh = RE::TESDataHandler::GetSingleton();
			auto g = dh ? dh->LookupForm(0xAE7, kPlugin) : nullptr;
			g_global = g ? g->As<RE::TESGlobal>() : nullptr;
			auto perk = [](RE::FormID id) { auto f = RE::TESForm::LookupByID(id); return f ? f->As<RE::BGSPerk>() : nullptr; };
			g_eagleEye = perk(0x58F61);
			g_steady1 = perk(0x103ADA);
			g_steady2 = perk(0x103ADB);
			SKSE::log::info("ADS: global {}, Eagle Eye {}, Steady Hand {}/{}", g_global ? "ok" : "missing", g_eagleEye ? "ok" : "-", g_steady1 ? "ok" : "-", g_steady2 ? "ok" : "-");
		}

		void SetSlow(float a_mult)
		{
			if (auto t = RE::BSTimer::GetSingleton()) {
				t->SetGlobalTimeMultiplier(a_mult, true);
			}
		}

		void Tick(RE::PlayerCharacter* a_pc, float a_dt)
		{
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return;
			}
			const bool allowed = Allowed(a_pc);
			if (!allowed) {
				g_held = false;
			}
			const bool want = allowed && g_held;
			if (want != g_on) {
				g_on = want;
				g_zoomOK = true;
				if (g_global) {
					g_global->value = want ? 1.0f : 0.0f;
				}
				SKSE::log::info("ADS {}", want ? "on" : "off");
			}
			auto cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !a_pc) {
				return;
			}
			const bool eagle = g_eagleEye && a_pc->HasPerk(g_eagleEye);
			auto av = a_pc->AsActorValueOwner();
			if (g_on && eagle && g_zoomOK && av) {   // vanilla Eagle Eye cost: 10 stamina per second while zoomed
				const float real = a_dt / (std::max)(0.1f, RE::BSTimer::QGlobalTimeMultiplier());
				av->DamageActorValue(RE::ActorValue::kStamina, 10.0f * real);
				if (av->GetActorValue(RE::ActorValue::kStamina) <= 0.0f) {
					g_zoomOK = false;
				}
			}
			const bool zoomed = g_on && g_zoomOK;
			const float target = zoomed ? 1.0f : 0.0f;
			const float realDt = a_dt / (std::max)(0.1f, RE::BSTimer::QGlobalTimeMultiplier());
			const float step = realDt / 0.15f;
			g_zoom = g_zoom < target ? (std::min)(target, g_zoom + step) : (std::max)(target, g_zoom - step);
			if (g_zoom > 0.0f) {
				if (g_baseFOV <= 0.0f) {
					g_baseFOV = cam->GetRuntimeData2().worldFOV;
				}
				const float k = eagle ? 0.62f : 0.85f;   // FOV multiplier at full zoom
				const float e = g_zoom * g_zoom * (3.0f - 2.0f * g_zoom);
				cam->GetRuntimeData2().worldFOV = g_baseFOV * (1.0f - (1.0f - k) * e);
			} else if (g_baseFOV > 0.0f) {
				cam->GetRuntimeData2().worldFOV = g_baseFOV;
				g_baseFOV = 0.0f;
			}
			// Steady Hand: slow time while zoomed
			float slow = 1.0f;
			if (zoomed && g_zoom >= 1.0f) {
				if (g_steady2 && a_pc->HasPerk(g_steady2)) {
					slow = 0.5f;
				} else if (g_steady1 && a_pc->HasPerk(g_steady1)) {
					slow = 0.75f;
				}
			}
			if (slow < 1.0f && !g_slowed) {
				SetSlow(slow);
				g_slowed = true;
			} else if (slow >= 1.0f && g_slowed) {
				SetSlow(1.0f);
				g_slowed = false;
			}
		}

		void Reset()
		{
			g_held = false;
			g_on = false;
			if (g_global) {
				g_global->value = 0.0f;
			}
			if (g_slowed) {
				SetSlow(1.0f);
				g_slowed = false;
			}
			auto cam = RE::PlayerCamera::GetSingleton();
			if (cam && g_baseFOV > 0.0f) {
				cam->GetRuntimeData2().worldFOV = g_baseFOV;
			}
			g_baseFOV = 0.0f;
			g_zoom = 0.0f;
		}
	}

	// diagnostics (Garand only): every animation event of the player with time and attack state, to see which clip runs
	class AnimLog : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		static AnimLog* Get()
		{
			static AnimLog s;
			return &s;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			if (!a_ev || !EquippedGunIsGarand()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const std::string_view tag = a_ev->tag.c_str() ? a_ev->tag.c_str() : "";
			if (tag.starts_with("Foot") || tag.starts_with("SoundPlay.FS") || tag == "TDM_" || tag.empty()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto pc = RE::PlayerCharacter::GetSingleton();
			auto ammo = pc ? pc->GetCurrentAmmo() : nullptr;
			SKSE::log::info("  anim {} [{}] state {} ammo {}", tag, a_ev->payload.c_str() ? a_ev->payload.c_str() : "",
				pc ? static_cast<std::uint32_t>(pc->AsActorState()->GetAttackState()) : 99u, ammo ? "on" : "off");
			return RE::BSEventNotifyControl::kContinue;
		}
		void Attach()
		{
			if (auto pc = RE::PlayerCharacter::GetSingleton()) {
				pc->RemoveAnimationGraphEventSink(this);
				pc->AddAnimationGraphEventSink(this);
			}
		}
	};

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
				last = now; lastGun = gun;
				char gap[32];
				if (dt >= 0.0f && dt < 5.0f) {
					std::snprintf(gap, sizeof(gap), "+%.3fs", dt);
				} else {
					std::snprintf(gap, sizeof(gap), "(first)");
				}
				SKSE::log::info("{}: shot {}/{} {} (attack state {}, speed x{:.2f})", gun->name, gun->fired, gun->size, gap,
					static_cast<std::uint32_t>(pc->AsActorState()->GetAttackState()), SpeedMult(pc));
			}
			if (gun->fired >= gun->size) {
				gun->fired = 0;
				if (gun->mech == 5) {
					Mech::g_locked = true;   // last round: the clip flies out, the op rod stays back
					Mech::g_closeT = -1.0f;
					PlaySnd(g_ping);
				}
				Unload(gun);   // 1.4.7: every gun - no extra shot between the empty magazine and the reload (1.4.6 log: SAA/M29 fired a 7th)
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
					if (g.mech == 5) {
						AnimLog::Get()->Attach();
						SKSE::log::info("{}: equipped", g.name);
					}
					g.fired = 0;  // gun (re)equipped: full magazine
					Mech::g_locked = false;
					Mech::g_closeT = -1.0f;
				}
			}
			auto form = RE::TESForm::LookupByID(a_ev->baseObject);
			if (form && form->Is(RE::FormType::Ammo)) {
				if (auto gun = EquippedGun()) {
					gun->fired = 0;  // ammo equipped by hand = manual reload
					if (gun->mech == 5 && Mech::g_locked && Mech::g_closeT < 0.0f) {
						Mech::g_closeT = 0.0f;
					}
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
		auto ping = dh ? dh->LookupForm(0xAE5, kPlugin) : nullptr;
		g_ping = ping ? ping->As<RE::BGSSoundDescriptorForm>() : nullptr;
		auto clip = dh ? dh->LookupForm(0xAE6, kPlugin) : nullptr;
		g_clipIn = clip ? clip->As<RE::BGSSoundDescriptorForm>() : nullptr;
		auto cyl = dh ? dh->LookupForm(0xAD5, kPlugin) : nullptr;
		g_bfrCyl = cyl ? cyl->As<RE::BGSSoundDescriptorForm>() : nullptr;
		ADS::Load();
		SKSE::log::info("Garand clip ping sound: {}, reload sound: {}; BFR cylinder: {}", g_ping ? "ok" : "missing", g_clipIn ? "ok" : "missing", g_bfrCyl ? "ok" : "missing");
		auto src = RE::ScriptEventSourceHolder::GetSingleton();
		src->AddEventSink<RE::TESContainerChangedEvent>(Sink::Get());
		src->AddEventSink<RE::TESEquipEvent>(Sink::Get());
	}

	void OnGameLoaded()
	{
		for (auto& g : g_guns) {
			g.fired = 0;
		}
		Mech::g_locked = false;
		Mech::g_closeT = -1.0f;
		g_unloaded = nullptr;
		AnimLog::Get()->Attach();
		ADS::Reset();
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .logName = "PortableOneRoomMag", .logPattern = "[%H:%M:%S.%e] %v" });
	SKSE::log::info("PortableOneRoomMag 1.5.0 - runtime {}", a_skse->RuntimeVersion().string());
	Mech::Install();
	ADS::Install();

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
