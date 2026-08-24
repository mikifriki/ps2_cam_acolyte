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
	bool original_course_metadata_saved = false;
	std::array<uint32_t, 64> original_course_metadata = {};

	static constexpr int position_x = 0;
	static constexpr int position_y = 1;
	static constexpr int position_z = 2;
	static constexpr uint32_t player_aggregate = 0x00314710;
	static constexpr uint32_t physics_pointer_offset = 0x0C;
	static constexpr uint32_t physics_position_offset = 0x10;
	static constexpr uint32_t physics_motion_vector_offset = 0x70;
	static constexpr uint32_t ee_ram_size = 0x02000000;
	static constexpr float radians_to_degrees = 57.2957795f;
	static constexpr uint32_t home_town_course_metadata = 0x018FAC80;
	static constexpr uint32_t course_filename_pointer_index = 0x38 / sizeof(uint32_t);
	static constexpr uint32_t course_filename_start = 0x002B3700;
	static constexpr uint32_t course_filename_end = 0x002B3908;
	static constexpr std::array<uint32_t, 10> course_metadata_addresses = {
		0x018FAC80, // C03
		0x018FAF80, // C06
		0x018FB080, // C06B
		0x018FB380, // C09
		0x018FB780, // C12SES
		0x018FBB80, // C15SES
		0x018FBC80, // C16
		0x018FBD80, // C17
		0x018FC280, // C22
		0x018FC480, // Practice
	};
	static constexpr std::array<const char*, 10> course_names = {
		"Home Town (C03)",
		"Storage of Water Tank (C06)",
		"Storage of Water Tank alternate (C06B)",
		"Abandoned Mine (C09)",
		"C12SES alternate",
		"C15SES alternate",
		"The Room (C16)",
		"Beach (C17)",
		"Alaska Large Descent (C22)",
		"Practice"
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

	bool read_course_metadata(const pcsx2& ps2, uint32_t address, std::array<uint32_t, 64>& metadata)
	{
		ps2_ipc_cmd command(ps2);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, 64> reads;
		for (size_t i = 0; i < reads.size(); ++i)
		{
			reads[i] = command.queue_read<uint32_t>(address + static_cast<uint32_t>(i * sizeof(uint32_t)));
		}
		command.send();
		for (size_t i = 0; i < reads.size(); ++i)
			metadata[i] = command.read(reads[i]);

		const auto filename_pointer = metadata[course_filename_pointer_index];
		return filename_pointer >= course_filename_start && filename_pointer < course_filename_end;
	}

	void apply_course_redirect(const pcsx2& ps2)
	{
		if (!original_course_metadata_saved)
		{
			if (!read_course_metadata(ps2, home_town_course_metadata, original_course_metadata))
				return;
			original_course_metadata_saved = true;
		}

		std::array<uint32_t, 64> target_metadata;
		if (selected_course == 0)
		{
			target_metadata = original_course_metadata;
		}
		else if (!read_course_metadata(ps2, course_metadata_addresses[selected_course], target_metadata))
		{
			return;
		}

		ps2_ipc_cmd command(ps2);
		for (size_t i = 0; i < target_metadata.size(); ++i)
		{
			command.write<uint32_t>(home_town_course_metadata + static_cast<uint32_t>(i * sizeof(uint32_t)), target_metadata[i]);
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
		ImGui::Text("Experimental Home Town course redirect");
		if (ImGui::Combo("Target course", &selected_course, course_names.data(), static_cast<int>(course_names.size())))
			course_redirect_applied = false;
		if (ImGui::Button("Apply Course Redirect"))
			apply_course_redirect(ps2);
		ImGui::TextWrapped("Apply outside a loaded course, then load Home Town. This clones the target's complete course metadata record, including its existing filename and setup values. Home Town restores the record captured when the tool connected.");
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
			bookmark_available = false;
			course_redirect_applied = false;
			original_course_metadata_saved = false;
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
