#include "Connection.hpp"
#include "hex_dump.hpp"
#include "Game.hpp"

#include <chrono>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <unordered_map>

#ifdef _WIN32
extern "C" { uint32_t GetACP(); }
#endif

// tiny helper: try to parse our C2S_Action frame from recv_buffer
static bool try_recv_action(Connection* c, uint8_t& out_mask) {
	auto& buf = c->recv_buffer;
	if (buf.size() < 4) return false;
	if (buf[0] != uint8_t(Message::C2S_Action)) return false;
	uint32_t size = (uint32_t(buf[3]) << 16) | (uint32_t(buf[2]) << 8) | uint32_t(buf[1]);
	if (size != 1) throw std::runtime_error("C2S_Action with unexpected size");
	if (buf.size() < 4 + size) return false; // wait for full payload
	out_mask = buf[4];
	buf.erase(buf.begin(), buf.begin() + 5);
	return true;
}

int main(int argc, char **argv) {
#ifdef _WIN32
	{ //when compiled on windows, check that code page is forced to utf-8 (makes file loading/saving work right):
		uint32_t code_page = GetACP();
		if (code_page == 65001) {
			std::cout << "Code page is properly set to UTF-8." << std::endl;
		} else {
			std::cout << "WARNING: code page is set to " << code_page << " instead of 65001 (UTF-8). Some file handling functions may fail." << std::endl;
		}
	}
	try {
#endif

	if (argc != 2) {
		std::cerr << "Usage:\n\t./server <port>" << std::endl;
		return 1;
	}

	Server server(argv[1]);

	std::unordered_map< Connection *, Player * > connection_to_player;
	Game game;

	while (true) {
		static auto next_tick = std::chrono::steady_clock::now() + std::chrono::duration< double >(Game::Tick);
		while (true) {
			auto now = std::chrono::steady_clock::now();
			double remain = std::chrono::duration< double >(next_tick - now).count();
			if (remain < 0.0) {
				next_tick += std::chrono::duration< double >(Game::Tick);
				break;
			}

			auto remove_connection = [&](Connection *c) {
				auto f = connection_to_player.find(c);
				if(f != connection_to_player.end()) {
					game.remove_player(f->second);
					connection_to_player.erase(f);
				}
			};

			server.poll([&](Connection *c, Connection::Event evt){
				if (evt == Connection::OnOpen) {
					if (connection_to_player.size() >= Game::MaxPlayers) {
						std::cout << "Max players reached, disconnecting client." << std::endl;
						// optional: send 'F' frame as你之前写的
						c->close();
						return;
					}
					connection_to_player.emplace(c, game.spawn_player());

				} else if (evt == Connection::OnClose) {
					remove_connection(c);

				} else { assert(evt == Connection::OnRecv);
					auto f = connection_to_player.find(c);
					if(f == connection_to_player.end()) {
						c->close();
						return;
					}
					Player &player = *f->second;

					try {
						bool progressed;
						do {
							progressed = false;

							// existing controls:
							if (player.controls.recv_controls_message(c)) {
								progressed = true;
								// debug print for controls (only when there was a 'downs'):
								if (player.controls.left.downs || player.controls.right.downs ||
									player.controls.up.downs || player.controls.down.downs ||
									player.controls.jump.downs) {
									std::cout << "[Controls] player=" << player.name
										<< " L:" << int(player.controls.left.downs)
										<< " R:" << int(player.controls.right.downs)
										<< " U:" << int(player.controls.up.downs)
										<< " D:" << int(player.controls.down.downs)
										<< " JUMP:" << int(player.controls.jump.downs)
										<< std::endl;
								}
							}

							// new action frame:
							uint8_t mask = 0;
							while (try_recv_action(c, mask)) {
								progressed = true;
								player.pending_action |= mask; // let Game::update consume/clear it
								std::cout << "[Action] player=" << player.name
									<< " attack=" << ((mask & 0x1) ? 1 : 0)
									<< " defend=" << ((mask & 0x2) ? 1 : 0)
									<< " parry="  << ((mask & 0x4) ? 1 : 0)
									<< std::endl;
							}
						} while (progressed);
					} catch (std::exception const &e) {
						std::cout << "Disconnecting client:" << e.what() << std::endl;
						c->close();
						remove_connection(c);
					}
				}
			}, remain);
		}

		game.update(Game::Tick);

		for (auto &[c, player] : connection_to_player) {
			game.send_state_message(c, player);
		}
	}

	return 0;

#ifdef _WIN32
	} catch (std::exception const &e) {
		std::cerr << "Unhandled exception:\n" << e.what() << std::endl;
		return 1;
	} catch (...) {
		std::cerr << "Unhandled exception (unknown type)." << std::endl;
		throw;
	}
#endif
}
