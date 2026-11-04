#pragma once

// Header-only core of the resource sync repair (kept free of game/hook code so it can be unit tested).
//
// Server behaviour this works around (tpp-server-emulator, CMD_SYNC_RESOURCE):
//   the server only applies a client sync when  client_version == server_version
//   and  request.version != client_version - 1.
// Some server commands (e.g. CMD_MINING_RESOURCE) raise server_version without telling the client,
// so the next sync is silently rejected and the server answers with the OLD stored values.
//
// Before the game sends its real CMD_SYNC_RESOURCE (request version V) we:
//   1. send a probe sync with version V-1 and the game's own payload. It can never be applied while the
//      server is aligned with the game (V-1 == client_version-1) and is rejected when the counters have drifted.
//      A rejected sync changes nothing but sets client_version = server_version, and returns server_version (S).
//   2. if S == V nothing drifted: done.
//   3. otherwise send a zero-quantity CMD_SALE_RESOURCE, which raises server_version AND client_version
//      together without touching any resource or GMP value. Afterwards client_version == server_version == S+1,
//      so the game's real request (version V) passes the gate and is applied exactly once.

namespace resource_sync_fix
{
	enum class result
	{
		skipped,     // not a sync request / nothing to do
		aligned,     // counters already matched
		repaired,    // counters were drifted and have been realigned
		failed       // network/protocol problem, request left untouched
	};

	// send: callable(const nlohmann::json&) -> std::optional<nlohmann::json>, returns the response "data" object
	template <typename Send>
	result repair(const nlohmann::json& game_request, Send&& send, std::string* info = nullptr)
	{
		const auto note = [&](const std::string& text)
		{
			if (info != nullptr)
			{
				*info = text;
			}
		};

		if (!game_request.is_object() ||
			!game_request.contains("msgid") || !game_request["msgid"].is_string() ||
			game_request["msgid"].get<std::string>() != "CMD_SYNC_RESOURCE")
		{
			return result::skipped;
		}

		if (!game_request.contains("version") || !game_request["version"].is_number_unsigned())
		{
			return result::skipped;
		}

		const auto version = game_request["version"].get<std::uint64_t>();
		if (version == 0)
		{
			return result::skipped;
		}

		// 1. probe: same payload as the real request, but version - 1
		auto probe = game_request;
		probe["version"] = version - 1;

		const std::optional<nlohmann::json> probe_opt = send(probe);
		if (!probe_opt.has_value())
		{
			note("probe sync failed");
			return result::failed;
		}

		const nlohmann::json& probe_result = probe_opt.value();
		if (!probe_result.is_object() ||
			!probe_result.contains("result") || probe_result["result"] != "NOERR" ||
			!probe_result.contains("version") || !probe_result["version"].is_number_unsigned())
		{
			note("probe sync failed");
			return result::failed;
		}

		const auto server_version = probe_result["version"].get<std::uint64_t>();
		if (server_version == version)
		{
			note("server and client versions match");
			return result::aligned;
		}

		// 3. zero quantity sale: bumps server+client version together, changes no values
		nlohmann::json sale;
		sale["rqid"] = 0;
		sale["msgid"] = "CMD_SALE_RESOURCE";
		sale["num"] = 0;
		sale["resouce_id"] = 0; // typo is part of the protocol
		sale["unit_price"] = 0;

		const std::optional<nlohmann::json> sale_opt = send(sale);
		if (!sale_opt.has_value() || !sale_opt->is_object() ||
			!sale_opt->contains("result") || (*sale_opt)["result"] != "NOERR")
		{
			note("version realign (sale) failed");
			return result::failed;
		}

		note("server version was " + std::to_string(server_version) + ", game sent " + std::to_string(version) + ", realigned");
		return result::repaired;
	}
}
