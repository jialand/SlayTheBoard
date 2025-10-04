#pragma once

#include <glm/glm.hpp>

#include <string>
#include <list>
#include <random>
#include <cstdint>
#include <unordered_map>

struct Connection;

// ---- wire message types ----
enum class Message : uint8_t {
	C2S_Controls = 1,    // 5-byte controls
	S2C_State    = 's',  // server -> client state
	C2S_Action   = 'a',  // client -> server action bitmask (bit0=attack, bit1=defend, bit2=parry)
};

// ---- high-level phase for client UI ----
enum class Phase : uint8_t {
	Waiting = 0,     // < 2 players
	ReadyPrompt = 1, // 2 players present; waiting for ready
	Playing = 2,     // in game
	RoundEnd = 3     // round over
};

// ---- action bits (for pending_action) ----
enum ActionBits : uint8_t {
	Action_Attack = 1 << 0,
	Action_Defend = 1 << 1,
	Action_Parry  = 1 << 2
};

// ---- input button ----
struct Button {
	uint8_t downs = 0;   // number of press events since last tick
	bool pressed = false;// pressed state
};

// ---- per-player state ----
struct Player {
	// client -> server controls
	struct Controls {
		Button left, right, up, down, jump;
		void send_controls_message(Connection *connection) const;
		bool recv_controls_message(Connection *connection);
	} controls;

	// server-side: bitmask of ActionBits to be consumed in update()
	uint8_t pending_action = 0;

	// gameplay state (server authoritative; sent to clients)
	bool ready = false;
	uint8_t hp = 3;

	// grid movement state (server-only authoring; position is derived)
	glm::ivec2 cell = glm::ivec2(0);
	glm::ivec2 facing = glm::ivec2(1,0); // (±1,0) or (0,±1)

	// preserved fields for network compatibility:
	glm::vec2 position = glm::vec2(0.0f, 0.0f); // derived from cell each tick
	glm::vec2 velocity = glm::vec2(0.0f, 0.0f); // kept for wire compatibility (unused)
	glm::vec3 color    = glm::vec3(1.0f, 1.0f, 1.0f);
	std::string name   = "";
};

struct Game {
	static constexpr size_t MaxPlayers = 2;
	float game_over_timer = 0.0f;

	// grid size:
	inline static constexpr int GridN = 4;

	std::list< Player > players; //(list so addresses remain stable)
	Player *spawn_player();      // add a player; returns pointer
	void remove_player(Player *);// remove a player

	std::mt19937 mt;
	uint32_t next_player_number = 1;

	// UI phase
	Phase phase = Phase::Waiting;
	int8_t winner_index = -1; // -1 = none; 0/1 = who won (server reorders in send_state)

	Game();

	// server tick:
	void update(float elapsed);

	// constants:
	inline static constexpr float Tick = 1.0f / 30.0f;
	inline static constexpr glm::vec2 ArenaMin = glm::vec2(-1.0f, -1.0f);
	inline static constexpr glm::vec2 ArenaMax = glm::vec2( 1.0f,  1.0f);

	inline static constexpr float PlayerRadius = 0.06f;
	inline static constexpr float PlayerSpeed = 2.0f;            // kept (unused)
	inline static constexpr float PlayerAccelHalflife = 0.25f;   // kept (unused)

	// ---- networking helpers ----
	bool recv_state_message(Connection *connection); // client
	void send_state_message(Connection *connection, Player *connection_player = nullptr) const; // server

private:
	// convert grid cell -> world center
	static glm::vec2 cell_to_world(glm::ivec2 cell);

	// combat timing
	inline static constexpr float AttackCooldown = 2.0f;
	inline static constexpr float DefendCooldown = 3.0f;
	inline static constexpr float ParryCooldown  = 5.0f;
	inline static constexpr float GuardWindow    = 0.5f;

	// per-player runtime (cooldowns + active windows)
	struct PerPlayerRuntime {
		float atk_cd = 0.0f;
		float def_cd = 0.0f;
		float pry_cd = 0.0f;
		float defend_t = 0.0f; // >0 means defend window active
		float parry_t  = 0.0f; // >0 means parry window active
	};

	std::unordered_map< Player*, PerPlayerRuntime > pstates;
};
