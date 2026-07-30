#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include "shared_camera.h"
#include "shared_ui.h"
#include "glm/trigonometric.hpp"
#include "glm/vec3.hpp"
#include "glm/geometric.hpp"
#include "glm/matrix.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

class drakan : public ps2_game
{
private:
	sentinel_counter sentinel;
	restoring_toggle_state<1> collision_flag;
	tweakable_value_set<float, 3> fov_values;
	read_only_value_set<float, 3> player_position;
	float position_edit[3] = { 0.0f, 0.0f, 0.0f };
	bool position_editor_initialized = false;

	restoring_toggle_state<1> infinite_flag;
	restoring_toggle_state<1> true_god_mode;
	tweakable_value_set<float, 1> infinite_bow_shooting;
	tweakable_value_set<double, 1> infinite_bow_ammo;
	float fov = 76.2722;
	static constexpr int position_x = 0;
	static constexpr int position_y = 1;
	static constexpr int position_z = 2;
	static constexpr int camera_fov_menu = 0;
	static constexpr int camera_fov_regular = 1;
	static constexpr int camera_fov_neg = 2;


	static constexpr int bow_shoot_speed = 0;
	static constexpr int bow_ammo = 0;
	bool show_experimental_options = false;

	// Retail USA retained debug-command wrapper. It submits the raw "fly" command
	// once and restores the update hook after the game has returned from it.
	static constexpr uint32_t fly_update_hook = 0x00135DC8;
	static constexpr uint32_t fly_expected_hook = 0x0C075748;
	static constexpr uint32_t fly_temporary_hook = 0x0C7FC000;
	static constexpr uint32_t fly_code_cave = 0x01FF0000;
	static constexpr uint32_t fly_command_buffer = 0x01FF0100;
	static constexpr uint32_t fly_completion_latch = 0x01FF0180;
	static constexpr uint32_t fly_command = 0x00796C66; // "fly\\0"
	static constexpr std::array<uint32_t, 30> fly_wrapper = { {
		0x27BDFFD0, 0x7FBF0020, 0xAFA40000, 0x0C075748, 0x00000000,
		0xAFA20004, 0x3C0801FF, 0x8D090180, 0x15200011, 0x00000000,
		0x3C080055, 0x8D085A54, 0x1100000D, 0x00000000, 0x8D0804B0,
		0x1100000A, 0x00000000, 0x24090001, 0x3C0801FF, 0xAD090180,
		0x3C0401FF, 0x0C0900EE, 0x34840100, 0x24090002, 0x3C0801FF,
		0xAD090180, 0x8FA20004, 0x7BBF0020, 0x03E00008, 0x27BD0030,
	} };

	enum class fly_command_state
	{
		idle,
		waiting_for_completion,
	};

	fly_command_state fly_state = fly_command_state::idle;
	bool fly_enabled = false;

	bool start_fly_command(const pcsx2& ps2)
	{
		ps2_ipc_cmd preflight(ps2);
		auto hook = preflight.queue_read<uint32_t>(fly_update_hook);
		auto hook_delay_slot = preflight.queue_read<uint32_t>(fly_update_hook + 4);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> workspace;
		for (size_t i = 0; i < workspace.size(); ++i)
		{
			workspace[i] = preflight.queue_read<uint32_t>(fly_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)));
		}
		preflight.send();

		if (preflight.read(hook) != fly_expected_hook || preflight.read(hook_delay_slot) != 0)
			return false;

		for (const auto& word : workspace)
		{
			if (preflight.read(word) != 0)
				return false;
		}

		ps2_ipc_cmd install(ps2);
		for (size_t i = 0; i < fly_wrapper.size(); ++i)
		{
			install.write<uint32_t>(fly_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)), fly_wrapper[i]);
		}
		install.write<uint32_t>(fly_command_buffer, fly_command);
		install.write<uint32_t>(fly_completion_latch, 0);
		install.write<uint32_t>(fly_update_hook, fly_temporary_hook); // Install the hook last.
		install.send();
		fly_state = fly_command_state::waiting_for_completion;
		return true;
	}

	void update_fly_command(const pcsx2& ps2)
	{
		if (fly_state != fly_command_state::waiting_for_completion)
			return;

		ps2_ipc_cmd read_latch(ps2);
		auto latch = read_latch.queue_read<uint32_t>(fly_completion_latch);
		read_latch.send();
		if (read_latch.read(latch) != 2)
			return;

		// The executor returned, so restore the hook before clearing the wrapper.
		ps2_ipc_cmd restore_hook(ps2);
		restore_hook.write<uint32_t>(fly_update_hook, fly_expected_hook).send();

		ps2_ipc_cmd clear_workspace(ps2);
		for (size_t i = 0; i < fly_wrapper.size(); ++i)
		{
			clear_workspace.write<uint32_t>(fly_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)), 0);
		}
		clear_workspace.write<uint32_t>(fly_command_buffer, 0);
		clear_workspace.write<uint32_t>(fly_completion_latch, 0).send();

		fly_state = fly_command_state::idle;
		fly_enabled = !fly_enabled;
	}

public:
	explicit drakan(const pcsx2& ps2)
		: sentinel(ps2, 0x01000000)
		, collision_flag(ps2, { {
			{ 0x0017F920, 0x00000000, 0x02242021 }	// Disable collision. (causes rynn to flicker currently from time to time)
		} })
		, fov_values(ps2, { 
			 { 0x006C65E0, 0x06C668C, 0x006C6674 },
		})
		, player_position(ps2, { {
			0x00A6AE14, // Rynn X, float32
			0x00A6AE18, // Rynn Y, float32
			0x00A6AE1C, // Rynn Z, float32
		} })
		, infinite_flag(ps2, {})
		, infinite_bow_shooting(ps2, { {
			0x00C4E604  // infinite shoot speed
		} })
		, infinite_bow_ammo(ps2, { {
			0x00d245a0  // infinite arrows
		} })
		, true_god_mode(ps2, { {
			{ 0x003D5530, 0x03E00008, 0x00A0302D }	// True god mode. Ignores fall damage.
		} })
	{
	}

	void draw_game_ui(const pcsx2& ps2, const controller& c, playback& camera_playback) override
	{
		ImGui::Text("game running.");

		ImGui::Separator();
		ImGui::Text("Rynn world position");
		ImGui::Text("X: %.3f", player_position.get(position_x));
		ImGui::Text("Y: %.3f", player_position.get(position_y));
		ImGui::Text("Z: %.3f", player_position.get(position_z));


		if (fly_state == fly_command_state::waiting_for_completion)
		{
			ImGui::Text("Fly command waiting for active gameplay...");
		}
		else if (!fly_enabled)
		{
			if (ImGui::Button("Enable Debug Fly"))
			{
				start_fly_command(ps2);
			}
		}
		else
		{
			if (ImGui::Button("Disable Debug Fly"))
			{
				start_fly_command(ps2);
			}
		}


		if (!position_editor_initialized)
		{
			position_edit[position_x] = player_position.get(position_x);
			position_edit[position_y] = player_position.get(position_y);
			position_edit[position_z] = player_position.get(position_z);
			position_editor_initialized = true;
		}

		if (ImGui::Button("Modify FOV")) 
		{
			fov_values.toggle_tweaking();
			sentinel.increment();
		} ImGui::SameLine();		

		ImGui::SliderFloat("", &fov, 20.0f, 180.0f, "%.2f");

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

		if (!true_god_mode.is_on()) {
			if (ImGui::Button("Enable True God mode"))
			{
				true_god_mode.set_on(true);
				sentinel.increment();
			}
		}
		else if (true_god_mode.is_on())
		{
			if (ImGui::Button("Disable True God mode"))
			{
				true_god_mode.set_on(false);
				sentinel.increment();
			}
		}

		ImGui::Checkbox("Enable Experiments", &show_experimental_options);
		if (show_experimental_options)
		{
			ImGui::Separator();
			ImGui::Text("Set Rynn world position");
			ImGui::InputFloat("Set X", &position_edit[position_x], 0.0f, 0.0f, "%.3f");
			ImGui::InputFloat("Set Y", &position_edit[position_y], 0.0f, 0.0f, "%.3f");
			ImGui::InputFloat("Set Z", &position_edit[position_z], 0.0f, 0.0f, "%.3f");

			if (ImGui::Button("Apply Position"))
			{
				if (std::isfinite(position_edit[position_x]) &&
					std::isfinite(position_edit[position_y]) &&
					std::isfinite(position_edit[position_z]))
				{
					ps2_ipc_cmd cmd(ps2);
					cmd.write<float>(0x00A6AE14, position_edit[position_x]);
					cmd.write<float>(0x00A6AE18, position_edit[position_y]);
					cmd.write<float>(0x00A6AE1C, position_edit[position_z]);
					cmd.send();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Use Current Position"))
			{
				position_edit[position_x] = player_position.get(position_x);
				position_edit[position_y] = player_position.get(position_y);
				position_edit[position_z] = player_position.get(position_z);
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
	}

	void update(const pcsx2& ps2, const controller_state& c, playback& camera_playback, float time_delta) override
	{
		player_position.update();

		if (sentinel.has_reset())
		{
			position_editor_initialized = false;
			collision_flag.reset();
			fov_values.reset();
			infinite_flag.reset();
			true_god_mode.reset();
			infinite_bow_shooting.reset();
			infinite_bow_ammo.reset();
			fly_state = fly_command_state::idle;
			fly_enabled = false;

		}

		update_fly_command(ps2);

		if (infinite_bow_shooting.currently_tweaking()) {
			infinite_bow_shooting.set(bow_shoot_speed, 1000.0f);
			infinite_bow_ammo.set(bow_ammo, 7.40408214321435E248); // interpreted as 180 in game
			infinite_bow_shooting.flush(ps2);
			infinite_bow_ammo.flush(ps2);
		}

		if (fov_values.currently_tweaking()) {
			fov_values.set(camera_fov_menu, fov);
			fov_values.set(camera_fov_regular, fov);
			fov_values.set(camera_fov_neg, -fov);
			fov_values.flush(ps2);
		}
	}

	bool needs_ui_refresh() const override
	{
		return true;
	}
};

ps2_game_static_register<drakan> r("4167D813", "Drakan - The Ancients' Gates (USA)");
