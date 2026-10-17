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

