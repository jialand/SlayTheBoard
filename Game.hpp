#pragma once

#include <glm/glm.hpp>

#include <string>
#include <list>
#include <random>
#include <cstdint> // ★ for uint8_t
#include <unordered_map>

struct Connection;

//Game state, separate from rendering.

//Currently set up for a "client sends controls" / "server sends whole state" situation.

enum class Message : uint8_t {
	C2S_Controls = 1, //Greg!
	S2C_State = 's',
	//...
};

//used to represent a control input:
struct Button {
	uint8_t downs = 0; //times the button has been pressed
	bool pressed = false; //is the button pressed now
};

//state of one player in the game:
struct Player {
	//player inputs (sent from client):
	struct Controls {
		Button left, right, up, down, jump;
		Button attack, guard, parry; // ★ new action buttons (J/K/L)

		void send_controls_message(Connection *connection) const;

		//returns 'false' if no message or not a controls message,
		//returns 'true' if read a controls message,
		//throws on malformed controls message
		bool recv_controls_message(Connection *connection);
	} controls;

	//player state (sent from server):
	glm::vec2 position = glm::vec2(0.0f, 0.0f);
	glm::vec2 velocity = glm::vec2(0.0f, 0.0f);

	glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
	std::string name = "";

	int gx = 0; // ★ grid X (cell index, authoritative on server)
	int gy = 0; // ★ grid Y (cell index, authoritative on server)

	int hp = 3; // ★ hit points (3 -> dead at <=0)

	enum Facing { // ★ 4-direction facing for actions
		FaceRight = 0, FaceLeft = 1, FaceUp = 2, FaceDown = 3
	} facing = FaceRight; // ★ default will be set at spawn
};

struct Game {
	static constexpr size_t MaxPlayers = 2;
	std::list< Player > players; //(using list so they can have stable addresses)
	Player *spawn_player(); //add player the end of the players list (may also, e.g., play some spawn anim)
	void remove_player(Player *); //remove player from game (may also, e.g., play some despawn anim)

	std::mt19937 mt; //used for spawning players
	uint32_t next_player_number = 1; //used for naming players

	Game();

	//state update function:
	void update(float elapsed);

	//constants:
	//the update rate on the server:
	inline static constexpr float Tick = 1.0f / 30.0f;

	//arena size:
	inline static constexpr glm::vec2 ArenaMin = glm::vec2(-1.0f, -1.0f);
	inline static constexpr glm::vec2 ArenaMax = glm::vec2( 1.0f,  1.0f);

	//player constants:
	inline static constexpr float PlayerRadius = 0.06f;
	inline static constexpr float PlayerSpeed = 2.0f;
	inline static constexpr float PlayerAccelHalflife = 0.25f;
	
	// ★ grid/board settings for 4x4 snap movement:
	inline static constexpr int BoardW = 4;      // number of columns
	inline static constexpr int BoardH = 4;      // number of rows
	inline static constexpr float CellSize = 0.5f; // each cell size in world units (2.0 span / 4 = 0.5)

	// ★ helper: convert grid cell to world center position:
	static inline glm::vec2 cell_center(int gx, int gy) {
		return glm::vec2(
			ArenaMin.x + (gx + 0.5f) * CellSize,
			ArenaMin.y + (gy + 0.5f) * CellSize
		);
	}

	// ★ action timing (seconds):
	inline static constexpr float AttackCD = 2.0f;
	inline static constexpr float GuardCD  = 3.0f;
	inline static constexpr float ParryCD  = 5.0f;
	inline static constexpr float GuardWindow = 0.5f;
	inline static constexpr float ParryWindow = 0.5f;

	// ★ per-player action state (cooldowns/timers):
	struct ActionState {
		float attack_cd = 0.0f;
		float guard_cd  = 0.0f;
		float parry_cd  = 0.0f;
		float guard_t   = 0.0f; // active guard window remaining
		float parry_t   = 0.0f; // active parry window remaining
	};
	std::unordered_map<Player*, ActionState> pstates; // ★ track per-player action state

	//---- communication helpers ----

	//used by client:
	//set game state from data in connection buffer
	// (return true if data was read)
	bool recv_state_message(Connection *connection);

	//used by server:
	//send game state.
	//  Will move "connection_player" to the front of the front of the sent list.
	void send_state_message(Connection *connection, Player *connection_player = nullptr) const;
};
