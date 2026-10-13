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
	uint32_t player_position_address = 0;
	bool player_position_available = false;
	float position_edit[3] = { 0.0f, 0.0f, 0.0f };
	bool position_editor_initialized = false;
	read_only_value_set<float, 3> camera_position;
	bool camera_position_available = false;

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
	static constexpr uint32_t manager_global = 0x00555A54;
	static constexpr uint32_t player_pointer_offset = 0x4B0;
	static constexpr uint32_t player_position_offset = 0x14;
	static constexpr uint32_t camera_pointer_offset = 0x4B4;
	static constexpr uint32_t camera_position_offset = 0x14;
	static constexpr uint32_t actor_relation_offset = 0xFC;
	static constexpr uint32_t relation_control_offset = 0x10;
	static constexpr uint32_t controller_mode_offset = 0x14C;
	static constexpr uint32_t fly_mode = 7;


	static constexpr int bow_shoot_speed = 0;
	static constexpr int bow_ammo = 0;
	bool show_experimental_options = false;

	bool update_player_position(const pcsx2& ps2)
	{
		ps2_ipc_cmd manager_read(ps2);
		auto manager = manager_read.queue_read<uint32_t>(manager_global);
		manager_read.send();
		const auto manager_address = manager_read.read(manager);

		if (manager_address == 0)
		{
			player_position_address = 0;
			player_position_available = false;
			return false;
		}

		ps2_ipc_cmd player_read(ps2);
		auto player = player_read.queue_read<uint32_t>(manager_address + player_pointer_offset);
		player_read.send();
		const auto player_address = player_read.read(player);

		if (player_address == 0)
		{
			player_position_address = 0;
			player_position_available = false;
			return false;
		}

		player_position_address = player_address + player_position_offset;
		player_position.update_base_address(player_position_address);
		player_position_available = true;
		return true;
	}

	bool update_camera_position(const pcsx2& ps2)
	{
		ps2_ipc_cmd manager_read(ps2);
		auto manager = manager_read.queue_read<uint32_t>(manager_global);
		manager_read.send();
		const auto manager_address = manager_read.read(manager);

		if (manager_address == 0)
		{
			camera_position_available = false;
			return false;
		}

		ps2_ipc_cmd camera_read(ps2);
		auto camera = camera_read.queue_read<uint32_t>(manager_address + camera_pointer_offset);
		camera_read.send();
		const auto camera_address = camera_read.read(camera);

		if (camera_address == 0)
		{
			camera_position_available = false;
			return false;
		}

		camera_position.update_base_address(camera_address + camera_position_offset);
		camera_position_available = true;
		return true;
	}

	void update_fly_mode(const pcsx2& ps2)
	{
		fly_enabled = false;
		if (!camera_position_available)
			return;

		const auto camera_address = camera_position.get_address(position_x) - camera_position_offset;
		ps2_ipc_cmd relation_read(ps2);
		auto relation = relation_read.queue_read<uint32_t>(camera_address + actor_relation_offset);
		relation_read.send();
		const auto relation_address = relation_read.read(relation);
		if (relation_address == 0)
			return;

		ps2_ipc_cmd control_read(ps2);
		auto control = control_read.queue_read<uint32_t>(relation_address + relation_control_offset);
		control_read.send();
		const auto control_address = control_read.read(control);
		if (control_address == 0)
			return;

		ps2_ipc_cmd mode_read(ps2);
		auto mode = mode_read.queue_read<uint32_t>(control_address + controller_mode_offset);
		mode_read.send();
		fly_enabled = mode_read.read(mode) == fly_mode;
	}

	// Retail USA retained debug-command wrapper. It submits FLY or DROP once
	// and restores the update hook after the game has returned from it.
	static constexpr uint32_t debug_update_hook = 0x00135DC8;
	static constexpr uint32_t debug_expected_hook = 0x0C075748;
	static constexpr uint32_t debug_temporary_hook = 0x0C7FC000;
	static constexpr uint32_t debug_code_cave = 0x01FF0000;
	static constexpr uint32_t debug_command_buffer = 0x01FF0100;
	static constexpr uint32_t debug_completion_latch = 0x01FF0180;
	static constexpr uint32_t fly_command = 0x00796C66; // "fly\\0"
	static constexpr uint32_t drop_command = 0x706F7264; // "drop", followed by a zero word.
	static constexpr std::array<uint32_t, 30> debug_wrapper = { {
		0x27BDFFD0, 0x7FBF0020, 0xAFA40000, 0x0C075748, 0x00000000,
		0xAFA20004, 0x3C0801FF, 0x8D090180, 0x15200011, 0x00000000,
		0x3C080055, 0x8D085A54, 0x1100000D, 0x00000000, 0x8D0804B0,
		0x1100000A, 0x00000000, 0x24090001, 0x3C0801FF, 0xAD090180,
		0x3C0401FF, 0x0C0900EE, 0x34840100, 0x24090002, 0x3C0801FF,
		0xAD090180, 0x8FA20004, 0x7BBF0020, 0x03E00008, 0x27BD0030,
	} };

	enum class debug_command_state
	{
		idle,
		waiting_for_fly,
		waiting_for_drop,
	};

	debug_command_state debug_state = debug_command_state::idle;
	bool fly_enabled = false;
	const char* debug_command_message = nullptr;

	bool start_debug_command(const pcsx2& ps2, debug_command_state command)
	{
		if (debug_state != debug_command_state::idle || command == debug_command_state::idle)
			return false;

		if (command == debug_command_state::waiting_for_drop &&
			(!fly_enabled || !player_position_available || !camera_position_available))
		{
			debug_command_message = "Enable Debug Fly before dropping Rynn.";
			return false;
		}

		ps2_ipc_cmd preflight(ps2);
		auto hook = preflight.queue_read<uint32_t>(debug_update_hook);
		auto hook_delay_slot = preflight.queue_read<uint32_t>(debug_update_hook + 4);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> workspace;
		for (size_t i = 0; i < workspace.size(); ++i)
		{
			workspace[i] = preflight.queue_read<uint32_t>(debug_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)));
		}
		preflight.send();

		if (preflight.read(hook) != debug_expected_hook || preflight.read(hook_delay_slot) != 0)
		{
			debug_command_message = "Debug command unavailable: update hook is in use.";
			return false;
		}

		for (const auto& word : workspace)
		{
			if (preflight.read(word) != 0)
			{
				debug_command_message = "Debug command unavailable: command workspace is in use.";
				return false;
			}
		}

		ps2_ipc_cmd install(ps2);
		for (size_t i = 0; i < debug_wrapper.size(); ++i)
		{
			install.write<uint32_t>(debug_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)), debug_wrapper[i]);
		}
		install.write<uint32_t>(debug_command_buffer,
			command == debug_command_state::waiting_for_drop ? drop_command : fly_command);
		install.write<uint32_t>(debug_command_buffer + sizeof(uint32_t), 0);
		install.write<uint32_t>(debug_completion_latch, 0);
		install.write<uint32_t>(debug_update_hook, debug_temporary_hook); // Install the hook last.
		install.send();
		debug_state = command;
		debug_command_message = nullptr;
		return true;
	}

	void update_debug_command(const pcsx2& ps2)
	{
		if (debug_state == debug_command_state::idle)
			return;

		ps2_ipc_cmd read_latch(ps2);
		auto latch = read_latch.queue_read<uint32_t>(debug_completion_latch);
		read_latch.send();
		if (read_latch.read(latch) != 2)
			return;

		// The executor returned, so restore the hook before clearing the wrapper.
		ps2_ipc_cmd restore_hook(ps2);
		restore_hook.write<uint32_t>(debug_update_hook, debug_expected_hook).send();

		ps2_ipc_cmd clear_workspace(ps2);
		for (size_t i = 0; i < debug_wrapper.size(); ++i)
		{
			clear_workspace.write<uint32_t>(debug_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)), 0);
		}
		clear_workspace.write<uint32_t>(debug_command_buffer, 0);
		clear_workspace.write<uint32_t>(debug_command_buffer + sizeof(uint32_t), 0);
		clear_workspace.write<uint32_t>(debug_completion_latch, 0).send();

		if (debug_state == debug_command_state::waiting_for_drop)
		{
			debug_command_message = "DROP returned. Check Rynn world position.";
		}
		debug_state = debug_command_state::idle;
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
		, camera_position(ps2, static_cast<uint32_t>(0))
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

		ImGui::Text("Set Rynn world position");
		ImGui::InputFloat("Set X", &position_edit[position_x], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Set Y", &position_edit[position_y], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Set Z", &position_edit[position_z], 0.0f, 0.0f, "%.3f");

		if (ImGui::Button("Apply Position"))
		{
			if (player_position_available &&
				std::isfinite(position_edit[position_x]) &&
				std::isfinite(position_edit[position_y]) &&
				std::isfinite(position_edit[position_z]))
			{
				ps2_ipc_cmd cmd(ps2);
				cmd.write<float>(player_position_address + (position_x * sizeof(float)), position_edit[position_x]);
				cmd.write<float>(player_position_address + (position_y * sizeof(float)), position_edit[position_y]);
				cmd.write<float>(player_position_address + (position_z * sizeof(float)), position_edit[position_z]);
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

		ImGui::Separator();

		ImGui::Text("Camera world position");
		if (camera_position_available)
		{
			ImGui::Text("X: %.3f", camera_position.get(position_x));
			ImGui::Text("Y: %.3f", camera_position.get(position_y));
			ImGui::Text("Z: %.3f", camera_position.get(position_z));
		}
		else
		{
			ImGui::Text("Camera position unavailable.");
		}

		ImGui::BeginDisabled(!fly_enabled || !player_position_available ||
			!camera_position_available || debug_state != debug_command_state::idle);
		if (ImGui::Button("Drop Rynn at Camera"))
		{
			start_debug_command(ps2, debug_command_state::waiting_for_drop);
		}
		ImGui::EndDisabled();
		ImGui::Text("DROP uses the flying camera position and offsets.");
		if (!fly_enabled)
			ImGui::Text("Enable Debug Fly to use DROP.");

		if (debug_state != debug_command_state::idle)
		{
			ImGui::Text(debug_state == debug_command_state::waiting_for_drop
				? "Drop command waiting for active gameplay..."
				: "Fly command waiting for active gameplay...");
		}
		else if (!fly_enabled)
		{
			if (ImGui::Button("Enable Debug Fly"))
			{
				start_debug_command(ps2, debug_command_state::waiting_for_fly);
			}
		}
		else
		{
			if (ImGui::Button("Disable Debug Fly"))
			{
				start_debug_command(ps2, debug_command_state::waiting_for_fly);
			}
		}

		ImGui::Separator();

		if (debug_command_message != nullptr)
			ImGui::TextUnformatted(debug_command_message);

		if (!position_editor_initialized && player_position_available)
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
		update_player_position(ps2);
		update_camera_position(ps2);

		if (sentinel.has_reset())
		{
			position_editor_initialized = false;
			player_position_address = 0;
			player_position_available = false;
			camera_position_available = false;
			collision_flag.reset();
			fov_values.reset();
			infinite_flag.reset();
			true_god_mode.reset();
			infinite_bow_shooting.reset();
			infinite_bow_ammo.reset();
			debug_state = debug_command_state::idle;
			fly_enabled = false;
			debug_command_message = nullptr;

		}

		update_debug_command(ps2);
		update_fly_mode(ps2);

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
