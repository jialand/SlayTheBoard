#include "Game.hpp"

#include "Connection.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <algorithm> // for std::clamp
#include <unordered_map> // ★ pstate map

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

void Player::Controls::send_controls_message(Connection *connection_) const {
	assert(connection_);
	auto &connection = *connection_;

	uint32_t size = 8; // ★ 8 buttons now
	connection.send(Message::C2S_Controls);
	connection.send(uint8_t(size));
	connection.send(uint8_t(size >> 8));
	connection.send(uint8_t(size >> 16));

	auto send_button = [&](Button const &b) {
		if (b.downs & 0x80) {
			std::cerr << "Wow, you are really good at pressing buttons!" << std::endl;
		}
		connection.send(uint8_t( (b.pressed ? 0x80 : 0x00) | (b.downs & 0x7f) ) );
	};

	send_button(left);
	send_button(right);
	send_button(up);
	send_button(down);
	send_button(jump);
	send_button(attack); // ★
	send_button(guard);  // ★
	send_button(parry);  // ★
}

bool Player::Controls::recv_controls_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;

	auto &recv_buffer = connection.recv_buffer;

	//expecting [type, size_low0, size_mid8, size_high8]:
	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::C2S_Controls)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	if (size != 8) throw std::runtime_error("Controls message with size " + std::to_string(size) + " != 8!"); // ★
	
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	auto recv_button = [](uint8_t byte, Button *button) {
		button->pressed = (byte & 0x80);
		uint32_t d = uint32_t(button->downs) + uint32_t(byte & 0x7f);
		if (d > 255) {
			std::cerr << "got a whole lot of downs" << std::endl;
			d = 255;
		}
		button->downs = uint8_t(d);
	};

	recv_button(recv_buffer[4+0], &left);
	recv_button(recv_buffer[4+1], &right);
	recv_button(recv_buffer[4+2], &up);
	recv_button(recv_buffer[4+3], &down);
	recv_button(recv_buffer[4+4], &jump);
	recv_button(recv_buffer[4+5], &attack); // ★
	recv_button(recv_buffer[4+6], &guard);  // ★
	recv_button(recv_buffer[4+7], &parry);  // ★

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}


//-----------------------------------------

Game::Game() : mt(0x15466666) {
}

static inline glm::ivec2 facing_vec(Player::Facing f) { // ★ map facing to dx,dy
	switch (f) {
		case Player::FaceRight: return {+1, 0};
		case Player::FaceLeft:  return {-1, 0};
		case Player::FaceUp:    return { 0,+1};
		default:                return { 0,-1}; // FaceDown
	}
}

static Player* get_player_at(std::list<Player> &ps, int gx, int gy) { // ★ find occupant
	for (auto &p : ps) if (p.gx == gx && p.gy == gy) return &p;
	return nullptr;
}

Player *Game::spawn_player() {
	players.emplace_back();
	Player &player = players.back();

	// spawn at corners and set default facing:
	//   first -> top-left (face right), second -> bottom-right (face left)
	size_t idx = 0;
	for (auto pi = players.begin(); pi != players.end(); ++pi) { if (&*pi == &player) break; ++idx; }
	if (idx == 0) {
		player.gx = 0;
		player.gy = BoardH - 1;
		player.facing = Player::FaceRight; // ★ default facing
	} else if (idx == 1) {
		player.gx = BoardW - 1;
		player.gy = 0;
		player.facing = Player::FaceLeft;  // ★ default facing
	} else {
		player.gx = 0;
		player.gy = 0;
		player.facing = Player::FaceRight;
	}
	player.position = cell_center(player.gx, player.gy);
	player.velocity = glm::vec2(0.0f);
	player.hp = 3; // ★ reset hp

	// keep color/name logic
	do {
		player.color.r = mt() / float(mt.max());
		player.color.g = mt() / float(mt.max());
		player.color.b = mt() / float(mt.max());
	} while (player.color == glm::vec3(0.0f));
	player.color = glm::normalize(player.color);

	player.name = "Player " + std::to_string(next_player_number++);

	// init action state entry (cooldowns/timers zero)
	pstates[&player] = ActionState{}; // ★

	return &player;
}

void Game::remove_player(Player *player) {
	bool found = false;
	for (auto pi = players.begin(); pi != players.end(); ++pi) {
		if (&*pi == player) {
			pstates.erase(&*pi); // ★ drop action state
			players.erase(pi);
			found = true;
			break;
		}
	}
	assert(found);
}

// optional: disallow walking into occupied cell
static bool cell_occupied(std::list<Player> const &players, int gx, int gy, Player const *self) {
	for (auto const &q : players) {
		if (&q == self) continue;
		if (q.gx == gx && q.gy == gy) return true;
	}
	return false;
}

// ★ helper: apply 1 damage with block/parry logic; returns true if damage applied
static bool try_apply_hit(Game &game, Player *attacker, Player *victim, int src_gx, int src_gy) {
	if (!attacker || !victim) return false;
	if (victim->hp <= 0) return false;

	auto it = game.pstates.find(victim);
	if (it == game.pstates.end()) return false;
	auto &vs = it->second;

	// Is the attack coming from the victim's facing direction (front)?
	glm::ivec2 vf = facing_vec(victim->facing);
	bool from_front = (src_gx == victim->gx + vf.x) && (src_gy == victim->gy + vf.y);

	// Parry window first: if active and from front -> negate and reflect 1 damage
	if (vs.parry_t > 0.0f && from_front) {
		// reflect to source cell (one cell in victim's facing = the attacker cell)
		Player *src = get_player_at(game.players, src_gx, src_gy);
		if (src && src->hp > 0) {
			src->hp = std::max(0, src->hp - 1);
			if (src->hp == 0) {
				std::cout << victim->name << " wins by parry!" << std::endl;
			}
		}
		return false; // victim takes no damage
	}

	// Guard window next: if active and from front -> negate damage
	if (vs.guard_t > 0.0f && from_front) {
		return false;
	}

	// Otherwise, apply damage
	victim->hp = std::max(0, victim->hp - 1);
	if (victim->hp == 0) {
		std::cout << (attacker ? attacker->name : std::string("Unknown"))
		          << " wins!" << std::endl;
	}
	return true;
}

void Game::update(float elapsed) {
	// decrement cooldowns/timers
	for (auto &p : players) {
		auto &s = pstates[&p];
		s.attack_cd = std::max(0.0f, s.attack_cd - elapsed);
		s.guard_cd  = std::max(0.0f, s.guard_cd  - elapsed);
		s.parry_cd  = std::max(0.0f, s.parry_cd  - elapsed);
		s.guard_t   = std::max(0.0f, s.guard_t   - elapsed);
		s.parry_t   = std::max(0.0f, s.parry_t   - elapsed);
	}

	// grid-snap movement (consume downs; set facing to last move)
	for (auto &p : players) {
		int nx = p.gx;
		int ny = p.gy;

		auto step = [&](int dx, int dy, uint8_t &downs, Player::Facing face_dir) {
			while (downs > 0) {
				int tx = std::clamp(nx + dx, 0, BoardW - 1);
				int ty = std::clamp(ny + dy, 0, BoardH - 1);
				if (!cell_occupied(players, tx, ty, &p)) {
					nx = tx; ny = ty;
					p.facing = face_dir; // ★ face to the direction of movement
				}
				downs -= 1;
			}
		};

		step(-1,  0, p.controls.left.downs,  Player::FaceLeft);
		step( 1,  0, p.controls.right.downs, Player::FaceRight);
		step( 0,  1, p.controls.up.downs,    Player::FaceUp);
		step( 0, -1, p.controls.down.downs,  Player::FaceDown);

		p.gx = nx; p.gy = ny;
		p.position = cell_center(p.gx, p.gy);
		p.velocity = glm::vec2(0.0f);
	}

	// actions: attack/guard/parry
	for (auto &p : players) {
		auto &s = pstates[&p];
		// ATTACK: immediate 1 dmg to the cell in front; 2s cooldown
		if (p.controls.attack.downs > 0 && s.attack_cd <= 0.0f) {
			glm::ivec2 af = facing_vec(p.facing);
			int tx = std::clamp(p.gx + af.x, 0, BoardW - 1);
			int ty = std::clamp(p.gy + af.y, 0, BoardH - 1);
			Player *victim = get_player_at(players, tx, ty);
			if (victim) {
				try_apply_hit(*this, &p, victim, p.gx, p.gy);
			}
			s.attack_cd = AttackCD;
		}

		// GUARD: enable front block for 0.5s; 3s cooldown
		if (p.controls.guard.downs > 0 && s.guard_cd <= 0.0f) {
			s.guard_t = GuardWindow;
			s.guard_cd = GuardCD;
		}

		// PARRY: enable front parry for 0.5s; 5s cooldown
		if (p.controls.parry.downs > 0 && s.parry_cd <= 0.0f) {
			s.parry_t = ParryWindow;
			s.parry_cd = ParryCD;
		}

		// clear action downs after consumption
		p.controls.attack.downs = 0;
		p.controls.guard.downs = 0;
		p.controls.parry.downs = 0;

		// also clear pressed flags to avoid client “held” drift (optional)
		p.controls.left.pressed = false;
		p.controls.right.pressed = false;
		p.controls.up.pressed = false;
		p.controls.down.pressed = false;
		p.controls.jump.pressed = false;
		p.controls.attack.pressed = false;
		p.controls.guard.pressed = false;
		p.controls.parry.pressed = false;
	}

	// clamp inside arena (safety)
	for (auto &p1 : players) {
		if (p1.position.x < ArenaMin.x + PlayerRadius) p1.position.x = ArenaMin.x + PlayerRadius;
		if (p1.position.x > ArenaMax.x - PlayerRadius) p1.position.x = ArenaMax.x - PlayerRadius;
		if (p1.position.y < ArenaMin.y + PlayerRadius) p1.position.y = ArenaMin.y + PlayerRadius;
		if (p1.position.y > ArenaMax.y - PlayerRadius) p1.position.y = ArenaMax.y - PlayerRadius;
	}
}


void Game::send_state_message(Connection *connection_, Player *connection_player) const {
	assert(connection_);
	auto &connection = *connection_;

	connection.send(Message::S2C_State);
	//will patch message size in later, for now placeholder bytes:
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	size_t mark = connection.send_buffer.size(); //keep track of this position in the buffer


	//send player info helper:
	auto send_player = [&](Player const &player) {
		connection.send(player.position);
		connection.send(player.velocity);
		connection.send(player.color);
	
		//NOTE: can't just 'send(name)' because player.name is not plain-old-data type.
		//effectively: truncates player name to 255 chars
		uint8_t len = uint8_t(std::min< size_t >(255, player.name.size()));
		connection.send(len);
		connection.send_buffer.insert(connection.send_buffer.end(), player.name.begin(), player.name.begin() + len);
	};

	//player count:
	connection.send(uint8_t(players.size()));
	if (connection_player) send_player(*connection_player);
	for (auto const &player : players) {
		if (&player == connection_player) continue;
		send_player(player);
	}

	//compute the message size and patch into the message header:
	uint32_t size = uint32_t(connection.send_buffer.size() - mark);
	connection.send_buffer[mark-3] = uint8_t(size);
	connection.send_buffer[mark-2] = uint8_t(size >> 8);
	connection.send_buffer[mark-1] = uint8_t(size >> 16);
}

bool Game::recv_state_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;
	auto &recv_buffer = connection.recv_buffer;

	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::S2C_State)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	uint32_t at = 0;
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	//copy bytes from buffer and advance position:
	auto read = [&](auto *val) {
		if (at + sizeof(*val) > size) {
			throw std::runtime_error("Ran out of bytes reading state message.");
		}
		std::memcpy(val, &recv_buffer[4 + at], sizeof(*val));
		at += sizeof(*val);
	};

	players.clear();
	uint8_t player_count;
	read(&player_count);
	for (uint8_t i = 0; i < player_count; ++i) {
		players.emplace_back();
		Player &player = players.back();
		read(&player.position);
		read(&player.velocity);
		read(&player.color);
		uint8_t name_len;
		read(&name_len);
		//n.b. would probably be more efficient to directly copy from recv_buffer, but I think this is clearer:
		player.name = "";
		for (uint8_t n = 0; n < name_len; ++n) {
			char c;
			read(&c);
			player.name += c;
		}
	}

	if (at != size) throw std::runtime_error("Trailing data in state message.");

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}
