#include <chrono>
#include <cctype>
// KingsglaiveFX: the phantom-sword show of the Kingsglaive mod without its Papyrus scripts.
// The OAR clips carry markers (SoundPlay.KGS_<sword>, SoundPlay.KGC_<stage>, SoundPlay.KGJ). This plugin listens to the
// player's animation events while the King's sword (Kingsglaive.esp 0x800) is in the right hand and:
//   KGS_<sword>        equips that phantom sword (hidden armor attached to the hand/weapon node) + summon sound
//   CastOKStop/attackStop  breaks the phantom swords: shatter explosion at the magic node + break sound, item removed
//   preHitFrame/weaponSwing/KillMoveStart with no phantom out -> Executioner (the original "default" phantom)
//   blockStart / blockStop  Aureliane (phantom shield) out / away
//   KGC_<n>            combo stage n (hidden abilities SML1..4 - the OAR configs pick the next combo / smash clip by them)
//   KGJ                judgement cut: the projectile spell, magnitude = weapon damage x attack damage mult
// Equipping the sword puts on the hand runes; unequipping it clears everything. Nothing is saved in the co-save:
// on load all phantoms / stage abilities are cleared.

namespace
{
	constexpr std::string_view kPlugin = "Kingsglaive.esp";
	constexpr RE::FormID kWeapon = 0x800, kSndBreak = 0x807, kSndSummon = 0x808, kSndJudge = 0x809, kSndSummonK = 0x80A,
						 kCutSpell = 0x80E, kExpl = 0x811, kExplL = 0x812, kDummy = 0x813, kStage1 = 0x818, kRuneR = 0x82A, kRuneL = 0x82B;

	enum Side { kR, kL, kNone };
	struct Phantom
	{
		const char*      name;
		RE::FormID       local;
		std::uint8_t     slots;   // 1 = slot 39, 2 = slot 48
		Side             side;    // shatter at the right / left magic node, kNone = no shatter effect
		bool             breakSound;
		int              summonSound;  // 0 none, 1 normal, 2 "K" (judgement)
		RE::TESObjectARMO* form{ nullptr };
		bool             out{ false };
	};

	Phantom g_ph[] = {
		{ "Eclair", 0x81C, 3, kR, true, 1 }, { "Judgement", 0x81D, 3, kL, true, 2 }, { "Kyrie", 0x81E, 3, kR, true, 1 },
		{ "Lapis", 0x81F, 3, kR, true, 1 }, { "LiberateSky", 0x820, 3, kR, true, 1 }, { "Prelude", 0x821, 1, kR, true, 1 },
		{ "Revenant", 0x822, 3, kR, true, 1 }, { "Sazanami", 0x823, 1, kR, true, 0 }, { "SazanamiSCB", 0x824, 2, kL, false, 1 },
		{ "SazanamiTsuka", 0x825, 1, kNone, false, 0 }, { "Unfaith", 0x826, 2, kL, true, 1 }, { "Untruth", 0x827, 1, kR, true, 1 },
		{ "Aureliane", 0x828, 3, kNone, true, 0 }, { "Executioner", 0x829, 3, kR, true, 1 },
	};

	RE::TESObjectWEAP*          g_weapon{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndBreak{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndSummon{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndSummonK{ nullptr };
	RE::BGSSoundDescriptorForm* g_sndJudge{ nullptr };
	RE::SpellItem*              g_cut{ nullptr };
	RE::BGSExplosion*           g_expl{ nullptr };
	RE::BGSExplosion*           g_explL{ nullptr };
	RE::TESObjectACTI*          g_dummy{ nullptr };
	RE::SpellItem*              g_stage[5]{};  // 1..4
	RE::TESObjectARMO*          g_runeR{ nullptr };
	RE::TESObjectARMO*          g_runeL{ nullptr };

	int g_curStage = 0;
	std::chrono::steady_clock::time_point g_stageTime{};

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
			if (a_keepShield && p.local == 0x828) {
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
		if (a_p.summonSound) {
			PlaySound(a_p.summonSound == 2 ? g_sndSummonK : g_sndSummon, a_pc);
		}
		if (a_p.local == 0x824) {   // scabbard comes with its hilt
			if (auto t = Find("SazanamiTsuka")) {
				Summon(a_pc, *t);
			}
		}
	}

	void SetStage(RE::PlayerCharacter* a_pc, int a_n)
	{
		for (int i = 1; i <= 4; ++i) {
			if (g_stage[i] && i != a_n && a_pc->HasSpell(g_stage[i])) {
				a_pc->RemoveSpell(g_stage[i]);
			}
		}
		if (a_n >= 1 && a_n <= 4 && g_stage[a_n] && !a_pc->HasSpell(g_stage[a_n])) {
			a_pc->AddSpell(g_stage[a_n]);
		}
		g_curStage = a_n;
		g_stageTime = std::chrono::steady_clock::now();
	}

	void JudgementCut(RE::PlayerCharacter* a_pc)
	{
		if (!g_cut) {
			return;
		}
		float dmg = g_weapon ? static_cast<float>(g_weapon->GetAttackDamage()) : 24.0f;
		const float mult = a_pc->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAttackDamageMult);
		if (mult > 0.0f) {
			dmg *= mult;
		}
		if (auto caster = a_pc->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
			caster->CastSpellImmediate(g_cut, false, nullptr, 1.0f, false, dmg, a_pc);
		}
		PlaySound(g_sndJudge, a_pc);
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
		if (g_curStage && std::chrono::steady_clock::now() - g_stageTime > std::chrono::seconds(4)) {
			SetStage(pc, 0);   // combo given up without an attackStop (stagger, menu, ...)
		}
		if (IEquals(a_tag, "SoundPlay")) {
			std::string_view p{ a_payload };
			if (p.starts_with("KGS_")) {
				if (auto ph = Find(p.substr(4))) {
					Summon(pc, *ph);
				}
			} else if (p.starts_with("KGC_") && p.size() == 5) {
				SetStage(pc, p[4] - '0');
			} else if (p == "KGJ") {
				JudgementCut(pc);
			}
			return;
		}
		if (IEquals(a_tag, "CastOKStop")) {
			BreakAll(pc, true, true);
		} else if (IEquals(a_tag, "attackStop") || IEquals(a_tag, "bashStop")) {
			BreakAll(pc, true, true);
			SetStage(pc, 0);
		} else if (IEquals(a_tag, "preHitFrame") || IEquals(a_tag, "weaponSwing") || IEquals(a_tag, "KillMoveStart")) {
			if (!AnyOut()) {
				if (auto ex = Find("Executioner")) {
					Summon(pc, *ex);
				}
			}
			if (IEquals(a_tag, "KillMoveStart")) {
				SetStage(pc, 0);
			}
		} else if (IEquals(a_tag, "blockStart")) {
			if (auto sh = Find("Aureliane"); sh && !sh->out) {
				BreakAll(pc, true);
				Summon(pc, *sh);
			}
		} else if (IEquals(a_tag, "blockStop")) {
			if (auto sh = Find("Aureliane")) {
				Break(pc, *sh, true);
			}
		} else if (IEquals(a_tag, "weaponSheathe") || IEquals(a_tag, "staggerStart")) {
			BreakAll(pc, true);
			SetStage(pc, 0);
		}
	}

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
				"weaponSwing", "KillMoveStart", "blockStart", "blockStop", "weaponSheathe", "staggerStart" };
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
				SKSE::log::info("King's sword {}", on ? "equipped" : "unequipped");
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnDataLoaded()
	{
		g_weapon = Lookup<RE::TESObjectWEAP>(kWeapon);
		g_sndBreak = Lookup<RE::BGSSoundDescriptorForm>(kSndBreak);
		g_sndSummon = Lookup<RE::BGSSoundDescriptorForm>(kSndSummon);
		g_sndSummonK = Lookup<RE::BGSSoundDescriptorForm>(kSndSummonK);
		g_sndJudge = Lookup<RE::BGSSoundDescriptorForm>(kSndJudge);
		g_cut = Lookup<RE::SpellItem>(kCutSpell);
		g_expl = Lookup<RE::BGSExplosion>(kExpl);
		g_explL = Lookup<RE::BGSExplosion>(kExplL);
		g_dummy = Lookup<RE::TESObjectACTI>(kDummy);
		for (int i = 1; i <= 4; ++i) {
			g_stage[i] = Lookup<RE::SpellItem>(kStage1 + i - 1);
		}
		g_runeR = Lookup<RE::TESObjectARMO>(kRuneR);
		g_runeL = Lookup<RE::TESObjectARMO>(kRuneL);
		int n = 0;
		for (auto& p : g_ph) {
			p.form = Lookup<RE::TESObjectARMO>(p.local);
			n += p.form ? 1 : 0;
		}
		SKSE::log::info("forms: sword {:08X}, {} phantom swords, cut {}, explosions {}/{}", g_weapon ? g_weapon->GetFormID() : 0, n,
			g_cut != nullptr, g_expl != nullptr, g_explL != nullptr);
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
		SKSE::log::info("game loaded, King's sword {}", holds ? "in hand" : "not equipped");
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .logName = "KingsglaiveFX", .logPattern = "[%H:%M:%S.%e] %v" });
	SKSE::log::info("KingsglaiveFX 1.0.0 - runtime {}", a_skse->RuntimeVersion().string());
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			SKSE::GetTaskInterface()->AddTask([]() { OnGameLoaded(); });
			break;
		default:
			break;
		}
	});
	return true;
}
