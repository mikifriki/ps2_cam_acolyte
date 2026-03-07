#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include "shared_camera.h"
#include "shared_ui.h"
#include "glm/trigonometric.hpp"
#include "glm/vec3.hpp"
#include "glm/geometric.hpp"
#include "glm/matrix.hpp"
#include <iostream>

class drakan : public ps2_game
{
private:
	sentinel_counter sentinel;
	restoring_toggle_state<1> collision_flag;
	restoring_toggle_state<2> disable_cam;
	tweakable_value_set<float, 5> camera_values;

	restoring_toggle_state<1> infinite_flag;
	tweakable_value_set<float, 1> infinite_bow_shooting;
	tweakable_value_set<double, 1> infinite_bow_ammo;

	float current_pitch = 0.0f;
	float current_yaw = 0.0f;
	static constexpr int camera_forward = 0;
	static constexpr int camera_up = 1;
	static constexpr int camera_right = 2;
	static constexpr int camera_yaw = 3;
	static constexpr int camera_pitch = 4;

	static constexpr int bow_shoot_speed = 0;
	static constexpr int bow_ammo = 0;

public:
	explicit drakan(const pcsx2& ps2)
		: sentinel(ps2, 0x01000000)
		, collision_flag(ps2, { {
			{ 0x0017F920, 0x00000000, 0x02242021 }	// Disable collision. (causes rynn to flicker currently from time to time)
		} })
		, disable_cam(ps2, { {
			{ 0x001343F0, 0x03E00008, 0x27BDFFE0  },// Camera disable	
			{ 0x0015DF90, 0x03E00008, 0x0080382D  },// Disable culling based on Rynns view
		} })
		, camera_values(ps2, {
			0x006C6810, // camera forward
			0x006C680C, // camera up/down
			0x006C6808, // camera right
			0x00000000, // camera yaw
			0x00000000, // camera pitch
			})
			, infinite_flag(ps2, { {0x00000000, 0x00000001 , 0x00000000} })
		, infinite_bow_shooting(ps2, { {
			0x00C4E604  // infinite arrows
		} })
		, infinite_bow_ammo(ps2, { {
			0x00d245a0  // infinite arrows
		} })
	{
	}

	void draw_game_ui(const pcsx2& ps2, const controller& c, playback& camera_playback) override
	{
		ImGui::Text("game running.");

		if (!collision_flag.is_on())
		{
			if (ImGui::Button("Disable Collision"))
			{
				collision_flag.set_on(true);
				sentinel.increment();
			}
		}
		else if (collision_flag.is_on())
		{
			if (ImGui::Button("Enable Collision"))
			{
				collision_flag.set_on(false);
				sentinel.increment();
			}
		}

		if (!disable_cam.is_on())
		{
			if (ImGui::Button("Enable Free Cam"))
			{
				disable_cam.set_on(true);
				camera_values.toggle_tweaking();
				sentinel.increment();
			}
		}
		else if (disable_cam.is_on())
		{
			if (ImGui::Button("Disable Free Cam"))
			{
				camera_values.toggle_tweaking();
				disable_cam.set_on(false);
				sentinel.increment();
			}
		}

		if (!infinite_flag.is_on()) {
			if (ImGui::Button("Enable infinite bow ammo/shoot speed"))
			{
				infinite_flag.set_on(true);
				infinite_bow_shooting.toggle_tweaking();
				infinite_bow_ammo.toggle_tweaking();
				sentinel.increment();
			}
		}
		else if (infinite_flag.is_on())
		{
			if (ImGui::Button("Disable infinite bow ammo/shoot speed"))
			{
				infinite_flag.set_on(false);
				infinite_bow_shooting.toggle_tweaking();
				infinite_bow_ammo.toggle_tweaking();
				sentinel.increment();
			}
		}
	}

	void update(const pcsx2& ps2, const controller_state& c, playback& camera_playback, float time_delta) override
	{
		if (sentinel.has_reset())
		{
			collision_flag.reset();
			disable_cam.reset();
		}

		if (infinite_bow_shooting.currently_tweaking()) {
			infinite_bow_shooting.set(bow_shoot_speed, 1000.0f);
			infinite_bow_ammo.set(bow_ammo, 7.40408214321435E248); // interpreted as 180 in game
			infinite_bow_shooting.flush(ps2);
			infinite_bow_ammo.flush(ps2);
		}
		if (camera_values.currently_tweaking())
		{
			float turn_scale = time_delta * 5.0f;
			float move_scale = time_delta * 5000.0f;

			 float current_yaw = camera_values.get(camera_yaw);
			 float current_pitch = camera_values.get(camera_pitch);

			glm::vec3 pos_delta = shared_camera::compute_freecam_pos_delta(c, glm::vec2(move_scale, -move_scale), current_yaw, current_pitch);
			glm::vec3 pos = glm::vec3(camera_values.get(camera_right), camera_values.get(camera_forward), camera_values.get(camera_up)) + pos_delta;

			camera_playback.update(time_delta, current_yaw, current_pitch, pos.x, pos.y, pos.z);

			if (camera_playback.update(time_delta, current_yaw, current_pitch, pos.x, pos.y, pos.z))
			{
				camera_values.set(camera_yaw, current_yaw);
				camera_values.set(camera_pitch, current_pitch);
			}

			camera_values.set(camera_forward, pos.y);
			camera_values.set(camera_up, pos.z);

			camera_values.set(camera_right, pos.x);

			camera_values.flush(ps2);
		}
	}
};

ps2_game_static_register<drakan> r("4167D813", "Drakan - The Ancients' Gates (USA)");