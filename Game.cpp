//@ChatGPT used
#include "Game.hpp"
#include "Connection.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_set>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

// ---------- wire I/O for controls (unchanged) ----------

void Player::Controls::send_controls_message(Connection *connection_) const {
	assert(connection_);
	auto &connection = *connection_;

	uint32_t size = 5;
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
}

bool Player::Controls::recv_controls_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;

	auto &recv_buffer = connection.recv_buffer;

	// expecting [type, size_low0, size_mid8, size_high8]:
	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::C2S_Controls)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	if (size != 5) throw std::runtime_error("Controls message with size " + std::to_string(size) + " != 5!");

	// need complete message:
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

	// delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}

// ---------- Game core ----------

Game::Game() : mt(0x15466666) {
}

// convert grid cell -> world-space center
glm::vec2 Game::cell_to_world(glm::ivec2 cell) {
	glm::vec2 min = ArenaMin;
	glm::vec2 max = ArenaMax;
	glm::vec2 cell_size = (max - min) / float(GridN);
	glm::vec2 center = min + (glm::vec2(cell) + glm::vec2(0.5f)) * cell_size;
	return center;
}

Player *Game::spawn_player() {
	players.emplace_back();
	Player &player = players.back();

	// color
	do {
		player.color.r = mt() / float(mt.max());
		player.color.g = mt() / float(mt.max());
		player.color.b = mt() / float(mt.max());
	} while (player.color == glm::vec3(0.0f));
	player.color = glm::normalize(player.color);

	player.name = "Player " + std::to_string(next_player_number++);

	player.ready = false;
	player.hp = 3;
	player.pending_action = 0;

	// initial spawn and facing
	if (players.size() == 1) {
		player.cell = glm::ivec2(0, GridN - 1);
		player.facing = glm::ivec2(1, 0);
	} else {
		player.cell = glm::ivec2(GridN - 1, 0);
		player.facing = glm::ivec2(-1, 0);
	}

	player.position = cell_to_world(player.cell);
	player.velocity = glm::vec2(0.0f);

	// ensure runtime slot
	pstates[&player] = PerPlayerRuntime{};

	return &player;
}

void Game::remove_player(Player *player) {
	bool found = false;
	for (auto pi = players.begin(); pi != players.end(); ++pi) {
		if (&*pi == player) {
			pstates.erase(&*pi);
			players.erase(pi);
			found = true;
			break;
		}
	}
	assert(found);
}

void Game::update(float elapsed) {
	// sync pstates with current players
	std::unordered_set<Player*> alive;
	for (auto &p : players) {
		alive.insert(&p);
		if (!pstates.count(&p)) pstates[&p] = PerPlayerRuntime{};
	}
	for (auto it = pstates.begin(); it != pstates.end(); ) {
		if (!alive.count(it->first)) it = pstates.erase(it);
		else ++it;
	}

	// decay timers
	for (auto &kv : pstates) {
		auto &rt = kv.second;
		rt.atk_cd  = std::max(0.0f, rt.atk_cd  - elapsed);
		rt.def_cd  = std::max(0.0f, rt.def_cd  - elapsed);
		rt.pry_cd  = std::max(0.0f, rt.pry_cd  - elapsed);
		rt.defend_t= std::max(0.0f, rt.defend_t- elapsed);
		rt.parry_t = std::max(0.0f, rt.parry_t - elapsed);
	}

	// --- phase management ---
	if (players.size() < 2) {
		phase = Phase::Waiting;
		winner_index = -1;
		for (auto &p : players) p.ready = false;
	}

	if (players.size() >= 2 && (phase == Phase::Waiting)) {
		phase = Phase::ReadyPrompt;
		winner_index = -1;
		for (auto &p : players) { p.ready = false; p.hp = 3; }
		// reset spawn
		auto it = players.begin();
		if (it != players.end()) {
			it->cell = glm::ivec2(0, GridN - 1);
			it->facing = glm::ivec2(1,0);
			it->position = cell_to_world(it->cell);
		}
		if (it != players.end()) {
			++it;
			if (it != players.end()) {
				it->cell = glm::ivec2(GridN - 1, 0);
				it->facing = glm::ivec2(-1,0);
				it->position = cell_to_world(it->cell);
			}
		}
	}

	// Enter to ready (mapped from jump.downs)
	if (phase == Phase::ReadyPrompt) {
		for (auto &p : players) {
			if (p.controls.jump.downs > 0) p.ready = true;
		}
		if (players.size() >= 2) {
			auto it = players.begin();
			Player &p0 = *it++;
			Player &p1 = *it;
			if (p0.ready && p1.ready) {
				phase = Phase::Playing;
				winner_index = -1;
				p0.hp = 3; p1.hp = 3;
				// snap positions to spawn
				p0.cell = glm::ivec2(0, GridN - 1); p0.facing = glm::ivec2(1,0);
				p1.cell = glm::ivec2(GridN - 1, 0); p1.facing = glm::ivec2(-1,0);
				p0.position = cell_to_world(p0.cell);
				p1.position = cell_to_world(p1.cell);
				// clear combat runtime
				pstates[&p0] = PerPlayerRuntime{};
				pstates[&p1] = PerPlayerRuntime{};
			}
		}
	}

	// --- grid movement: one step per key down, clamped to board, no overlap ---
	if (phase == Phase::Playing && players.size() >= 2) {
		auto it = players.begin();
		Player &p0 = *it++;
		Player &p1 = *it;

		auto try_move = [&](Player &p, const glm::ivec2 &delta, const Player &other) {
			if (delta == glm::ivec2(0)) return;
			glm::ivec2 tgt = p.cell + delta;
			tgt.x = std::clamp(tgt.x, 0, GridN - 1);
			tgt.y = std::clamp(tgt.y, 0, GridN - 1);
			// block if the target cell is occupied by the other player
			if (tgt == other.cell) {
				// still update facing even if blocked
				if (delta.x != 0 || delta.y != 0) p.facing = glm::ivec2((delta.x!=0)?(delta.x>0?1:-1):0, (delta.y!=0)?(delta.y>0?1:-1):0);
				return;
			}
			// commit move
			p.cell = tgt;
			if (delta.x != 0 || delta.y != 0) p.facing = glm::ivec2((delta.x!=0)?(delta.x>0?1:-1):0, (delta.y!=0)?(delta.y>0?1:-1):0);
		};

		// consume one-step inputs
		glm::ivec2 d0(0), d1(0);
		if (p0.controls.left.downs  > 0) d0.x -= 1;
		if (p0.controls.right.downs > 0) d0.x += 1;
		if (p0.controls.down.downs  > 0) d0.y -= 1;
		if (p0.controls.up.downs    > 0) d0.y += 1;

		if (p1.controls.left.downs  > 0) d1.x -= 1;
		if (p1.controls.right.downs > 0) d1.x += 1;
		if (p1.controls.down.downs  > 0) d1.y -= 1;
		if (p1.controls.up.downs    > 0) d1.y += 1;

		try_move(p0, d0, p1);
		try_move(p1, d1, p0);

		// snap world positions
		p0.position = cell_to_world(p0.cell);
		p1.position = cell_to_world(p1.cell);
		p0.velocity = glm::vec2(0.0f);
		p1.velocity = glm::vec2(0.0f);

		// ---------- combat ----------
		auto &r0 = pstates[&p0];
		auto &r1 = pstates[&p1];

		// 1) arm defend/parry windows first (so same-tick defense works)
		if ((p0.pending_action & Action_Defend) && r0.def_cd <= 0.0f) {
			r0.defend_t = GuardWindow;
			r0.def_cd = DefendCooldown;
		}
		if ((p0.pending_action & Action_Parry) && r0.pry_cd <= 0.0f) {
			r0.parry_t = GuardWindow;
			r0.pry_cd = ParryCooldown;
		}
		if ((p1.pending_action & Action_Defend) && r1.def_cd <= 0.0f) {
			r1.defend_t = GuardWindow;
			r1.def_cd = DefendCooldown;
		}
		if ((p1.pending_action & Action_Parry) && r1.pry_cd <= 0.0f) {
			r1.parry_t = GuardWindow;
			r1.pry_cd = ParryCooldown;
		}

		// helper: check if defender faces attacker (for blocking direction)
		auto faces_attacker = [](const Player &defender, const Player &attacker)->bool {
			glm::ivec2 dir = attacker.cell - defender.cell; // from defender to attacker
			return dir == defender.facing;
		};

		// 2) resolve attacks (check target cell = cell + facing)
		auto try_attack = [&](Player &attacker, PerPlayerRuntime &ra, Player &defender, PerPlayerRuntime &rd) {
			if (!(attacker.pending_action & Action_Attack)) return;
			if (ra.atk_cd > 0.0f) return;

			glm::ivec2 target = attacker.cell + attacker.facing;
			if (target == defender.cell) {
				bool block_dir = faces_attacker(defender, attacker);

				bool parried = (rd.parry_t > 0.0f) && block_dir;
				bool defended = (rd.defend_t > 0.0f) && block_dir;

				if (parried) {
					// defender parries: attacker takes 1 damage
					if (attacker.hp > 0) attacker.hp -= 1;
					// [ADD] server-side debug:
					std::cout << "[Combat] PARry  | " << defender.name << " parried " << attacker.name
					          << "  => " << attacker.name << " HP=" << int(attacker.hp) << std::endl;
				} else if (defended) {
					// blocked: no damage
					// [ADD] server-side debug:
					std::cout << "[Combat] BLOCK  | " << defender.name << " blocked " << attacker.name
					          << "  => " << defender.name << " HP=" << int(defender.hp) << std::endl;
				} else {
					// hit: defender takes 1 damage
					if (defender.hp > 0) defender.hp -= 1;
					// [ADD] server-side debug:
					std::cout << "[Combat] HIT    | " << attacker.name << " hit " << defender.name
					          << "  => " << defender.name << " HP=" << int(defender.hp) << std::endl;
				}
			}
			ra.atk_cd = AttackCooldown;
		};

		// attacks (order does not matter because damage is immediate and we don't remove players mid-frame)
		try_attack(p0, r0, p1, r1);
		try_attack(p1, r1, p0, r0);
	}

	// reset 'downs' since controls have been handled; clear this-tick actions
	for (auto &p : players) {
		p.controls.left.downs = 0;
		p.controls.right.downs = 0;
		p.controls.up.downs = 0;
		p.controls.down.downs = 0;
		p.controls.jump.downs = 0;
		p.pending_action = 0;
	}

	// round end check
	if (phase == Phase::Playing && players.size() >= 2) {
		auto it = players.begin();
		const Player &p0 = *it++;
		const Player &p1 = *it;
		if (p0.hp == 0 || p1.hp == 0) {
			phase = Phase::RoundEnd;
			winner_index = (p0.hp > p1.hp) ? 0 : 1;
			game_over_timer = 0.0f;
			const_cast<Player&>(p0).ready = false;
			const_cast<Player&>(p1).ready = false;
			// clear windows so不会残留到下一局
			pstates.at(const_cast<Player*>(&p0)) = PerPlayerRuntime{};
			pstates.at(const_cast<Player*>(&p1)) = PerPlayerRuntime{};
		}
	}

	if (phase == Phase::RoundEnd && players.size() >= 2) {
		game_over_timer += elapsed;
		if (game_over_timer >= 5.0f) {
			// move back to ReadyPrompt (ready room)
			phase = Phase::ReadyPrompt;
			winner_index = -1;     // clear winner for the new round
			game_over_timer = 0.0f;
	
			// reset players: ready=false, hp restored, snap to spawns & facing
			auto it_round = players.begin();          // <- renamed to avoid shadowing
			Player &pl_a = *it_round++;               // <- renamed (was p0)
			Player &pl_b = *it_round;                 // <- renamed (was p1)
	
			pl_a.ready = false; pl_b.ready = false;
			pl_a.hp = 3;        pl_b.hp = 3;
	
			pl_a.cell = glm::ivec2(0, GridN - 1);  pl_a.facing = glm::ivec2(1, 0);
			pl_b.cell = glm::ivec2(GridN - 1, 0);  pl_b.facing = glm::ivec2(-1, 0);
	
			pl_a.position = cell_to_world(pl_a.cell);
			pl_b.position = cell_to_world(pl_b.cell);
			pl_a.velocity = glm::vec2(0.0f);
			pl_b.velocity = glm::vec2(0.0f);
	
			// clear per-player runtime windows & cooldowns (no carry-over)
			pstates[&pl_a] = PerPlayerRuntime{};
			pstates[&pl_b] = PerPlayerRuntime{};
		}
	}
}

// ---------- S2C state (phase/winner + per-player hp/ready + POD) ----------

void Game::send_state_message(Connection *connection_, Player *connection_player) const {
	assert(connection_);
	auto &connection = *connection_;

	connection.send(Message::S2C_State);
	// placeholder size (3 bytes)
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	size_t mark = connection.send_buffer.size();

	// write phase + winner_index first:
	int8_t winner_for_conn = -1;
	if (winner_index >= 0) {
		// identify p0/p1 in server list:
		const Player* p0 = nullptr;
		const Player* p1 = nullptr;
		auto it = players.begin();
		if (it != players.end()) { p0 = &*it++; }
		if (it != players.end()) { p1 = &*it; }

		const Player* server_winner = (winner_index == 0 ? p0 : p1);
		if (connection_player && server_winner && p0 && p1) {
			// map to perspective: 0 = you, 1 = opponent
			winner_for_conn = (server_winner == connection_player) ? 0 : 1;
		} else {
			// fallback (e.g., unexpected states)
			winner_for_conn = winner_index;
		}
	}

	connection.send(uint8_t(phase));
	connection.send(int8_t(winner_for_conn));

	// helper to send a player:
	auto send_player = [&](Player const &player) {
		connection.send(player.position);
		connection.send(player.velocity);
		connection.send(player.color);
		uint8_t len = uint8_t(std::min< size_t >(255, player.name.size()));
		connection.send(len);
		connection.send_buffer.insert(connection.send_buffer.end(), player.name.begin(), player.name.begin() + len);
		// ready + hp
		connection.send(uint8_t(player.ready ? 1 : 0));
		connection.send(uint8_t(player.hp));
	};

	// player count (send connection's player first)
	connection.send(uint8_t(players.size()));
	if (connection_player) send_player(*connection_player);
	for (auto const &player : players) {
		if (&player == connection_player) continue;
		send_player(player);
	}

	// patch size
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
	if (recv_buffer.size() < 4 + size) return false;

	auto read = [&](auto *val) {
		if (at + sizeof(*val) > size) throw std::runtime_error("Ran out of bytes reading state message.");
		std::memcpy(val, &recv_buffer[4 + at], sizeof(*val));
		at += sizeof(*val);
	};

	// read phase + winner_index
	uint8_t phase_u8 = 0;
	int8_t win_i8 = -1;
	read(&phase_u8);
	read(&win_i8);
	phase = Phase(phase_u8);
	winner_index = win_i8;

	players.clear();
	uint8_t player_count = 0;
	read(&player_count);
	for (uint8_t i = 0; i < player_count; ++i) {
		players.emplace_back();
		Player &player = players.back();
		read(&player.position);
		read(&player.velocity);
		read(&player.color);
		uint8_t name_len = 0;
		read(&name_len);
		player.name.clear();
		for (uint8_t n = 0; n < name_len; ++n) {
			char c;
			read(&c);
			player.name += c;
		}
		uint8_t ready_u8 = 0;
		uint8_t hp_u8 = 3;
		read(&ready_u8);
		read(&hp_u8);
		player.ready = (ready_u8 != 0);
		player.hp = hp_u8;
	}

	if (at != size) throw std::runtime_error("Trailing data in state message.");

	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);
	return true;
}
