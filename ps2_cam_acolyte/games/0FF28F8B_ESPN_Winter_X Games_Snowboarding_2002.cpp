#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include <cmath>
#include <cstdint>

class espn_winter_x_games_snowboarding_2002 : public ps2_game
{
private:
	sentinel_counter sentinel;
	read_only_value_set<float, 3> player_position;
	read_only_value_set<float, 3> motion_vector;
	uint32_t player_position_address = 0;
	uint32_t motion_vector_address = 0;
	bool player_position_available = false;
	float position_edit[3] = { 0.0f, 0.0f, 0.0f };
	bool position_editor_initialized = false;

	static constexpr int position_x = 0;
	static constexpr int position_y = 1;
	static constexpr int position_z = 2;
	static constexpr uint32_t player_aggregate = 0x00314710;
	static constexpr uint32_t physics_pointer_offset = 0x0C;
	static constexpr uint32_t physics_position_offset = 0x10;
	static constexpr uint32_t physics_motion_vector_offset = 0x70;
	static constexpr uint32_t ee_ram_size = 0x02000000;

	bool update_player_state(const pcsx2& ps2)
	{
		ps2_ipc_cmd pointer_read(ps2);
		auto physics_object = pointer_read.queue_read<uint32_t>(player_aggregate + physics_pointer_offset);
		pointer_read.send();
		const auto physics_object_address = pointer_read.read(physics_object);

		if (physics_object_address == 0 ||
			physics_object_address > ee_ram_size - physics_motion_vector_offset - (3 * sizeof(float)))
		{
			player_position_address = 0;
			motion_vector_address = 0;
			player_position_available = false;
			position_editor_initialized = false;
			return false;
		}

		const auto new_position_address = physics_object_address + physics_position_offset;
		const auto new_motion_vector_address = physics_object_address + physics_motion_vector_offset;
		if (new_position_address != player_position_address)
		{
			player_position_address = new_position_address;
			motion_vector_address = new_motion_vector_address;
			player_position.update_base_address(player_position_address);
			motion_vector.update_base_address(motion_vector_address);
		}
		else
		{
			player_position.update();
			motion_vector.update();
		}

		player_position_available = true;
		return true;
	}

	void copy_current_position_to_editor()
	{
		position_edit[position_x] = player_position.get(position_x);
		position_edit[position_y] = player_position.get(position_y);
		position_edit[position_z] = player_position.get(position_z);
		position_editor_initialized = true;
	}

public:
	explicit espn_winter_x_games_snowboarding_2002(const pcsx2& ps2)
		: sentinel(ps2, 0x01000000)
		, player_position(ps2, uint32_t{ 0 })
		, motion_vector(ps2, uint32_t{ 0 })
	{
	}

	void draw_game_ui(const pcsx2& ps2, const controller& c, playback& camera_playback) override
	{
		ImGui::Text("Rider world position");
		if (player_position_available)
		{
			ImGui::Text("X: %.3f", player_position.get(position_x));
			ImGui::Text("Y: %.3f", player_position.get(position_y));
			ImGui::Text("Z: %.3f", player_position.get(position_z));

			const auto motion_x = motion_vector.get(position_x);
			const auto motion_y = motion_vector.get(position_y);
			const auto motion_z = motion_vector.get(position_z);
			const auto motion_magnitude = std::sqrt(
				(motion_x * motion_x) + (motion_y * motion_y) + (motion_z * motion_z));

			ImGui::Separator();
			ImGui::Text("Rider motion vector");
			ImGui::Text("X: %+.3f", motion_x);
			ImGui::Text("Y: %+.3f", motion_y);
			ImGui::Text("Z: %+.3f", motion_z);
			ImGui::Text("Magnitude: %.3f", motion_magnitude);
		}
		else
		{
			ImGui::Text("Unavailable. Load into a course.");
		}

		ImGui::Separator();
		ImGui::Text("Set rider world position");
		ImGui::BeginDisabled(!player_position_available);
		ImGui::InputFloat("Set X", &position_edit[position_x], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Set Y", &position_edit[position_y], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Set Z", &position_edit[position_z], 0.0f, 0.0f, "%.3f");

		if (ImGui::Button("Apply Position") &&
			std::isfinite(position_edit[position_x]) &&
			std::isfinite(position_edit[position_y]) &&
			std::isfinite(position_edit[position_z]))
		{
			ps2_ipc_cmd command(ps2);
			command.write<float>(player_position_address + (position_x * sizeof(float)), position_edit[position_x]);
			command.write<float>(player_position_address + (position_y * sizeof(float)), position_edit[position_y]);
			command.write<float>(player_position_address + (position_z * sizeof(float)), position_edit[position_z]);
			command.send();
		}

		ImGui::SameLine();
		if (ImGui::Button("Use Current Position"))
		{
			copy_current_position_to_editor();
		}
		ImGui::EndDisabled();

		if (!position_editor_initialized && player_position_available)
		{
			copy_current_position_to_editor();
		}
	}

	void update(const pcsx2& ps2, const controller_state& c, playback& camera_playback, float time_delta) override
	{
		if (sentinel.has_reset())
		{
			player_position_address = 0;
			motion_vector_address = 0;
			player_position_available = false;
			position_editor_initialized = false;
		}

		update_player_state(ps2);
	}

	bool needs_ui_refresh() const override
	{
		return true;
	}
};

ps2_game_static_register<espn_winter_x_games_snowboarding_2002> r(
	"0FF28F8B", "ESPN Winter X Games Snowboarding 2002 (USA)");
