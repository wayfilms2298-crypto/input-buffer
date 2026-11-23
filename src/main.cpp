// Input Buffer — a Geode mod for Geometry Dash
//
// The idea: if you tap a little too early (press AND release before you land
// or reach an orb), the game normally ignores it, because GD only jumps while
// the button is *held* at the moment you land / touch the orb.
//
// This mod fixes that by keeping a short tap "virtually held" for a few extra
// milliseconds. If you land or touch an orb in that window, the jump happens.
// As soon as the buffered input does something (or the window runs out), the
// mod sends the real release to the game.
//
// It never adds presses you didn't make, and it never extends a tap that
// already did something (so it won't cause accidental double jumps).

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

namespace {
	constexpr int JUMP_BUTTON = 1; // 1 = jump, 2 = left, 3 = right (platformer)

	struct BufferState {
		bool realHeld = false;      // the player is physically holding the button
		bool virtualHold = false;   // we are holding it for them after a release
		bool consumed = false;      // the current press already caused an action
		float pressAge = 0.f;       // seconds since the press started
		float remaining = 0.f;      // seconds of virtual hold left
		bool lastOnGround = false;  // ground state on the previous frame

		void reset() { *this = BufferState{}; }
	};

	template <class T>
	T setting(char const* key) {
		return Mod::get()->getSettingValue<T>(key);
	}

	bool gamemodeAllowed(PlayerObject* p) {
		if (!p) return false;
		// Flying modes: holding longer changes physics, so never buffer them.
		if (p->m_isShip || p->m_isDart || p->m_isSwing) return false;
		if (p->m_isBird)   return setting<bool>("ufo");
		if (p->m_isBall)   return setting<bool>("ball");
		if (p->m_isRobot)  return setting<bool>("robot");
		if (p->m_isSpider) return setting<bool>("spider");
		return setting<bool>("cube");
	}

	bool practiceAllows() {
		if (!setting<bool>("in-practice-only")) return true;
		auto pl = PlayLayer::get();
		return pl && pl->m_isPracticeMode;
	}
}

class $modify(BufferedLayer, GJBaseGameLayer) {
	struct Fields {
		BufferState p1;
		BufferState p2;
		bool sendingOurOwn = false; // guards against re-entering our own hook
	};

	BufferState& stateFor(bool isPlayer1) {
		return isPlayer1 ? m_fields->p1 : m_fields->p2;
	}

	PlayerObject* playerFor(bool isPlayer1) {
		return isPlayer1 ? m_player1 : m_player2;
	}

	void sendRelease(bool isPlayer1) {
		m_fields->sendingOurOwn = true;
		GJBaseGameLayer::handleButton(false, JUMP_BUTTON, isPlayer1);
		m_fields->sendingOurOwn = false;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (m_fields->sendingOurOwn || button != JUMP_BUTTON || !setting<bool>("enabled")) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		auto& s = stateFor(isPlayer1);
		auto player = playerFor(isPlayer1);

		if (down) {
			// A new real press while we're still virtually holding: the game
			// already thinks the button is down, so just hand control back.
			bool wasVirtual = s.virtualHold;
			s.realHeld = true;
			s.virtualHold = false;
			s.consumed = false;
			s.pressAge = 0.f;
			s.lastOnGround = player && player->m_isOnGround;
			if (wasVirtual) return;
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		// Real release.
		s.realHeld = false;
		float window = setting<int64_t>("buffer-ms") / 1000.f;
		float left = window - s.pressAge;

		bool shouldBuffer =
			!s.consumed && left > 0.f &&
			gamemodeAllowed(player) && practiceAllows();

		if (shouldBuffer) {
			s.virtualHold = true;
			s.remaining = left;
			return; // swallow the release for now; update() sends it later
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void tickPlayer(bool isPlayer1, float dt) {
		auto& s = stateFor(isPlayer1);
		auto player = playerFor(isPlayer1);
		if (!player) return;

		bool onGround = player->m_isOnGround;
		// Leaving the ground while the button is down = a jump/flip happened.
		if ((s.realHeld || s.virtualHold) && s.lastOnGround && !onGround) {
			s.consumed = true;
		}
		s.lastOnGround = onGround;

		if (s.realHeld) s.pressAge += dt;

		if (s.virtualHold) {
			s.remaining -= dt;
			if (s.consumed || s.remaining <= 0.f) {
				s.virtualHold = false;
				sendRelease(isPlayer1);
			}
		}
	}

	void update(float dt) {
		GJBaseGameLayer::update(dt);
		tickPlayer(true, dt);
		tickPlayer(false, dt);
	}

	void markConsumed(PlayerObject* player) {
		if (player == m_player1) m_fields->p1.consumed = true;
		else if (player == m_player2) m_fields->p2.consumed = true;
	}

	void clearBuffers() {
		// If we were holding for the player, let go first so the game
		// doesn't think the button is stuck down after respawning.
		if (m_fields->p1.virtualHold) sendRelease(true);
		if (m_fields->p2.virtualHold) sendRelease(false);
		m_fields->p1.reset();
		m_fields->p2.reset();
	}
};

// Hitting an orb counts as the buffered input doing its job.
class $modify(PlayerObject) {
	void ringJump(RingObject* ring, bool p1) {
		PlayerObject::ringJump(ring, p1);
		if (auto gl = GameManager::get()->m_gameLayer) {
			static_cast<BufferedLayer*>(gl)->markConsumed(this);
		}
	}
};

// Don't carry a buffered tap across a death / restart.
class $modify(PlayLayer) {
	void resetLevel() {
		static_cast<BufferedLayer*>(static_cast<GJBaseGameLayer*>(this))->clearBuffers();
		PlayLayer::resetLevel();
	}
};
