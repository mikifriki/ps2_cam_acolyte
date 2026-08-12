#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include <array>
#include <cmath>
#include <cstdint>

class espn_winter_x_games_snowboarding_2002 : public ps2_game
{
private:
	sentinel_counter sentinel;
	read_only_value_set<float, 3> player_position;
	read_only_value_set<float, 3> motion_vector;
	uint32_t physics_object_address = 0;
	uint32_t player_position_address = 0;
	uint32_t motion_vector_address = 0;
	bool player_position_available = false;
	float position_edit[3] = { 0.0f, 0.0f, 0.0f };
	float motion_edit[3] = { 0.0f, 0.0f, 0.0f };
	float bookmarked_position[3] = { 0.0f, 0.0f, 0.0f };
	float position_step = 100.0f;
	bool position_editor_initialized = false;
	bool motion_editor_initialized = false;
	bool bookmark_available = false;
	bool zero_motion_after_position_change = true;
	int selected_course = 0;
	bool course_redirect_applied = false;

	static constexpr int position_x = 0;
	static constexpr int position_y = 1;
	static constexpr int position_z = 2;
	static constexpr uint32_t player_aggregate = 0x00314710;
	static constexpr uint32_t physics_pointer_offset = 0x0C;
	static constexpr uint32_t physics_position_offset = 0x10;
	static constexpr uint32_t physics_motion_vector_offset = 0x70;
	static constexpr uint32_t ee_ram_size = 0x02000000;
	static constexpr float radians_to_degrees = 57.2957795f;
	static constexpr std::array<uint32_t, 3> course_filename_addresses = {
		0x002B3748, 0x00334E6C, 0x018FC6B0
	};
	static constexpr std::array<const char*, 7> course_filenames = {
		"FIELD_C03.BPX", "FIELD_C06.BPX", "FIELD_C09.BPX", "FIELD_C16.BPX",
		"FIELD_C17.BPX", "FIELD_C22.BPX", "FIELD_C06B.BPX"
	};
	static constexpr std::array<const char*, 7> course_names = {
		"Home Town (C03)", "C06", "C09", "C16", "C17", "C22", "C06B"
	};

	bool update_player_state(const pcsx2& ps2)
	{
		ps2_ipc_cmd pointer_read(ps2);
		auto physics_object = pointer_read.queue_read<uint32_t>(player_aggregate + physics_pointer_offset);
		pointer_read.send();
		const auto physics_object_address = pointer_read.read(physics_object);

		if (physics_object_address == 0 ||
			physics_object_address > ee_ram_size - physics_motion_vector_offset - (3 * sizeof(float)))
		{
			this->physics_object_address = 0;
			player_position_address = 0;
			motion_vector_address = 0;
			player_position_available = false;
			position_editor_initialized = false;
			motion_editor_initialized = false;
			return false;
		}

		this->physics_object_address = physics_object_address;
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

	void copy_current_motion_to_editor()
	{
		motion_edit[position_x] = motion_vector.get(position_x);
		motion_edit[position_y] = motion_vector.get(position_y);
		motion_edit[position_z] = motion_vector.get(position_z);
		motion_editor_initialized = true;
	}

	static bool is_finite_vector(const float* vector)
	{
		return std::isfinite(vector[position_x]) &&
			std::isfinite(vector[position_y]) &&
			std::isfinite(vector[position_z]);
	}

	void write_motion_vector(const pcsx2& ps2, const float* vector)
	{
		if (!player_position_available || !is_finite_vector(vector))
			return;

		ps2_ipc_cmd command(ps2);
		for (int component = 0; component < 3; ++component)
		{
			command.write<float>(motion_vector_address + (component * sizeof(float)), vector[component]);
		}
		command.send();
	}

	void write_position(const pcsx2& ps2, const float* position)
	{
		if (!player_position_available || !is_finite_vector(position))
			return;

		ps2_ipc_cmd command(ps2);
		for (int component = 0; component < 3; ++component)
		{
			command.write<float>(player_position_address + (component * sizeof(float)), position[component]);
		}
		if (zero_motion_after_position_change)
		{
			for (int component = 0; component < 3; ++component)
			{
				command.write<float>(motion_vector_address + (component * sizeof(float)), 0.0f);
			}
		}
		command.send();
	}

	void nudge_position(const pcsx2& ps2, int component, float amount)
	{
		float target[3] = {
			player_position.get(position_x),
			player_position.get(position_y),
			player_position.get(position_z)
		};
		target[component] += amount;
		write_position(ps2, target);
	}

	void apply_course_redirect(const pcsx2& ps2)
	{
		const auto filename = course_filenames[selected_course];
		ps2_ipc_cmd command(ps2);
		for (const auto address : course_filename_addresses)
		{
			for (uint32_t i = 0; i < 16; ++i)
			{
				command.write<uint8_t>(address + i, static_cast<uint8_t>(filename[i]));
				if (filename[i] == '\0')
				{
					for (++i; i < 16; ++i)
						command.write<uint8_t>(address + i, 0);
					break;
				}
			}
		}
		command.send();
		course_redirect_applied = true;
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
			const auto horizontal_magnitude = std::sqrt((motion_x * motion_x) + (motion_z * motion_z));

			ImGui::Separator();
			ImGui::Text("Rider motion vector");
			ImGui::Text("X: %+.3f", motion_x);
			ImGui::Text("Y: %+.3f", motion_y);
			ImGui::Text("Z: %+.3f", motion_z);
			ImGui::Text("Magnitude: %.3f", motion_magnitude);
			ImGui::Text("Horizontal: %.3f", horizontal_magnitude);
			if (horizontal_magnitude > 0.001f)
				ImGui::Text("Heading: %.1f deg", std::atan2(motion_x, motion_z) * radians_to_degrees);
			else
				ImGui::Text("Heading: --");
			ImGui::Text("Vertical: %s", motion_y > 0.001f ? "rising" : motion_y < -0.001f ? "falling" : "level");
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

		if (ImGui::Button("Apply Position"))
		{
			write_position(ps2, position_edit);
		}

		ImGui::SameLine();
		if (ImGui::Button("Use Current Position"))
		{
			copy_current_position_to_editor();
		}
		ImGui::Checkbox("Zero motion after position changes", &zero_motion_after_position_change);

		ImGui::InputFloat("Nudge distance", &position_step, 10.0f, 100.0f, "%.1f");
		if (std::isfinite(position_step) && position_step > 0.0f)
		{
			if (ImGui::Button("X -")) nudge_position(ps2, position_x, -position_step);
			ImGui::SameLine();
			if (ImGui::Button("X +")) nudge_position(ps2, position_x, position_step);
			if (ImGui::Button("Y -")) nudge_position(ps2, position_y, -position_step);
			ImGui::SameLine();
			if (ImGui::Button("Y +")) nudge_position(ps2, position_y, position_step);
			if (ImGui::Button("Z -")) nudge_position(ps2, position_z, -position_step);
			ImGui::SameLine();
			if (ImGui::Button("Z +")) nudge_position(ps2, position_z, position_step);
		}

		if (ImGui::Button("Save Position Bookmark"))
		{
			bookmarked_position[position_x] = player_position.get(position_x);
			bookmarked_position[position_y] = player_position.get(position_y);
			bookmarked_position[position_z] = player_position.get(position_z);
			bookmark_available = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!bookmark_available);
		if (ImGui::Button("Teleport to Bookmark"))
			write_position(ps2, bookmarked_position);
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		ImGui::Separator();
		ImGui::Text("Set rider motion vector");
		ImGui::BeginDisabled(!player_position_available);
		ImGui::InputFloat("Motion X", &motion_edit[position_x], 0.0f, 0.0f, "%+.3f");
		ImGui::InputFloat("Motion Y", &motion_edit[position_y], 0.0f, 0.0f, "%+.3f");
		ImGui::InputFloat("Motion Z", &motion_edit[position_z], 0.0f, 0.0f, "%+.3f");
		if (ImGui::Button("Apply Motion"))
			write_motion_vector(ps2, motion_edit);
		ImGui::SameLine();
		if (ImGui::Button("Use Current Motion"))
			copy_current_motion_to_editor();
		ImGui::SameLine();
		if (ImGui::Button("Zero Motion"))
		{
			const float stopped[3] = { 0.0f, 0.0f, 0.0f };
			write_motion_vector(ps2, stopped);
			motion_edit[position_x] = 0.0f;
			motion_edit[position_y] = 0.0f;
			motion_edit[position_z] = 0.0f;
		}
		ImGui::EndDisabled();

		ImGui::Separator();
		ImGui::Text("Runtime addresses");
		ImGui::Text("Physics: 0x%08X", physics_object_address);
		ImGui::Text("Position: 0x%08X", player_position_address);
		ImGui::Text("Motion: 0x%08X", motion_vector_address);

		ImGui::Separator();
		ImGui::Text("Home Town course archive redirect");
		if (ImGui::Combo("Target course", &selected_course, course_names.data(), static_cast<int>(course_names.size())))
			course_redirect_applied = false;
		if (ImGui::Button("Apply Course Redirect"))
			apply_course_redirect(ps2);
		ImGui::TextWrapped("Apply outside a loaded course, then load Home Town. The original Home Town spawn metadata is retained and may place the rider out of bounds.");
		if (course_redirect_applied)
			ImGui::Text("Applied: %s", course_names[selected_course]);

		if (!position_editor_initialized && player_position_available)
		{
			copy_current_position_to_editor();
		}
		if (!motion_editor_initialized && player_position_available)
		{
			copy_current_motion_to_editor();
		}
	}

	void update(const pcsx2& ps2, const controller_state& c, playback& camera_playback, float time_delta) override
	{
		if (sentinel.has_reset())
		{
			physics_object_address = 0;
			player_position_address = 0;
			motion_vector_address = 0;
			player_position_available = false;
			position_editor_initialized = false;
			motion_editor_initialized = false;
			course_redirect_applied = false;
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
