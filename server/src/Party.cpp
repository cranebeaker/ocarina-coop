#include "Party.hpp"
#include "Player.hpp"
#include "Scene.hpp"
#include "nlohmann/json.hpp"

namespace ZeldaOnline
{
	void Party::SetSettings(const std::string& settings) {
		if (settings.empty())
		{
			m_settings.clear();
			m_privateDungeons = false;
			m_storyCoop = false;
			m_shareConsumables = false;
			return;
		}

		try
		{
			auto parsed = nlohmann::json::parse(settings);

			if (!parsed.is_object())
			{
				std::printf("Party: settings json is not an object, ignoring\n");
				return;
			}

			m_privateDungeons = parsed.value("privateDungeons", false);
			m_storyCoop = parsed.value("storyCoop", false);
			m_shareConsumables = m_storyCoop && parsed.value("shareAmmoRupeesHp", false);
			m_settings = parsed.dump();
		}
		catch (const std::exception& e)
		{
			std::printf("Party: failed to parse settings json (%s), ignoring\n", e.what());
		}
	}

	bool Party::SetSettingsFromMember(const std::string& settings, bool isAdmin)
	{
		if (isAdmin || !m_storyCoop)
		{
			// a non-admin may not switch Story Co-op on, so check the incoming keys when it is currently off
			if (!isAdmin)
			{
				try
				{
					auto parsed = nlohmann::json::parse(settings, nullptr, false);
					if (parsed.is_object() && (parsed.value("storyCoop", false) || parsed.value("shareAmmoRupeesHp", false)))
					{
						parsed["storyCoop"] = false;
						parsed["shareAmmoRupeesHp"] = false;
						SetSettings(parsed.dump());
						return false;
					}
				}
				catch (const std::exception&)
				{
				}
			}

			SetSettings(settings);
			return true;
		}

		// Story Co-op is active and the sender is not the admin: keep the current Story Co-op keys
		auto parsed = nlohmann::json::parse(settings, nullptr, false);
		if (!parsed.is_object())
			return false;

		bool same = parsed.value("storyCoop", false) == m_storyCoop &&
			parsed.value("shareAmmoRupeesHp", false) == m_shareConsumables;

		parsed["storyCoop"] = m_storyCoop;
		parsed["shareAmmoRupeesHp"] = m_shareConsumables;
		SetSettings(parsed.dump());
		return same;
	}

	bool Party::MergeStory(const StoryProgress& incoming)
	{
		bool changed = false;

		if (!m_storySeeded)
		{
			// the first snapshot also seeds the small key counts, afterwards UPDATE_DUNGEON_KEYS owns them
			for (int i = 0; i < 19; i++)
			{
				if (incoming.dungeonKeys[i] > 0)
					m_dungeonKeys[i] = (uint8_t)incoming.dungeonKeys[i];
			}
			m_storySeeded = true;
			changed = true;
		}

		if (m_story.MergeFrom(incoming))
			changed = true;

		return changed;
	}

	void Party::SendToAll(const ByteStream& payload, Player* except)
	{
		for (auto& other : m_members)
		{
			if (other == except)
				continue;
			other->SendPacket(payload);
		}
	}

	void Party::SendToScene(Scene* scene, const ByteStream& payload, Player* except)
	{
		for (auto& other : m_members)
		{
			if (other == except || other->CurrentScene() != scene)
				continue;
			other->SendPacket(payload);
		}
	}
}

