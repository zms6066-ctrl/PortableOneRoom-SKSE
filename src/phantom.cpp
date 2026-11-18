// Phantom Sword (Portable One Room.esp B00): the phantom-sword show of the Kingsglaive mod, without its Papyrus scripts.
// The OAR clips (OpenAnimationReplacer\\PortableOneRoom_PhantomSword) carry markers SoundPlay.KGS_<sword> / SoundPlay.KGC_<stage>.
// While the sword is in the player's right hand:
//   KGS_<sword>              that phantom sword (hidden armor in slot 39/48 on the hand / weapon node) + summon sound
//   CastOKStop / attackStop  the phantom swords shatter: explosion at the magic node + break sound, item removed
//   preHitFrame / weaponSwing / KillMoveStart with none out -> Executioner
//   blocking (polled each frame)  Aureliane (phantom shield)
//   KGC_<n>                  combo stage n: hidden abilities B0E-B10, read by the OAR configs to pick combo 2-4.
//                            attackStop keeps the stage 1.2 s (vanilla 2H chains only right -> left)
// Equipping the sword puts on the hand runes; unequipping clears everything; loading a save clears leftovers.
#include "phantom.h"
#include <chrono>
#include <cctype>

namespace
{
	constexpr std::string_view kPlugin = "Portable One Room.esp";
	constexpr RE::FormID kWeapon = 0xB00, kSndBreak = 0xB04, kSndSummon = 0xB05, kExpl = 0xB08, kExplL = 0xB09, kDummy = 0xB0A,
						 kStage1 = 0xB0E, kRuneR = 0xB2A, kRuneL = 0xB2B;
	constexpr int        kStages = 3;

	enum Side { kR, kL, kNone };
	struct Phantom
	{
		const char*      name;
		RE::FormID       local;
		std::uint8_t     slots;   // 1 = slot 39, 2 = slot 48
		Side             side;    // shatter at the right / left magic node, kNone = no shatter effect
		bool             breakSound;
		bool             summonSound;
		RE::TESObjectARMO* form{ nullptr };
		bool             out{ false };
	};

	Phantom g_ph[] = {
		{ "Prelude", 0xB20, 1, kR, true, true }, { "Lapis", 0xB21, 3, kR, true, true }, { "Untruth", 0xB22, 1, kR, true, true },
		{ "Unfaith", 0xB23, 2, kL, true, true }, { "Revenant", 0xB24, 3, kR, true, true }, { "Eclair", 0xB25, 3, kR, true, true },
		{ "LiberateSky", 0xB26, 3, kR, true, true }, { "Kyrie", 0xB27, 3, kR, true, true }, { "Aureliane", 0xB28, 3, kNone, true, false },
		{ "Executioner", 0xB29, 3, kR, true, true },
	};

	RE::TESObjectWEAP*          g_weapon{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndBreak{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndSummon{ nullptr };
	RE::BGSExplosion*           g_expl{ nullptr };
	RE::BGSExplosion*           g_explL{ nullptr };
	RE::TESObjectACTI*          g_dummy{ nullptr };
	RE::SpellItem*              g_stage[kStages + 1]{};  // 1..3
	RE::TESObjectARMO*          g_runeR{ nullptr };
	RE::TESObjectARMO*          g_runeL{ nullptr };

	int g_curStage = 0;
	std::chrono::steady_clock::time_point g_stageTime{};
	std::chrono::steady_clock::time_point g_resetAt{};   // attackStop: the stage survives a moment (vanilla 2H chains
	bool g_resetPending = false;                         // only right -> left, the next click starts a new attack)
	bool g_wasBlocking = false;

	template <class T>
	T* Lookup(RE::FormID a_local)
	{
		auto dh = RE::TESDataHandler::GetSingleton();
		auto f = dh ? dh->LookupForm(a_local, kPlugin) : nullptr;
		return f ? f->As<T>() : nullptr;
	}

	Phantom* Find(std::string_view a_name)
	{
		for (auto& p : g_ph) {
			if (a_name == p.name) {
				return &p;
			}
		}
		return nullptr;
	}

	bool IEquals(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
				return false;
			}
		}
		return true;
	}

	bool HoldsSword(RE::PlayerCharacter* a_pc)
	{
		auto obj = a_pc ? a_pc->GetEquippedObject(false) : nullptr;
		return obj && g_weapon && obj == g_weapon;
	}

	void PlaySound(RE::BGSSoundDescriptorForm* a_snd, RE::PlayerCharacter* a_pc)
	{
		if (!a_snd || !a_pc) {
			return;
		}
		RE::BSSoundHandle h;
		if (auto am = RE::BSAudioManager::GetSingleton(); am && am->GetSoundHandle(h, a_snd)) {
			h.SetPosition(a_pc->GetPosition());
			if (auto root = a_pc->Get3D()) {
				h.SetObjectToFollow(root);
			}
			h.Play();
		}
	}

	std::int32_t Count(RE::PlayerCharacter* a_pc, RE::TESBoundObject* a_obj)
	{
		auto inv = a_pc->GetInventoryCounts([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
		auto it = inv.find(a_obj);
		return it != inv.end() ? it->second : 0;
	}

	void PutOn(RE::PlayerCharacter* a_pc, RE::TESObjectARMO* a_armor)
	{
		if (!a_armor) {
			return;
		}
		if (Count(a_pc, a_armor) <= 0) {
			a_pc->AddObjectToContainer(a_armor, nullptr, 1, nullptr);
		}
		RE::ActorEquipManager::GetSingleton()->EquipObject(a_pc, a_armor, nullptr, 1, nullptr, false, true, false, true);
	}

	void TakeOff(RE::PlayerCharacter* a_pc, RE::TESObjectARMO* a_armor)
	{
		if (!a_armor) {
			return;
		}
		if (const auto n = Count(a_pc, a_armor); n > 0) {
			RE::ActorEquipManager::GetSingleton()->UnequipObject(a_pc, a_armor, nullptr, 1, nullptr, false, true, false, true);
			a_pc->RemoveItem(a_armor, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		}
	}

	void Shatter(RE::PlayerCharacter* a_pc, Side a_side)
	{
		auto expl = a_side == kL ? g_explL : g_expl;
		auto root = a_pc->Get3D();
		if (!expl || !g_dummy || !root || a_side == kNone) {
			return;
		}
		static const RE::BSFixedString nodeR{ "NPC R MagicNode [RMag]" }, nodeL{ "NPC L MagicNode [LMag]" };
		auto node = root->GetObjectByName(a_side == kL ? nodeL : nodeR);
		auto dummy = a_pc->PlaceObjectAtMe(g_dummy, false);
		if (!dummy) {
			return;
		}
		if (node) {
			dummy->SetPosition(node->world.translate);
		}
		dummy->PlaceObjectAtMe(expl, false);
		dummy->Disable();
		dummy->SetDelete(true);
	}

	void Break(RE::PlayerCharacter* a_pc, Phantom& a_p, bool a_fx)
	{
		if (!a_p.out) {
			return;
		}
		a_p.out = false;
		TakeOff(a_pc, a_p.form);
		if (a_fx) {
			Shatter(a_pc, a_p.side);
			if (a_p.breakSound) {
				PlaySound(g_sndBreak, a_pc);
			}
		}
	}

	void BreakAll(RE::PlayerCharacter* a_pc, bool a_fx, bool a_keepShield = false)
	{
		for (auto& p : g_ph) {
			if (a_keepShield && p.local == 0xB28) {
				continue;
			}
			Break(a_pc, p, a_fx);
		}
	}

	bool AnyOut()
	{
		for (auto& p : g_ph) {
			if (p.out) {
				return true;
			}
		}
		return false;
	}

	void Summon(RE::PlayerCharacter* a_pc, Phantom& a_p)
	{
		if (!a_p.form) {
			return;
		}
		for (auto& o : g_ph) {   // the engine would push the old sword out of the slot: shatter it like the original did
			if (o.out && &o != &a_p && (o.slots & a_p.slots)) {
				Break(a_pc, o, true);
			}
		}
		if (a_p.out) {
			return;
		}
		PutOn(a_pc, a_p.form);
		a_p.out = true;
		SKSE::log::info("phantom sword: summon {}", a_p.name);
		if (a_p.summonSound) {
			PlaySound(g_sndSummon, a_pc);
		}
	}

	void SetStage(RE::PlayerCharacter* a_pc, int a_n)
	{
		if (a_n < 0 || a_n > kStages) {
			a_n = 0;
		}
		for (int i = 1; i <= kStages; ++i) {
			if (g_stage[i] && i != a_n && a_pc->HasSpell(g_stage[i])) {
				a_pc->RemoveSpell(g_stage[i]);
			}
		}
		if (a_n >= 1 && g_stage[a_n] && !a_pc->HasSpell(g_stage[a_n])) {
			a_pc->AddSpell(g_stage[a_n]);
		}
		if (a_n != g_curStage) {
			SKSE::log::info("phantom sword: combo stage {}", a_n);
		}
		g_curStage = a_n;
		g_stageTime = std::chrono::steady_clock::now();
		g_resetPending = false;
	}

	void Runes(RE::PlayerCharacter* a_pc, bool a_on)
	{
		if (a_on) {
			PutOn(a_pc, g_runeR);
			PutOn(a_pc, g_runeL);
		} else {
			TakeOff(a_pc, g_runeR);
			TakeOff(a_pc, g_runeL);
		}
	}

	void Clear(RE::PlayerCharacter* a_pc)
	{
		g_wasBlocking = false;
		for (auto& p : g_ph) {
			p.out = false;
			TakeOff(a_pc, p.form);
		}
		SetStage(a_pc, 0);
	}

	void Handle(std::string a_tag, std::string a_payload)
	{
		auto pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !HoldsSword(pc)) {
			return;
		}
		if (IEquals(a_tag, "SoundPlay")) {
			std::string_view p{ a_payload };
			if (p.starts_with("KGS_")) {
				if (auto ph = Find(p.substr(4))) {
					Summon(pc, *ph);
				}
			} else if (p.starts_with("KGC_") && p.size() == 5) {
				SetStage(pc, p[4] - '0');
			}
			return;
		}
		if (IEquals(a_tag, "CastOKStop")) {
			BreakAll(pc, true, true);
		} else if (IEquals(a_tag, "attackStop") || IEquals(a_tag, "bashStop")) {
			BreakAll(pc, true, true);
			if (g_curStage) {
				g_resetPending = true;
				g_resetAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
			}
		} else if (IEquals(a_tag, "preHitFrame") || IEquals(a_tag, "weaponSwing") || IEquals(a_tag, "KillMoveStart")) {
			if (!AnyOut()) {
				if (auto ex = Find("Executioner")) {
					Summon(pc, *ex);
				}
			}
			if (IEquals(a_tag, "KillMoveStart")) {
				SetStage(pc, 0);
			}
		} else if (IEquals(a_tag, "weaponSheathe") || IEquals(a_tag, "staggerStart")) {
			BreakAll(pc, true);
			SetStage(pc, 0);
		}
	}

	// every frame (PlayerCharacter::Update): block state -> phantom shield, delayed stage reset, stale stage timeout.
	// Blocking is polled because blockStart / blockStop only reach the event sink when the block clip carries them.
}

namespace Phantom
{
	void Tick(RE::PlayerCharacter* a_pc)
	{
		if (!HoldsSword(a_pc)) {
			g_wasBlocking = false;
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (g_resetPending && now >= g_resetAt) {
			g_resetPending = false;
			SetStage(a_pc, 0);
		}
		if (g_curStage && now - g_stageTime > std::chrono::seconds(5)) {
			SetStage(a_pc, 0);
		}
		const bool blocking = a_pc->IsBlocking();
		if (blocking != g_wasBlocking) {
			g_wasBlocking = blocking;
			if (auto sh = Find("Aureliane")) {
				if (blocking) {
					BreakAll(a_pc, true);
					Summon(a_pc, *sh);
				} else {
					Break(a_pc, *sh, true);
				}
			}
		}
	}

}

namespace
{
	class AnimSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		static AnimSink* Get()
		{
			static AnimSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			if (!a_ev || a_ev->tag.empty()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			std::string_view tag{ a_ev->tag.c_str() };
			static constexpr std::string_view kWanted[] = { "SoundPlay", "CastOKStop", "attackStop", "bashStop", "preHitFrame",
				"weaponSwing", "KillMoveStart", "weaponSheathe", "staggerStart" };
			bool want = false;
			for (auto w : kWanted) {
				if (IEquals(tag, w)) {
					want = true;
					break;
				}
			}
			if (!want) {
				return RE::BSEventNotifyControl::kContinue;
			}
			std::string payload = a_ev->payload.empty() ? std::string{} : std::string{ a_ev->payload.c_str() };
			if (IEquals(tag, "SoundPlay") && !payload.starts_with("KG")) {
				return RE::BSEventNotifyControl::kContinue;
			}
			SKSE::GetTaskInterface()->AddTask([t = std::string{ tag }, payload]() { Handle(t, payload); });
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
	{
	public:
		static EquipSink* Get()
		{
			static EquipSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev, RE::BSTEventSource<RE::TESEquipEvent>*) override
		{
			auto pc = RE::PlayerCharacter::GetSingleton();
			if (!a_ev || !a_ev->actor || a_ev->actor.get() != pc || !g_weapon || a_ev->baseObject != g_weapon->GetFormID()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const bool on = a_ev->equipped;
			SKSE::GetTaskInterface()->AddTask([on]() {
				auto pc = RE::PlayerCharacter::GetSingleton();
				if (!pc) {
					return;
				}
				if (on) {
					pc->AddAnimationGraphEventSink(AnimSink::Get());   // graph may have been rebuilt (new game, race change)
					Runes(pc, true);
				} else {
					Clear(pc);
					Runes(pc, false);
				}
				SKSE::log::info("phantom sword {}", on ? "equipped" : "unequipped");
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};

}

namespace Phantom
{
	void OnDataLoaded()
	{
		g_weapon = Lookup<RE::TESObjectWEAP>(kWeapon);
		g_sndBreak = Lookup<RE::BGSSoundDescriptorForm>(kSndBreak);
		g_sndSummon = Lookup<RE::BGSSoundDescriptorForm>(kSndSummon);
		g_expl = Lookup<RE::BGSExplosion>(kExpl);
		g_explL = Lookup<RE::BGSExplosion>(kExplL);
		g_dummy = Lookup<RE::TESObjectACTI>(kDummy);
		for (int i = 1; i <= kStages; ++i) {
			g_stage[i] = Lookup<RE::SpellItem>(kStage1 + i - 1);
		}
		g_runeR = Lookup<RE::TESObjectARMO>(kRuneR);
		g_runeL = Lookup<RE::TESObjectARMO>(kRuneL);
		int n = 0;
		for (auto& p : g_ph) {
			p.form = Lookup<RE::TESObjectARMO>(p.local);
			n += p.form ? 1 : 0;
		}
		SKSE::log::info("phantom sword: form {:08X}, {} phantom swords, explosions {}/{}", g_weapon ? g_weapon->GetFormID() : 0, n,
			g_expl != nullptr, g_explL != nullptr);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(EquipSink::Get());
	}

	void OnGameLoaded()
	{
		auto pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		pc->AddAnimationGraphEventSink(AnimSink::Get());
		Clear(pc);
		const bool holds = HoldsSword(pc);
		Runes(pc, holds);
		SKSE::log::info("phantom sword: game loaded, {}", holds ? "in hand" : "not equipped");
	}
}
