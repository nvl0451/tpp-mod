#pragma once

// Header-only core of the resource sync repair (kept free of game/hook code so it can be unit tested).
//
// Server behaviour this works around (tpp-server-emulator, CMD_SYNC_RESOURCE):
//   the server only applies a client sync when  client_version == server_version
//   and  request.version != client_version - 1.
// Other server commands (e.g. CMD_MINING_RESOURCE, daily rewards) raise server_version without telling the
// client, and the game can keep an old version around, so syncs get silently rejected and the server answers
// with the OLD stored values.
//
// Before the game sends its real CMD_SYNC_RESOURCE (request version V) we send zero-quantity
// CMD_SALE_RESOURCE messages. A sale raises server_version AND client_version together (so they are equal
// afterwards), changes no resource or GMP value, and answers with the new server version S.
// The real request is applied exactly when  V != S - 1, so we repeat the sale until that holds
// (at most two sales in practice). We never send a sync ourselves, so nothing can be applied twice.

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

		nlohmann::json sale;
		sale["rqid"] = 0;
		sale["msgid"] = "CMD_SALE_RESOURCE";
		sale["num"] = 0;
		sale["resouce_id"] = 0; // typo is part of the protocol
		sale["unit_price"] = 0;

		for (auto attempt = 1; attempt <= 4; attempt++)
		{
			const std::optional<nlohmann::json> sale_opt = send(sale);
			if (!sale_opt.has_value())
			{
				note("version realign (sale) failed");
				return result::failed;
			}

			const nlohmann::json& sale_result = sale_opt.value();
			if (!sale_result.is_object() ||
				!sale_result.contains("result") || sale_result["result"] != "NOERR" ||
				!sale_result.contains("version") || !sale_result["version"].is_number_unsigned())
			{
				note("version realign (sale) failed");
				return result::failed;
			}

			const auto server_version = sale_result["version"].get<std::uint64_t>();
			if (version + 1 != server_version)
			{
				note("game version " + std::to_string(version) + ", server version now " +
					std::to_string(server_version) + " (" + std::to_string(attempt) + " realign step(s))");
				return result::repaired;
			}
		}

		note("could not realign versions");
		return result::failed;
	}
}
