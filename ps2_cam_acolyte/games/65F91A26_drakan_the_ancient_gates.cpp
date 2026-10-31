#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include "shared_camera.h"
#include "shared_ui.h"
#include "drakan_arokh_programs.h"
#include "drakan_arokh_power_program.h"
#include "drakan_arokh_remove_program.h"
#include <cstring>
#include "glm/trigonometric.hpp"
#include "glm/vec3.hpp"
#include "glm/geometric.hpp"
#include "glm/matrix.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <iostream>

class drakan : public ps2_game
{
private:
	// Linker padding after .reginfo, outside the game heap and EE code pages.
	static constexpr uint32_t load_sentinel_address = 0x004DB270;
	sentinel_counter sentinel;
	restoring_toggle_state<5> collision_flag;
	restoring_toggle_state<3> arokh_collision_flag;
	bool collision_before_arokh = false;
	tweakable_value_set<float, 3> fov_values;
	read_only_value_set<float, 3> player_position;
	uint32_t player_position_address = 0;
	bool player_position_available = false;
	float position_edit[3] = { 0.0f, 0.0f, 0.0f };
	bool position_editor_initialized = false;
	read_only_value_set<float, 3> camera_position;
	bool camera_position_available = false;
	read_only_value_set<uint32_t, 1> hud_values;

	restoring_toggle_state<1> infinite_flag;
	restoring_toggle_state<1> true_god_mode;
	double jump_height = 1.0; // Pending input; typing does not change the game.
	double jump_height_applied = 1.0;
	bool jump_height_invalid = false;
	tweakable_value_set<float, 1> infinite_bow_shooting;
	tweakable_value_set<double, 1> infinite_bow_ammo;
	float fov = 76.2722;
	static constexpr int position_x = 0;
	static constexpr int position_y = 1;
	static constexpr int position_z = 2;
	static constexpr int hud_visibility = 0;
	static constexpr uint32_t hud_flag_address = 0x004FC57C;
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

	static constexpr std::array<uint32_t, 5> camera_speed_addresses = {
		0x002A34B0, 0x002A34E0, 0x002A3500, 0x002A3528, 0x002A3548
	};
	static constexpr std::array<float, 7> camera_speed_presets = {
		0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f
	};
	float camera_speed_multiplier = 1.0f;
	bool camera_speed_available = false;
	const char* camera_speed_message = nullptr;

	static std::array<uint32_t, 10> camera_speed_words(float multiplier)
	{
		std::array<uint32_t, 10> words;
		for (size_t i = 0; i < camera_speed_addresses.size(); ++i)
		{
			const float sign = (i == 0 || i == 2 || i == 4) ? -1.0f : 1.0f;
			const uint32_t bits = std::bit_cast<uint32_t>(sign * 20000.0f * multiplier);
			words[i * 2] = 0x3C010000 | (bits >> 16);
			words[i * 2 + 1] = 0x34210000 | (bits & 0xFFFF);
		}
		return words;
	}

	float read_camera_speed(const pcsx2& ps2)
	{
		ps2_ipc_cmd read(ps2);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, 10> queued;
		for (size_t i = 0; i < camera_speed_addresses.size(); ++i)
		{
			queued[i * 2] = read.queue_read<uint32_t>(camera_speed_addresses[i]);
			queued[i * 2 + 1] = read.queue_read<uint32_t>(camera_speed_addresses[i] + 4);
		}
		read.send();
		std::array<uint32_t, 10> words;
		for (size_t i = 0; i < words.size(); ++i)
			words[i] = read.read(queued[i]);
		// Accept only complete retail/preset fingerprints, including a preset
		// restored by a save state or left by an earlier Acolyte connection.
		for (float preset : camera_speed_presets)
			if (words == camera_speed_words(preset))
				return preset;
		return 0.0f;
	}

	bool set_camera_speed(const pcsx2& ps2, float multiplier)
	{
		bool supported = false;
		for (float preset : camera_speed_presets)
			supported |= multiplier == preset;
		if (!supported || (multiplier != 1.0f && !fly_enabled))
			return false;

		const float current = read_camera_speed(ps2);
		if (current == 0.0f)
		{
			camera_speed_available = false;
			camera_speed_message = "Camera speed unavailable: movement code has changed.";
			return false;
		}
		if (current != multiplier)
		{
			const auto words = camera_speed_words(multiplier);
			ps2_ipc_cmd write(ps2);
			for (size_t i = 0; i < camera_speed_addresses.size(); ++i)
			{
				write.write<uint32_t>(camera_speed_addresses[i], words[i * 2]);
				write.write<uint32_t>(camera_speed_addresses[i] + 4, words[i * 2 + 1]);
			}
			write.send();
			if (read_camera_speed(ps2) != multiplier)
			{
				camera_speed_available = false;
				camera_speed_message = "Camera speed change could not be confirmed.";
				return false;
			}
			sentinel.increment();
		}
		camera_speed_multiplier = multiplier;
		camera_speed_available = true;
		camera_speed_message = nullptr;
		return true;
	}

	void update_camera_speed(const pcsx2& ps2)
	{
		const float current = read_camera_speed(ps2);
		camera_speed_available = current != 0.0f;
		if (!camera_speed_available)
		{
			camera_speed_message = "Camera speed unavailable: movement code has changed.";
			return;
		}
		camera_speed_multiplier = current;
		camera_speed_message = nullptr;
		if (!fly_enabled && current != 1.0f)
			set_camera_speed(ps2, 1.0f);
	}

	void draw_camera_speed_ui(const pcsx2& ps2)
	{
		ImGui::PushID("camera_speed");
		if (camera_speed_available)
			ImGui::Text("Camera speed: %.3gx", camera_speed_multiplier);
		else
			ImGui::TextUnformatted("Camera speed unavailable");
		ImGui::BeginDisabled(!fly_enabled || !camera_speed_available ||
			debug_state != debug_command_state::idle);
		ImGui::BeginDisabled(camera_speed_multiplier <= camera_speed_presets.front());
		if (ImGui::Button("Decrease speed"))
			set_camera_speed(ps2, camera_speed_multiplier * 0.5f);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(camera_speed_multiplier >= camera_speed_presets.back());
		if (ImGui::Button("Increase speed"))
			set_camera_speed(ps2, camera_speed_multiplier * 2.0f);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Reset speed"))
			set_camera_speed(ps2, 1.0f);
		ImGui::EndDisabled();
		if (!fly_enabled)
			ImGui::TextUnformatted("Enable Debug Fly to adjust camera speed.");
		if (camera_speed_message != nullptr)
			ImGui::TextUnformatted(camera_speed_message);
		ImGui::PopID();
	}

	bool render_distance_reset_pending = true;
	float render_distance_reset_stable_time = 0.0f;
	std::array<uint32_t, 6> render_distance_reset_snapshot = {};

	void request_render_distance_reset()
	{
		render_distance_reset_pending = true;
		render_distance_reset_stable_time = 0.0f;
		render_distance_reset_snapshot = {};
		render_distance_available = false;
		render_distance_editor_initialized = false;
		render_distance_default = 0.0f;
		render_manager_address = 0;
		render_camera_address = 0;
		render_level_properties = 0;
		render_distance_message = nullptr;
	}

	float render_distance = 0.0f;
	float render_distance_edit = 0.0f;
	float render_distance_default = 0.0f;
	uint32_t render_manager_address = 0;
	uint32_t render_camera_address = 0;
	uint32_t render_level_properties = 0;
	bool render_distance_available = false;
	bool render_distance_editor_initialized = false;
	float render_fog_width = 0.0f;
	const char* render_distance_message = nullptr;
	static constexpr uint32_t view_camera_global = 0x00555A48;
	static constexpr uint32_t level_properties_offset = 0x4C0;
	static constexpr float world_units_per_lu = 2048.0f;
	struct sector_instruction
	{
		uint32_t address;
		uint32_t original;
		uint32_t extended;
	};
	// Consider every terrain sector, then let the game's distance and frustum checks filter them.
	static constexpr std::array<sector_instruction, 4> terrain_sector_patch = {{
		{ 0x00152428, 0x8C7E0078, 0x8EDE0010 }, // Use the total sector count.
		{ 0x001524EC, 0x8C430070, 0x00000000 }, // Skip the current sector's candidate list.
		{ 0x001524F0, 0x00641821, 0x00000000 },
		{ 0x001524F4, 0x94620000, 0x0200102D }, // Use the loop index as the sector ID.
	}};

	std::array<uint32_t, 4> read_terrain_sector_code(const pcsx2& ps2) const
	{
		ps2_ipc_cmd read(ps2);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, 4> words;
		for (size_t i = 0; i < words.size(); ++i)
			words[i] = read.queue_read<uint32_t>(terrain_sector_patch[i].address);
		read.send();
		std::array<uint32_t, 4> code;
		for (size_t i = 0; i < code.size(); ++i)
			code[i] = read.read(words[i]);
		return code;
	}

	static bool valid_ee_address(uint32_t address, uint32_t size)
	{
		return size <= 0x02000000 && address >= 0x00100000 && address % 4 == 0 &&
			address <= 0x02000000 - size;
	}

	static bool valid_render_distance(float distance)
	{
		return std::isfinite(distance) && distance > 32.0f && distance <= 10000000.0f;
	}

	template<typename T>
	static T read_ee_value(const pcsx2& ps2, uint32_t address)
	{
		ps2_ipc_cmd read(ps2);
		auto value = read.queue_read<T>(address);
		read.send();
		return read.read(value);
	}

	static bool valid_distance_and_fog(float distance, float width)
	{
		return valid_render_distance(distance) && std::isfinite(width) &&
			width > 0.0f && width <= distance;
	}

	// Property 92 is the loaded level's Max Fog Depth, in level units.
	// Sparse overrides fall back to the level's full property array.
	float read_default_render_distance(const pcsx2& ps2, uint32_t properties) const
	{
		if (!valid_ee_address(properties, 4))
			return 0.0f;
		auto object = read_ee_value<uint32_t>(ps2, properties);
		for (int depth = 0; depth < 4 && valid_ee_address(object, 24); ++depth)
		{
			ps2_ipc_cmd header_read(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 6> header;
			for (size_t i = 0; i < header.size(); ++i)
				header[i] = header_read.queue_read<uint32_t>(object + static_cast<uint32_t>(i * 4));
			header_read.send();
			const auto count = header_read.read(header[0]);
			const auto values = header_read.read(header[1]);
			const auto vtable = header_read.read(header[2]);
			uint32_t value_address = 0;
			if (vtable == 0x0051B0D0 && count > 92 && count <= 1024 &&
				valid_ee_address(values, count * 4))
			{
				value_address = values + 92 * 4;
			}
			else if (vtable == 0x0051B098)
			{
				const auto override_count = header_read.read(header[4]);
				const auto indices = header_read.read(header[5]);
				if (override_count > 128 || (override_count != 0 &&
					(!valid_ee_address(indices, override_count * 2) ||
					 !valid_ee_address(values, override_count * 4))))
					return 0.0f;
				if (override_count != 0)
				{
					ps2_ipc_cmd indices_read(ps2);
					std::array<ps2_ipc_cmd::queued_read<uint16_t>, 128> entries;
					for (uint32_t i = 0; i < override_count; ++i)
						entries[i] = indices_read.queue_read<uint16_t>(indices + i * 2);
					indices_read.send();
					for (uint32_t i = 0; i < override_count; ++i)
						if (indices_read.read(entries[i]) == 92)
							value_address = values + i * 4;
				}
				object = header_read.read(header[3]);
			}
			else
				return 0.0f;
			if (value_address != 0)
			{
				const float distance = read_ee_value<int32_t>(ps2, value_address) * world_units_per_lu;
				return valid_render_distance(distance) ? distance : 0.0f;
			}
		}
		return 0.0f;
	}

	void update_render_distance(const pcsx2& ps2)
	{
		render_distance_available = false;
		ps2_ipc_cmd pointers_read(ps2);
		auto manager = pointers_read.queue_read<uint32_t>(manager_global);
		auto camera = pointers_read.queue_read<uint32_t>(view_camera_global);
		pointers_read.send();
		const auto manager_address = pointers_read.read(manager);
		const auto camera_address = pointers_read.read(camera);
		if (!player_position_available || !valid_ee_address(manager_address, 0x524) ||
			!valid_ee_address(camera_address, 0xD8))
		{
			render_level_properties = 0;
			render_distance_default = 0.0f;
			render_distance_editor_initialized = false;
			return;
		}

		ps2_ipc_cmd values_read(ps2);
		auto far_distance = values_read.queue_read<float>(manager_address + 0x394);
		auto fog_width = values_read.queue_read<float>(manager_address + 0x3A8);
		auto properties = values_read.queue_read<uint32_t>(manager_address + level_properties_offset);
		values_read.send();
		const float distance = values_read.read(far_distance);
		const float width = values_read.read(fog_width);
		if (!valid_distance_and_fog(distance, width))
		{
			render_distance_default = 0.0f;
			render_distance_editor_initialized = false;
			return;
		}

		const auto level_properties = values_read.read(properties);
		const bool level_changed = manager_address != render_manager_address ||
			level_properties != render_level_properties;
		if (level_changed || render_distance_default <= 0.0f)
			render_distance_default = read_default_render_distance(ps2, level_properties);
		if (level_changed)
		{
			render_distance_editor_initialized = false;
			render_distance_message = nullptr;
		}
		render_manager_address = manager_address;
		render_camera_address = camera_address;
		render_level_properties = level_properties;
		render_distance = distance;
		render_fog_width = width;
		render_distance_available = true;
		if (!render_distance_editor_initialized)
		{
			render_distance_edit = distance;
			render_distance_editor_initialized = true;
		}
	}

	struct distance_field
	{
		uint32_t offset;
		float value;
	};

	static bool make_render_distance_values(float distance, float current_distance,
		float current_width, std::array<distance_field, 10>& fields)
	{
		if (!valid_render_distance(distance) || !valid_distance_and_fog(current_distance, current_width))
			return false;
		const float width = distance * (current_width / current_distance);
		const float start = distance - width;
		const float distance_squared = distance * distance;
		const float start_squared = start * start;
		const float squared_span = distance_squared - start_squared;
		if (width <= 0.0f || squared_span <= 0.0f ||
			!std::isfinite(1.0f / width) || !std::isfinite(1.0f / squared_span))
			return false;

		// Update main distance and fog settings; leave secondary distance unchanged.
		fields = {{
			{ 0x388, distance / world_units_per_lu },
			{ 0x38C, distance_squared },
			{ 0x394, distance },
			{ 0x398, start_squared },
			{ 0x39C, distance_squared },
			{ 0x3A0, 1.0f / squared_span },
			{ 0x3A4, distance },
			{ 0x3A8, width },
			{ 0x3AC, 1.0f / width },
			{ 0x3B0, start },
		}};
		return true;
	}

	// This vtable callback runs before camera rebuild and terrain rendering. Keep the
	// executor immutable: PINE writes only its mailbox, on a separate 4 KiB page.
	static constexpr uint32_t render_callback = 0x00529A5C;
	static constexpr uint32_t render_original_callback = 0x003B1DE0;
	static constexpr uint32_t render_allocation = 0x00555C50;
	static constexpr uint32_t render_bootstrap_address = 0x00555CAC;
	static constexpr std::array<uint32_t, 18> render_bootstrap = {{
		// Linker padding in .sbss; allocate once using the game's malloc(0x3000).
		0x3C080055, 0x8D025C50, 0x14400009, 0x00000000,
		0x27BDFFF0, 0x7FBF0000, 0x0C12AF80, 0x34043000,
		0x3C080055, 0xAD025C50, 0x7BBF0000, 0x27BD0010,
		// Restore the callback and tail-call it, preserving the caller's ra.
		0x3C080053, 0x3C09003B, 0x35291DE0, 0xAD099A5C,
		0x080EC778, 0x00000000,
	}};

	template<size_t N>
	static std::array<uint32_t, N> read_render_program(const pcsx2& ps2, uint32_t address)
	{
		std::array<uint32_t, N> words;
		for (size_t first = 0; first < N; first += 128)
		{
			ps2_ipc_cmd read(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> queued;
			const auto count = std::min(size_t(128), N - first);
			for (size_t i = 0; i < count; ++i)
				queued[i] = read.queue_read<uint32_t>(address + static_cast<uint32_t>((first + i) * 4));
			read.send();
			for (size_t i = 0; i < count; ++i)
				words[first + i] = read.read(queued[i]);
		}
		return words;
	}

	uint32_t prepare_render_mailbox(const pcsx2& ps2)
	{
		auto allocation = read_ee_value<uint32_t>(ps2, render_allocation);
		auto callback = read_ee_value<uint32_t>(ps2, render_callback);
		if (allocation == 0)
		{
			if (callback != render_original_callback && callback != render_bootstrap_address)
				return 0;
			const auto existing = read_render_program<render_bootstrap.size()>(ps2, render_bootstrap_address);
			if (existing != render_bootstrap)
			{
				if (callback != render_original_callback || existing != decltype(existing){})
					return 0;
				ps2_ipc_cmd install(ps2);
				for (size_t i = 0; i < render_bootstrap.size(); ++i)
					install.write<uint32_t>(render_bootstrap_address + static_cast<uint32_t>(i * 4), render_bootstrap[i]);
				install.send();
				if (read_render_program<render_bootstrap.size()>(ps2, render_bootstrap_address) != render_bootstrap)
					return 0;
			}
			ps2_ipc_cmd install_hook(ps2);
			install_hook.write<uint32_t>(render_callback, render_bootstrap_address).send();
			for (int wait = 0; wait < 100; ++wait)
			{
				allocation = read_ee_value<uint32_t>(ps2, render_allocation);
				callback = read_ee_value<uint32_t>(ps2, render_callback);
				if (callback == render_original_callback)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (callback != render_original_callback)
				return 0;
		}
		if (!valid_ee_address(allocation, 0x3000))
			return 0;
		if (sentinel.has_reset(false))
			return 0;
		const auto code = (allocation + 0xFFF) & ~0xFFFu;
		const auto mailbox = code + 0x1000;
		const std::array<uint32_t, 31> executor = {{
			0x3C080000 | (mailbox >> 16), 0x35080000 | (mailbox & 0xFFFF),
			0x8D090000, 0x19200015, 0x00000000, // Idle: tail-call the original callback.
			0x2D220010, 0x10400014, 0x250A0020, 0x240B000A, // At most 15 writes, ten guards.
			0x8D420000, 0x8D430004, 0x8C420000, 0x1443000E, 0x254A0008,
			0x256BFFFF, 0x1560FFF9, 0x00000000, // Reject before writing if any guard differs.
			0x250A0070, 0x8D420000, 0x8D430004, 0xAC430000,
			0x2529FFFF, 0x1520FFFB, 0x254A0008, // Apply the complete batch on the EE thread.
			0xAD000000, 0x080EC778, 0x00000000, // Acknowledge success.
			0x2409FFFF, 0xAD090000, 0x080EC778, 0x00000000, // Acknowledge rejection.
		}};
		// A state may have been saved after malloc but before publishing the
		// executor. Finish that installation only while it is still unreachable.
		if (callback == render_original_callback)
		{
			if (read_render_program<render_bootstrap.size()>(ps2, render_bootstrap_address) != render_bootstrap)
				return 0;
			ps2_ipc_cmd install(ps2);
			for (size_t i = 0; i < executor.size(); ++i)
				install.write<uint32_t>(code + static_cast<uint32_t>(i * 4), executor[i]);
			install.write<uint32_t>(mailbox, 0).send();
		}
		if (read_render_program<executor.size()>(ps2, code) != executor ||
			(callback != render_original_callback && callback != code))
			return 0;
		if (callback != code)
		{
			ps2_ipc_cmd install_hook(ps2);
			install_hook.write<uint32_t>(render_callback, code).send();
		}
		return mailbox;
	}

	bool queue_render_distance(const pcsx2& ps2, const std::array<distance_field, 10>& fields,
		bool extend_sectors, const std::array<uint32_t, 4>& terrain_code)
	{
		const auto mailbox = prepare_render_mailbox(ps2);
		if (sentinel.has_reset(false))
		{
			request_render_distance_reset();
			return false;
		}
		if (mailbox == 0)
		{
			render_distance_message = "Draw distance is unavailable.";
			return false;
		}
		if (static_cast<int32_t>(read_ee_value<uint32_t>(ps2, mailbox)) > 0)
		{
			render_distance_message = "Updating draw distance...";
			return false;
		}
		const std::array<std::array<uint32_t, 2>, 10> guards = {{
			{ load_sentinel_address, read_ee_value<uint32_t>(ps2, load_sentinel_address) },
			{ manager_global, render_manager_address }, { view_camera_global, render_camera_address },
			{ render_manager_address + level_properties_offset, render_level_properties },
			{ render_manager_address + 0x394, std::bit_cast<uint32_t>(render_distance) },
			{ render_manager_address + 0x3A8, std::bit_cast<uint32_t>(render_fog_width) },
			{ terrain_sector_patch[0].address, terrain_code[0] },
			{ terrain_sector_patch[1].address, terrain_code[1] },
			{ terrain_sector_patch[2].address, terrain_code[2] },
			{ terrain_sector_patch[3].address, terrain_code[3] },
		}};
		ps2_ipc_cmd queue(ps2);
		for (size_t i = 0; i < guards.size(); ++i)
		{
			queue.write<uint32_t>(mailbox + 0x20 + static_cast<uint32_t>(i * 8), guards[i][0]);
			queue.write<uint32_t>(mailbox + 0x24 + static_cast<uint32_t>(i * 8), guards[i][1]);
		}
		uint32_t count = 0;
		auto write = [&](uint32_t address, uint32_t value) {
			queue.write<uint32_t>(mailbox + 0x70 + count * 8, address);
			queue.write<uint32_t>(mailbox + 0x74 + count++ * 8, value);
		};
		for (size_t i = 0; i < terrain_sector_patch.size(); ++i)
		{
			const auto& instruction = terrain_sector_patch[i];
			const auto desired = extend_sectors ? instruction.extended : instruction.original;
			if (terrain_code[i] != desired)
				write(instruction.address, desired);
		}
		for (const auto& field : fields)
			write(render_manager_address + field.offset, std::bit_cast<uint32_t>(field.value));
		write(render_camera_address + 0xD4, 1);
		queue.send();
		if (sentinel.has_reset(false))
		{
			request_render_distance_reset();
			return false;
		}
		ps2_ipc_cmd publish(ps2);
		publish.write<uint32_t>(mailbox, count).send(); // Publish only after the batch is complete.
		for (int wait = 0; wait < 100; ++wait)
		{
			const auto remaining = read_ee_value<uint32_t>(ps2, mailbox);
			if (remaining == 0)
				return true;
			if (remaining == 0xFFFFFFFF)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		// Never reuse an unacknowledged batch. The next sentinel update expires it.
		render_distance_message = "Could not change draw distance. Try again.";
		return false;
	}

	enum class distance_action { apply, double_current, reset };

	bool apply_render_distance(const pcsx2& ps2, float distance,
		distance_action action = distance_action::apply, bool recovering = false)
	{
		// Observe without consuming the load notification needed by update().
		if (sentinel.has_reset(false))
		{
			request_render_distance_reset();
			return false;
		}
		if (render_distance_reset_pending && !recovering)
			return false;
		update_render_distance(ps2);
		if (!render_distance_available || debug_state != debug_command_state::idle)
			return false;
		if (action == distance_action::double_current)
			distance = render_distance * 2.0f;
		else if (action == distance_action::reset)
		{
			render_distance_default = read_default_render_distance(ps2, render_level_properties);
			if (render_distance_default <= 0.0f)
			{
				render_distance_message = "Level default is unavailable.";
				return false;
			}
			distance = render_distance_default;
		}
		std::array<distance_field, 10> fields;
		if (!make_render_distance_values(distance, render_distance, render_fog_width, fields))
		{
			render_distance_message = "Enter a distance above 32 and at most 10000000.";
			return false;
		}

		const auto manager = render_manager_address;
		const auto camera = render_camera_address;
		const auto properties = render_level_properties;
		const auto previous_distance = render_distance;
		const auto previous_width = render_fog_width;
		const auto code = read_terrain_sector_code(ps2);
		for (size_t i = 0; i < code.size(); ++i)
		{
			if (code[i] != terrain_sector_patch[i].original && code[i] != terrain_sector_patch[i].extended)
			{
				render_distance_message = "Draw distance is unavailable for this game state.";
				return false;
			}
		}
		// A state load/level switch can occur between IPC batches. Recheck before writing.
		update_render_distance(ps2);
		if (sentinel.has_reset(false))
		{
			request_render_distance_reset();
			return false;
		}
		if (!render_distance_available ||
			manager != render_manager_address || camera != render_camera_address ||
			properties != render_level_properties || previous_distance != render_distance ||
			previous_width != render_fog_width ||
			(action == distance_action::reset &&
			 distance != read_default_render_distance(ps2, render_level_properties)))
		{
			// Cancel this action; the recovery caller handles waiting and retrying.
			render_distance_message = recovering ? nullptr :
				"Could not change draw distance. Try again.";
			return false;
		}
		const bool extend_sectors = action != distance_action::reset;
		// Guest stores at the pre-render callback keep recompiler invalidation on
		// the EE thread and finish the mode switch before any terrain traversal.
		if (ps2.get_ipc()->Status() != PINE::PCSX2::Running)
		{
			if (!recovering)
				render_distance_message = "Resume the game to change draw distance.";
			return false;
		}
		if (!queue_render_distance(ps2, fields, extend_sectors, code))
			return false;
		// Preserve a load notification if a state was restored during the write batch.
		if (sentinel.has_reset(false))
		{
			request_render_distance_reset();
			return false;
		}
		sentinel.increment();
		update_render_distance(ps2);
		render_distance_edit = distance;
		render_distance_message = render_distance_available && render_distance == distance ?
			nullptr : "Could not change draw distance. Try again.";
		ps2_ipc_cmd verify(ps2);
		std::array<ps2_ipc_cmd::queued_read<float>, 10> applied_fields;
		for (size_t i = 0; i < fields.size(); ++i)
			applied_fields[i] = verify.queue_read<float>(manager + fields[i].offset);
		verify.send();
		for (size_t i = 0; i < fields.size(); ++i)
			if (verify.read(applied_fields[i]) != fields[i].value)
				render_distance_message = "Could not change draw distance. Try again.";
		const auto applied_code = read_terrain_sector_code(ps2);
		for (size_t i = 0; i < applied_code.size(); ++i)
		{
			const auto expected = extend_sectors ? terrain_sector_patch[i].extended : terrain_sector_patch[i].original;
			if (applied_code[i] != expected)
				render_distance_message = "Could not change draw distance. Try again.";
		}
		return render_distance_message == nullptr;
	}

	void update_render_distance_recovery(const pcsx2& ps2, float time_delta)
	{
		if (!render_distance_reset_pending)
			return;
		if (!render_distance_available || debug_state != debug_command_state::idle)
		{
			render_distance_reset_stable_time = 0.0f;
			render_distance_reset_snapshot = {};
			return;
		}
		// Resolve the loaded level again, even if its property-holder address was reused.
		render_distance_default = read_default_render_distance(ps2, render_level_properties);
		if (render_distance_default <= 0.0f)
		{
			render_distance_reset_stable_time = 0.0f;
			return;
		}
		const std::array<uint32_t, 6> snapshot = { render_manager_address, render_camera_address,
			render_level_properties, std::bit_cast<uint32_t>(render_distance),
			std::bit_cast<uint32_t>(render_fog_width), std::bit_cast<uint32_t>(render_distance_default) };
		if (snapshot != render_distance_reset_snapshot)
		{
			render_distance_reset_snapshot = snapshot;
			render_distance_reset_stable_time = 0.0f;
			return;
		}
		if (std::isfinite(time_delta) && time_delta > 0.0f)
			render_distance_reset_stable_time += std::min(time_delta, 0.05f);
		if (render_distance_reset_stable_time < 0.25f)
			return;
		render_distance_message = nullptr;
		if (apply_render_distance(ps2, 0.0f, distance_action::reset, true))
			render_distance_reset_pending = false;
		else
		{
			// Loads, rejected guards and delayed callbacks can interrupt a reset.
			// Wait for a fresh stable snapshot, then validate/reinstall and retry.
			render_distance_reset_stable_time = 0.0f;
			render_distance_reset_snapshot = {};
		}
	}

	void draw_render_distance_ui(const pcsx2& ps2)
	{
		ImGui::Separator();
		ImGui::PushID("render_distance");
		ImGui::Text("Draw distance and fog");
		if (render_distance_available)
			ImGui::Text("Current: %.3f", render_distance);
		else
			ImGui::TextUnformatted("Current: unavailable");
		if (render_distance_default > 0.0f)
			ImGui::Text("Level default: %.3f", render_distance_default);
		if (render_distance_reset_pending)
			ImGui::TextUnformatted("Resetting draw distance...");
		ImGui::BeginDisabled(!render_distance_available || render_distance_reset_pending ||
			debug_state != debug_command_state::idle);
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputFloat("Distance", &render_distance_edit, 0.0f, 0.0f, "%.3f");
		if (ImGui::Button("Apply"))
			apply_render_distance(ps2, render_distance_edit);
		ImGui::SameLine();
		if (ImGui::Button("Double"))
			apply_render_distance(ps2, 0.0f, distance_action::double_current);
		ImGui::SameLine();
		ImGui::BeginDisabled(render_distance_default <= 0.0f);
		if (ImGui::Button("Reset"))
			apply_render_distance(ps2, 0.0f, distance_action::reset);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (render_distance_message != nullptr)
			ImGui::TextUnformatted(render_distance_message);
		ImGui::PopID();
	}

	void set_jump_height(const pcsx2& ps2, double multiplier)
	{
		// Height scales with launch velocity squared; gravity stays unchanged.
		const double velocity = 2850.0 * std::sqrt(multiplier);
		jump_height_invalid = !std::isfinite(multiplier) || multiplier <= 0.0 ||
			velocity < std::numeric_limits<float>::min() || velocity > std::numeric_limits<float>::max();
		if (jump_height_invalid)
			return;
		const uint32_t bits = std::bit_cast<uint32_t>(static_cast<float>(velocity));
		ps2_ipc_cmd write(ps2);
		write.write<uint32_t>(0x002FB3A4, 0x3C010000 | (bits >> 16));
		write.write<uint32_t>(0x002FB3A8, 0x34210000 | (bits & 0xFFFF));
		write.send();
		sentinel.increment();
		jump_height = multiplier;
		jump_height_applied = multiplier;
	}

	bool update_player_position(const pcsx2& ps2)
	{
		const auto manager_address = read_ee_value<uint32_t>(ps2, manager_global);

		if (manager_address == 0)
		{
			player_position_address = 0;
			player_position_available = false;
			return false;
		}

		const auto player_address = read_ee_value<uint32_t>(ps2, manager_address + player_pointer_offset);

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
		const auto manager_address = read_ee_value<uint32_t>(ps2, manager_global);

		if (manager_address == 0)
		{
			camera_position_available = false;
			return false;
		}

		const auto camera_address = read_ee_value<uint32_t>(ps2, manager_address + camera_pointer_offset);

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
		const auto relation_address = read_ee_value<uint32_t>(ps2, camera_address + actor_relation_offset);
		if (relation_address == 0)
			return;

		const auto control_address = read_ee_value<uint32_t>(ps2, relation_address + relation_control_offset);
		if (control_address == 0)
			return;

		fly_enabled = read_ee_value<uint32_t>(ps2, control_address + controller_mode_offset) == fly_mode;
	}

	// Retail USA retained debug-command wrapper. It submits FLY, DROP, HUD or HENDRIX once
	// and restores the update hook after the game has returned from it.
	static constexpr uint32_t debug_update_hook = 0x00135DC8;
	static constexpr uint32_t debug_expected_hook = 0x0C075748;
	static constexpr uint32_t debug_temporary_hook = 0x0C7FC000;
	static constexpr uint32_t debug_code_cave = 0x01FF0000;
	static constexpr uint32_t debug_command_buffer = 0x01FF0100;
	static constexpr uint32_t debug_completion_latch = 0x01FF0180;
	static constexpr uint32_t fly_command = 0x00796C66; // "fly\\0"
	static constexpr uint32_t drop_command = 0x706F7264; // "drop", followed by a zero word.
	static constexpr uint32_t hud_command = 0x00647568; // "hud\\0"
	static constexpr uint32_t hendrix_command = 0x646E6568; // "hend"
	static constexpr uint32_t hendrix_command_tail = 0x00786972; // "rix" and terminator.
	static constexpr std::array<uint32_t, 30> debug_wrapper = { {
		0x27BDFFD0, 0x7FBF0020, 0xAFA40000, 0x0C075748, 0x00000000,
		0xAFA20004, 0x3C0801FF, 0x8D090180, 0x15200011, 0x00000000,
		0x3C080055, 0x8D085A54, 0x1100000D, 0x00000000, 0x8D0804B0,
		0x1100000A, 0x00000000, 0x24090001, 0x3C0801FF, 0xAD090180,
		0x3C0401FF, 0x0C0900EE, 0x34840100, 0x24090002, 0x3C0801FF,
		0xAD090180, 0x8FA20004, 0x7BBF0020, 0x03E00008, 0x27BD0030,
	} };

	// Native dragon teleport/enable wrapper; shares the debug command busy state.
	// Resolves companion handles in-game and preserves all 60 transform bytes.
	static constexpr uint32_t arokh_code_cave = 0x01FF0200;
	static constexpr uint32_t arokh_temporary_hook = 0x0C7FC080;
	static constexpr uint32_t arokh_workspace_size = 0x600;
	static constexpr uint32_t arokh_completion_latch = 0x01FF0780;
	static constexpr uint32_t arokh_enable_event = 0x01FF0790;
	static constexpr uint32_t arokh_enable_payload = 0x01FF07A0;

	enum class debug_command_state
	{
		idle,
		waiting_for_fly,
		waiting_for_drop,
		waiting_for_hud,
		waiting_for_hendrix,
		waiting_for_arokh,
		waiting_for_arokh_spawn,
		waiting_for_arokh_power,
	};

	debug_command_state debug_state = debug_command_state::idle;
	bool fly_enabled = false;
	bool arokh_missing = false;
	const char* arokh_message = nullptr;
	const char* debug_command_message = nullptr;

	enum class arokh_action { teleport, spawn, give_power, remove_power };
	enum class arokh_result : uint32_t
	{
		ready = 0,
		running = 1,
		complete = 2,
		unavailable = 3,
		missing = 4,
		already_exists = 5,
	};

	struct arokh_power_option
	{
		const char* name;
		const char* command;
		uint32_t primary_key;
		uint32_t resource_class = 0; // Nonzero selects a native projectile grant.
		uint32_t hud_slot = 0;
	};
	// Common resource keys, not disc record IDs or session-specific indices.
	static constexpr std::array<arokh_power_option, 12> arokh_power_options = {{
		{ "Fire", "GIVE Fire", 0x0003340A },
		{ "Ice", "GIVE Ice", 0x00033524 },
		{ "Lightning", "GIVE Lightning", 0x000329EF },
		{ "Poison / Darkness", "GIVE Poison", 0x00032665 },
		{ "Lava Ball", nullptr, 0x00033801, 0x10008, 0 },
		{ "Sea Dragon Lightning Ball", nullptr, 0x00033A20, 0x10055, 3 },
		{ "Grotus Magic", nullptr, 0x0003408C, 0x1006C, 0 },
		{ "Crow Fire Breath", nullptr, 0x00032C74, 0x10008, 0 },
		{ "Snow Crow Fireball", nullptr, 0x00032CEC, 0x10008, 0 },
		{ "Skeleton Dragon Lightning", nullptr, 0x00032DB6, 0x1003A, 3 },
		{ "Sonic Boom 01", nullptr, 0x00032E40, 0x10008, 1 },
		{ "Sonic Boom 02", nullptr, 0x00032E5C, 0x10008, 1 },
	}};
	struct arokh_power_record
	{
		uint32_t primary;
		uint32_t secondary;
		uint32_t hud;
		uint32_t flags;
		uint32_t primary_key;
	};
	struct arokh_power_snapshot
	{
		uint32_t manager = 0;
		uint32_t active_actor = 0;
		uint32_t controller = 0;
		uint32_t records = 0;
		uint32_t count = 0;
		uint32_t capacity = 0;
		int32_t selected = -1;
		std::array<arokh_power_record, 64> powers = {};
	};
	arokh_power_snapshot arokh_powers;
	arokh_power_snapshot arokh_remove_before;
	int arokh_remove_index = -1;
	bool arokh_powers_available = false;
	float arokh_power_refresh_time = 0.0f;
	int arokh_power_selection = 0;
	int arokh_power_requested = -1;
	char arokh_power_message[160] = {};
	static constexpr uint32_t arokh_power_command_buffer = 0x01FF0700;
	static constexpr uint32_t arokh_power_request = 0x01FF07C0;

	uint32_t actor_controller(const pcsx2& ps2, uint32_t actor) const
	{
		if (!valid_ee_address(actor, 0x100))
			return 0;
		const auto relation = read_ee_value<uint32_t>(ps2, actor + 0xFC);
		if (!valid_ee_address(relation, 0x14))
			return 0;
		const auto control = read_ee_value<uint32_t>(ps2, relation + 0x10);
		return valid_ee_address(control, 0x6B4) ? control : 0;
	}

	// Read-only equivalent of native dragon lookup 0x003D2478. Mounted control
	// already points at Arokh; on foot resolve Rynn's companion handle.
	uint32_t resolve_arokh_controller(const pcsx2& ps2, uint32_t manager,
		uint32_t active, uint32_t rows, uint32_t resource_count) const
	{
		if (!valid_ee_address(active, 0x100))
			return 0;
		const auto resource = read_ee_value<uint16_t>(ps2, active + 0x10);
		if (resource >= resource_count)
			return 0;
		const auto type = read_ee_value<uint32_t>(ps2, rows + resource * 36 + 8);
		auto actor = active;
		auto control = actor_controller(ps2, actor);
		if (control == 0)
			return 0;
		if (type == 0x10009)
		{
			const auto handle = read_ee_value<uint16_t>(ps2, control + 0x4D8);
			const auto last_handle_index = read_ee_value<uint32_t>(ps2, manager + 0x84);
			const auto handles = read_ee_value<uint32_t>(ps2, manager + 0x7C);
			if (handle == 0 || last_handle_index >= 0x10000 ||
				handle > last_handle_index + 1 || !valid_ee_address(handles, (last_handle_index + 1) * 8))
				return 0;
			actor = read_ee_value<uint32_t>(ps2, handles + (handle - 1) * 8);
			if (!valid_ee_address(actor, 0x100))
				return 0;
			const auto dragon_resource = read_ee_value<uint16_t>(ps2, actor + 0x10);
			if (dragon_resource >= resource_count ||
				read_ee_value<uint32_t>(ps2, rows + dragon_resource * 36 + 8) != 0x1001C)
				return 0;
			control = actor_controller(ps2, actor);
		}
		else if (type != 0x1001C)
			return 0;
		if (control == 0 || read_ee_value<uint32_t>(ps2, control + 0x80) != actor ||
			read_ee_value<uint32_t>(ps2, control + 0x57C) != 0x00525C58)
			return 0;
		// Mounted Arokh has no companion-active bit. Accept the verified active
		// dragon actor directly; on foot still require an active companion.
		const auto flags = read_ee_value<uint32_t>(ps2, control + 0x94);
		if (((flags & 1) == 0 && actor != active) || (flags & 2) != 0)
			return 0;
		return control;
	}

	bool read_arokh_powers(const pcsx2& ps2, arokh_power_snapshot& snapshot) const
	{
		arokh_power_snapshot next;
		next.manager = read_ee_value<uint32_t>(ps2, manager_global);
		if (!valid_ee_address(next.manager, 0x524))
			return false;
		const auto registry = read_ee_value<uint32_t>(ps2, next.manager + 0x320);
		if (!valid_ee_address(registry, 0x30))
			return false;
		const auto rows = read_ee_value<uint32_t>(ps2, registry + 8);
		const auto resources = read_ee_value<uint32_t>(ps2, registry + 0x10);
		if (resources == 0 || resources > 20000 || !valid_ee_address(rows, resources * 36))
			return false;
		next.active_actor = read_ee_value<uint32_t>(ps2, next.manager + 0x4B0);
		next.controller = resolve_arokh_controller(ps2, next.manager, next.active_actor, rows, resources);
		if (next.controller == 0)
			return false;
		const auto header = read_render_program<4>(ps2, next.controller + 0x6A4);
		next.selected = static_cast<int32_t>(header[0]);
		next.records = header[1];
		next.count = header[3];
		next.capacity = header[2];
		if (next.count > next.powers.size() || header[2] < next.count || header[2] > 65536 ||
			next.selected < -1 || (next.selected >= 0 && static_cast<uint32_t>(next.selected) >= next.count) ||
			(next.count != 0 && !valid_ee_address(next.records, next.count * 16)))
			return false;
		// 32 records per IPC batch keeps the command count below 256.
		for (uint32_t first = 0; first < next.count; first += 32)
		{
			ps2_ipc_cmd read(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> words;
			const auto count = std::min(32u, next.count - first);
			for (uint32_t i = 0; i < count * 4; ++i)
				words[i] = read.queue_read<uint32_t>(next.records + first * 16 + i * 4);
			read.send();
			for (uint32_t i = 0; i < count; ++i)
			{
				auto& power = next.powers[first + i];
				power = { read.read(words[i * 4]), read.read(words[i * 4 + 1]),
					read.read(words[i * 4 + 2]), read.read(words[i * 4 + 3]), 0 };
				if (power.primary >= resources || (power.secondary != 0xFFFFFFFF && power.secondary >= resources))
					return false;
			}
		}
		if (next.count != 0)
		{
			ps2_ipc_cmd read_keys(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 64> keys;
			for (uint32_t i = 0; i < next.count; ++i)
				keys[i] = read_keys.queue_read<uint32_t>(rows + next.powers[i].primary * 36);
			read_keys.send();
			for (uint32_t i = 0; i < next.count; ++i)
				next.powers[i].primary_key = read_keys.read(keys[i]);
		}
		// A load or list reallocation between batches invalidates the snapshot.
		if (read_ee_value<uint32_t>(ps2, manager_global) != next.manager ||
			read_ee_value<uint32_t>(ps2, next.manager + 0x4B0) != next.active_actor ||
			read_ee_value<uint32_t>(ps2, next.manager + 0x320) != registry ||
			read_ee_value<uint32_t>(ps2, registry + 8) != rows ||
			read_ee_value<uint32_t>(ps2, registry + 0x10) != resources ||
			resolve_arokh_controller(ps2, next.manager, next.active_actor, rows, resources) != next.controller ||
			read_render_program<4>(ps2, next.controller + 0x6A4) != header)
			return false;
		snapshot = next;
		return true;
	}

	void refresh_arokh_powers(const pcsx2& ps2)
	{
		arokh_powers_available = player_position_available && read_arokh_powers(ps2, arokh_powers);
		if (!arokh_powers_available)
			arokh_powers = {};
		arokh_power_refresh_time = 0.0f;
	}

	bool arokh_has_power(uint32_t key) const
	{
		for (uint32_t i = 0; i < arokh_powers.count; ++i)
			if (arokh_powers.powers[i].primary_key == key)
				return true;
		return false;
	}

	static const char* arokh_power_name(uint32_t key)
	{
		for (const auto& option : arokh_power_options)
			if (option.primary_key == key)
				return option.name;
		// Identify by projectile identity, not the reused HUD icon.
		if (key == 0x00033850) return "Sonic";
		if (key == 0x00033801) return "Lava Ball";
		if (key == 0x00033A20) return "Sea Dragon Lightning Ball";
		return nullptr;
	}

	// Resolve each candidate from this level's live registry. Never reuse a
	// previous session's resource index or accept a same-key different class.
	bool resolve_arokh_projectile(const pcsx2& ps2, const arokh_power_option& option,
		uint32_t& rows, uint32_t& index) const
	{
		const auto registry = read_ee_value<uint32_t>(ps2, arokh_powers.manager + 0x320);
		if (!valid_ee_address(registry, 0x30))
			return false;
		rows = read_ee_value<uint32_t>(ps2, registry + 8);
		const auto count = read_ee_value<uint32_t>(ps2, registry + 0x10);
		if (count == 0 || count > 20000 || !valid_ee_address(rows, count * 36))
			return false;
		index = 0xFFFFFFFF;
		for (uint32_t first = 0; first < count; first += 64)
		{
			ps2_ipc_cmd read(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> entries;
			const auto batch = std::min(64u, count - first);
			for (uint32_t i = 0; i < batch; ++i)
			{
				entries[i * 2] = read.queue_read<uint32_t>(rows + (first + i) * 36);
				entries[i * 2 + 1] = read.queue_read<uint32_t>(rows + (first + i) * 36 + 8);
			}
			read.send();
			for (uint32_t i = 0; i < batch; ++i)
				if (read.read(entries[i * 2]) == option.primary_key &&
					read.read(entries[i * 2 + 1]) == option.resource_class)
				{
					if (index != 0xFFFFFFFF) return false;
					index = first + i;
				}
		}
		return index != 0xFFFFFFFF &&
			read_ee_value<uint32_t>(ps2, manager_global) == arokh_powers.manager &&
			read_ee_value<uint32_t>(ps2, arokh_powers.manager + 0x320) == registry &&
			read_ee_value<uint32_t>(ps2, registry + 8) == rows &&
			read_ee_value<uint32_t>(ps2, registry + 0x10) == count;
	}

	bool start_arokh_power(const pcsx2& ps2)
	{
		if (debug_state != debug_command_state::idle || arokh_power_selection < 0 ||
			arokh_power_selection >= static_cast<int>(arokh_power_options.size()))
			return false;
		refresh_arokh_powers(ps2);
		if (!arokh_powers_available || ps2.get_ipc()->Status() != PINE::PCSX2::Running)
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Arokh powers unavailable. Resume gameplay with Arokh present.");
			return false;
		}
		const auto& option = arokh_power_options[arokh_power_selection];
		if (arokh_has_power(option.primary_key))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Arokh already has %s.", option.name);
			return false;
		}
		const bool projectile = option.resource_class != 0;
		uint32_t rows = 0, resource_index = 0xFFFFFFFF;
		if (projectile && (!resolve_arokh_projectile(ps2, option, rows, resource_index) ||
			arokh_powers.count >= arokh_powers.powers.size()))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "%s is unavailable in this level.", option.name);
			return false;
		}
		if (read_ee_value<uint32_t>(ps2, 0x003D35C8) != 0x0C0F491E ||
			(!projectile && read_ee_value<uint32_t>(ps2, 0x002403B8) != 0x27BDFAF0) ||
			(projectile && (read_ee_value<uint32_t>(ps2, 0x0027D018) != 0x27BDFF40 ||
			 read_ee_value<uint32_t>(ps2, 0x002706E0) != 0x27BDFFF0)))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Arokh power commands unavailable in this game state.");
			return false;
		}
		if (!arokh_workspace_available(ps2))
			return false;
		upload_arokh_program(ps2, arokh_action::give_power);
		if (read_render_program<drakan_arokh_power_program::give_power.size()>(ps2, arokh_code_cave) !=
			drakan_arokh_power_program::give_power)
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Could not prepare Arokh's power command.");
			return false;
		}
		ps2_ipc_cmd install(ps2);
		// Fixed commands only; selecting the dropdown never writes to the game.
		const auto length = option.command != nullptr ? std::strlen(option.command) : 0;
		for (uint32_t i = 0; i < 32; ++i)
		{
			install.write<uint8_t>(arokh_power_command_buffer + i, i < length ? option.command[i] : 0);
		}
		install.write<uint32_t>(arokh_power_request, arokh_powers.manager);
		install.write<uint32_t>(arokh_power_request + 4, arokh_powers.controller);
		install.write<uint32_t>(arokh_power_request + 8, arokh_powers.active_actor);
		install.write<uint32_t>(arokh_power_request + 12, projectile ? 1 : 0);
		install.write<uint32_t>(arokh_power_request + 16, rows);
		install.write<uint32_t>(arokh_power_request + 20, resource_index);
		install.write<uint32_t>(arokh_power_request + 24, option.primary_key);
		install.write<uint32_t>(arokh_power_request + 28, option.resource_class);
		// Null-source native weapon event; use existing elemental HUD resources.
		install.write<uint32_t>(0x01FF0730, 0);
		install.write<uint32_t>(0x01FF0734, 0x01FF0740);
		install.write<uint32_t>(0x01FF0740, 0x1005);
		install.write<uint32_t>(0x01FF0744, resource_index);
		install.write<uint32_t>(0x01FF0748, 0xFFFFFFFF);
		install.write<uint32_t>(0x01FF074C, option.hud_slot);
		install.write<uint32_t>(0x01FF0750, 0);
		install.write<uint32_t>(arokh_completion_latch, 0);
		install.write<uint32_t>(debug_update_hook, arokh_temporary_hook);
		install.send();
		arokh_power_requested = arokh_power_selection;
		debug_state = debug_command_state::waiting_for_arokh_power;
		arokh_missing = false;
		arokh_message = nullptr;
		std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Applying %s...", option.name);
		return true;
	}

	void finish_arokh_power(const pcsx2& ps2, bool completed)
	{
		if (arokh_remove_index >= 0)
		{
			finish_arokh_remove(ps2, completed);
			return;
		}
		refresh_arokh_powers(ps2);
		const auto& option = arokh_power_options[arokh_power_requested];
		bool granted = completed && arokh_powers_available && arokh_has_power(option.primary_key);
		if (granted && option.resource_class != 0)
		{
			const auto selected = arokh_powers.selected;
			granted = selected >= 0 && static_cast<uint32_t>(selected) < arokh_powers.count;
			if (granted)
			{
				const auto& record = arokh_powers.powers[selected];
				granted = record.primary_key == option.primary_key && record.secondary == 0xFFFFFFFF &&
					record.hud == option.hud_slot && record.flags == 0;
			}
		}
		std::snprintf(arokh_power_message, sizeof(arokh_power_message), granted ?
			"Arokh has %s." : "Could not confirm %s. Check Arokh's powers and try again.", option.name);
		arokh_power_requested = -1;
	}

	static bool same_arokh_power(const arokh_power_record& a, const arokh_power_record& b)
	{
		return a.primary == b.primary && a.secondary == b.secondary && a.hud == b.hud &&
			a.flags == b.flags && a.primary_key == b.primary_key;
	}

	bool arokh_attack_idle(const pcsx2& ps2, uint32_t control) const
	{
		return (read_ee_value<uint32_t>(ps2, control + 0x90) & 0x22) == 0 &&
			read_ee_value<uint32_t>(ps2, control + 0x89C) == 0 &&
			read_ee_value<uint32_t>(ps2, control + 0x9C) == 0;
	}

	bool start_arokh_remove(const pcsx2& ps2, uint32_t index)
	{
		if (debug_state != debug_command_state::idle || !arokh_powers_available ||
			index >= arokh_powers.count)
			return false;
		const auto clicked = arokh_powers;
		refresh_arokh_powers(ps2);
		if (!arokh_powers_available || ps2.get_ipc()->Status() != PINE::PCSX2::Running ||
			arokh_powers.controller != clicked.controller || arokh_powers.manager != clicked.manager ||
			index >= arokh_powers.count || !same_arokh_power(clicked.powers[index], arokh_powers.powers[index]))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Powers changed. Select Remove again during gameplay.");
			return false;
		}
		if (arokh_powers.count <= 1)
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Keep at least one Arokh power.");
			return false;
		}
		if (!arokh_attack_idle(ps2, arokh_powers.controller))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Release Arokh's attack and wait for it to finish, then remove the power.");
			return false;
		}
		const auto registry = read_ee_value<uint32_t>(ps2, arokh_powers.manager + 0x320);
		if (!valid_ee_address(registry, 0x30))
			return false;
		const auto rows = read_ee_value<uint32_t>(ps2, registry + 8);
		const auto resources = read_ee_value<uint32_t>(ps2, registry + 0x10);
		if (!valid_ee_address(registry, 0x30) || resources == 0 || resources > 20000 ||
			!valid_ee_address(rows, resources * 36) ||
			read_ee_value<uint32_t>(ps2, 0x003D35C8) != 0x0C0F491E ||
			read_ee_value<uint32_t>(ps2, 0x002706E0) != 0x27BDFFF0 ||
			read_ee_value<uint32_t>(ps2, 0x00270790) != 0x0C09B3EE ||
			read_ee_value<uint32_t>(ps2, 0x0026CFB8) != 0x27BDFFC0)
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Power removal unavailable in this game state.");
			return false;
		}
		if (!arokh_workspace_available(ps2))
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Power removal unavailable: another command owns the workspace.");
			return false;
		}
		upload_arokh_program(ps2, arokh_action::remove_power);
		if (read_render_program<drakan_arokh_remove_program::remove_power.size()>(ps2, arokh_code_cave) !=
			drakan_arokh_remove_program::remove_power)
		{
			std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Could not prepare power removal.");
			return false;
		}
		const auto& target = arokh_powers.powers[index];
		const std::array<uint32_t, 16> request = {{ arokh_powers.manager, arokh_powers.controller,
			arokh_powers.active_actor, index, static_cast<uint32_t>(arokh_powers.selected),
			arokh_powers.records, arokh_powers.capacity, arokh_powers.count,
			target.primary, target.secondary, target.hud, target.flags, rows, target.primary_key,
			resources, registry }};
		ps2_ipc_cmd install(ps2);
		for (uint32_t i = 0; i < request.size(); ++i)
			install.write<uint32_t>(arokh_power_request + i * 4, request[i]);
		install.write<uint32_t>(arokh_completion_latch, 0);
		install.write<uint32_t>(debug_update_hook, arokh_temporary_hook);
		install.send();
		arokh_remove_before = arokh_powers;
		arokh_remove_index = static_cast<int>(index);
		debug_state = debug_command_state::waiting_for_arokh_power;
		arokh_missing = false;
		arokh_message = nullptr;
		const auto* name = arokh_power_name(target.primary_key);
		std::snprintf(arokh_power_message, sizeof(arokh_power_message), "Removing %s...", name ? name : "power");
		return true;
	}

	void finish_arokh_remove(const pcsx2& ps2, bool completed)
	{
		refresh_arokh_powers(ps2);
		const auto index = static_cast<uint32_t>(arokh_remove_index);
		const auto& before = arokh_remove_before;
		int32_t selected = before.selected;
		if (selected < 0 || selected == static_cast<int32_t>(index)) selected = 0;
		else if (selected > static_cast<int32_t>(index)) --selected;
		bool removed = completed && arokh_powers_available &&
			arokh_powers.manager == before.manager && arokh_powers.controller == before.controller &&
			arokh_powers.records == before.records && arokh_powers.capacity == before.capacity &&
			arokh_powers.count + 1 == before.count && arokh_powers.selected == selected;
		if (removed)
			for (uint32_t i = 0; i < arokh_powers.count; ++i)
				if (!same_arokh_power(arokh_powers.powers[i], before.powers[i < index ? i : i + 1]))
					removed = false;
		const auto* name = arokh_power_name(before.powers[index].primary_key);
		arokh_remove_index = -1;
		arokh_remove_before = {};
	}

	void draw_arokh_powers_ui(const pcsx2& ps2)
	{
		ImGui::PushID("arokh_powers");
		ImGui::SetNextItemWidth(240.0f);
		if (ImGui::BeginCombo("Power", arokh_power_options[arokh_power_selection].name))
		{
			for (int i = 0; i < static_cast<int>(arokh_power_options.size()); ++i)
			{
				if (i == 4) ImGui::SeparatorText("Experimental projectiles");
				const bool selected = i == arokh_power_selection;
				if (ImGui::Selectable(arokh_power_options[i].name, selected))
					arokh_power_selection = i;
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(debug_state != debug_command_state::idle || !arokh_powers_available);
		if (ImGui::Button("Apply"))
			start_arokh_power(ps2);
		ImGui::EndDisabled();
		if (arokh_power_message[0] != '\0')
			ImGui::TextWrapped("%s", arokh_power_message);
		ImGui::TextUnformatted("Current powers");
		if (!arokh_powers_available)
			ImGui::TextUnformatted("Unavailable - Arokh must be present in gameplay.");
		else if (arokh_powers.count == 0)
			ImGui::TextUnformatted("No powers.");
		else
			for (uint32_t i = 0; i < arokh_powers.count; ++i)
			{
				const auto* name = arokh_power_name(arokh_powers.powers[i].primary_key);
				const char* equipped = arokh_powers.selected == static_cast<int32_t>(i) ? " (equipped)" : "";
				if (name != nullptr) ImGui::BulletText("%s%s", name, equipped);
				else ImGui::BulletText("Other power (%u)%s", arokh_powers.powers[i].primary, equipped);
				ImGui::SameLine();
				ImGui::PushID(static_cast<int>(i));
				ImGui::BeginDisabled(debug_state != debug_command_state::idle || arokh_powers.count <= 1);
				const bool remove = ImGui::SmallButton("Remove");
				ImGui::EndDisabled();
				ImGui::PopID();
				if (remove)
				{
					start_arokh_remove(ps2, i);
					break; // Refresh can invalidate the rows currently being drawn.
				}
			}
		if (arokh_powers_available && arokh_powers.count == 1)
			ImGui::TextUnformatted("Keep at least one power.");
		ImGui::PopID();
	}

	bool arokh_workspace_available(const pcsx2& ps2)
	{
		ps2_ipc_cmd hook_read(ps2);
		auto hook = hook_read.queue_read<uint32_t>(debug_update_hook);
		auto delay_slot = hook_read.queue_read<uint32_t>(debug_update_hook + 4);
		hook_read.send();
		if (hook_read.read(hook) != debug_expected_hook || hook_read.read(delay_slot) != 0)
		{
			arokh_message = "Arokh command unavailable: update hook is in use.";
			return false;
		}

		// Keep batches below the IPC helper's 8-bit command index limit.
		for (uint32_t offset = 0; offset < arokh_workspace_size; offset += 0x200)
		{
			ps2_ipc_cmd workspace_read(ps2);
			std::array<ps2_ipc_cmd::queued_read<uint32_t>, 128> workspace;
			for (size_t i = 0; i < workspace.size(); ++i)
			{
				workspace[i] = workspace_read.queue_read<uint32_t>(arokh_code_cave + offset +
					static_cast<uint32_t>(i * sizeof(uint32_t)));
			}
			workspace_read.send();
			for (const auto& word : workspace)
			{
				if (workspace_read.read(word) != 0)
				{
					arokh_message = "Arokh command unavailable: command workspace is in use.";
					return false;
				}
			}
		}

		return true;
	}

	void upload_arokh_program(const pcsx2& ps2, arokh_action action)
	{
		const auto* wrapper = action == arokh_action::spawn ?
			drakan_arokh_programs::spawn.data() :
			action == arokh_action::remove_power ? drakan_arokh_remove_program::remove_power.data() :
			action == arokh_action::give_power ? drakan_arokh_power_program::give_power.data() : drakan_arokh_programs::teleport.data();
		const auto wrapper_size = action == arokh_action::spawn ?
			drakan_arokh_programs::spawn.size() :
			action == arokh_action::remove_power ? drakan_arokh_remove_program::remove_power.size() :
			action == arokh_action::give_power ? drakan_arokh_power_program::give_power.size() : drakan_arokh_programs::teleport.size();
		for (size_t offset = 0; offset < wrapper_size; offset += 128)
		{
			ps2_ipc_cmd install_code(ps2);
			for (size_t i = offset; i < wrapper_size && i < offset + 128; ++i)
			{
				install_code.write<uint32_t>(arokh_code_cave + static_cast<uint32_t>(i * sizeof(uint32_t)), wrapper[i]);
			}
			install_code.send();
		}
	}

	bool start_arokh_command(const pcsx2& ps2, arokh_action action)
	{
		if (debug_state != debug_command_state::idle || !player_position_available ||
			(action == arokh_action::spawn && !arokh_missing))
			return false;

		// Validate first; never overwrite another command's hook or workspace.
		if (!arokh_workspace_available(ps2))
			return false;

		upload_arokh_program(ps2, action);
		// Publish the request only after its program and input data are ready.
		ps2_ipc_cmd install(ps2);
		install.write<uint32_t>(arokh_enable_event, 0);
		install.write<uint32_t>(arokh_enable_event + 4, arokh_enable_payload);
		install.write<uint32_t>(arokh_enable_payload, 0x1001);
		install.write<uint32_t>(arokh_completion_latch, 0);
		install.write<uint32_t>(debug_update_hook, arokh_temporary_hook); // Install the hook last.
		install.send();
		debug_state = action == arokh_action::spawn ? debug_command_state::waiting_for_arokh_spawn : debug_command_state::waiting_for_arokh;
		arokh_missing = false;
		arokh_message = nullptr;
		debug_command_message = nullptr;
		return true;
	}

	void update_arokh_command(const pcsx2& ps2)
	{
		ps2_ipc_cmd completion_read(ps2);
		auto hook = completion_read.queue_read<uint32_t>(debug_update_hook);
		auto latch = completion_read.queue_read<uint32_t>(arokh_completion_latch);
		completion_read.send();
		const auto hook_value = completion_read.read(hook);
		const auto result = static_cast<arokh_result>(completion_read.read(latch));
		if (hook_value != arokh_temporary_hook)
		{
			if (debug_state == debug_command_state::waiting_for_arokh_power)
			{
				debug_state = debug_command_state::idle;
				finish_arokh_power(ps2, false);
				return;
			}
			arokh_missing = false;
			arokh_message = debug_state == debug_command_state::waiting_for_arokh_spawn ?
				"Arokh spawn interrupted." : "Arokh teleport interrupted.";
			debug_state = debug_command_state::idle;
			return;
		}
		// Wait while the PS2 is still executing the request.
		if (result < arokh_result::complete || result > arokh_result::already_exists)
			return;

		// Stop entering the program before clearing its temporary memory.
		ps2_ipc_cmd restore_hook(ps2);
		restore_hook.write<uint32_t>(debug_update_hook, debug_expected_hook).send();

		for (uint32_t offset = 0; offset < arokh_workspace_size; offset += 0x200)
		{
			ps2_ipc_cmd clear_workspace(ps2);
			for (uint32_t word = 0; word < 0x200; word += sizeof(uint32_t))
			{
				clear_workspace.write<uint32_t>(arokh_code_cave + offset + word, 0);
			}
			clear_workspace.send();
		}
		if (debug_state == debug_command_state::waiting_for_arokh_power)
		{
			debug_state = debug_command_state::idle;
			finish_arokh_power(ps2, result == arokh_result::complete);
			return;
		}
		const bool spawning = debug_state == debug_command_state::waiting_for_arokh_spawn;
		debug_state = debug_command_state::idle;
		arokh_missing = !spawning && result == arokh_result::missing;
		if (arokh_missing)
			arokh_message = "Arokh is missing. Use Spawn Arokh to bring him here.";
		else if (result == arokh_result::already_exists)
			arokh_message = "Arokh already exists. Use Teleport Arokh to Rynn.";
		else if (spawning)
			arokh_message = result == arokh_result::complete ? "Arokh spawned next to Rynn." :
				"Arokh spawn did not complete. Try teleport again once the level has loaded.";
		else
			arokh_message = result == arokh_result::complete ? "Arokh moved next to Rynn." :
				"Unable to summon Arokh. Dismount and make sure he is available.";
	}

	bool start_debug_command(const pcsx2& ps2, debug_command_state command)
	{
		if (debug_state != debug_command_state::idle ||
			(command != debug_command_state::waiting_for_fly &&
			 command != debug_command_state::waiting_for_drop &&
			 command != debug_command_state::waiting_for_hud &&
			 command != debug_command_state::waiting_for_hendrix))
			return false;

		if (command == debug_command_state::waiting_for_hendrix)
		{
			if (!player_position_available || ps2.get_ipc()->Status() != PINE::PCSX2::Running)
			{
				return false;
			}
			// Verify the retail parser and both native skill/progression calls.
			ps2_ipc_cmd verify(ps2);
			auto parser = verify.queue_read<uint32_t>(0x002403B8);
			auto skills = verify.queue_read<uint32_t>(0x002404F8);
			auto progression = verify.queue_read<uint32_t>(0x00240500);
			verify.send();
			if (verify.read(parser) != 0x27BDFAF0 || verify.read(skills) != 0x0C0F563A ||
				verify.read(progression) != 0x0C0F55CC)
			{
				return false;
			}
		}

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
		uint32_t command_text = fly_command;
		uint32_t command_tail = 0;
		if (command == debug_command_state::waiting_for_drop)
			command_text = drop_command;
		else if (command == debug_command_state::waiting_for_hud)
			command_text = hud_command;
		else if (command == debug_command_state::waiting_for_hendrix)
		{
			command_text = hendrix_command;
			command_tail = hendrix_command_tail;
		}
		install.write<uint32_t>(debug_command_buffer, command_text);
		install.write<uint32_t>(debug_command_buffer + sizeof(uint32_t), command_tail);
		install.write<uint32_t>(debug_completion_latch, 0);
		install.write<uint32_t>(debug_update_hook, debug_temporary_hook); // Install the hook last.
		install.send();
		debug_state = command;
		debug_command_message = nullptr;
		return true;
	}

	void update_debug_command(const pcsx2& ps2)
	{
		if (debug_state == debug_command_state::waiting_for_arokh ||
			debug_state == debug_command_state::waiting_for_arokh_spawn ||
			debug_state == debug_command_state::waiting_for_arokh_power)
		{
			update_arokh_command(ps2);
			return;
		}

		if (debug_state == debug_command_state::idle)
			return;

		if (read_ee_value<uint32_t>(ps2, debug_completion_latch) != 2)
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

	static void draw_coordinate(const char* label, float value)
	{
		char text[64];
		std::snprintf(text, sizeof(text), "%.3f", value);

		ImGui::PushID(label);
		ImGui::BeginGroup();
		ImGui::Text("%c:", label[0]);
		ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x);

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::SetNextItemWidth(ImGui::CalcTextSize(text).x + 1.0f);
		ImGui::InputText("##value", text, sizeof(text),
			ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		ImGui::EndGroup();
		ImGui::PopID();
	}

	void initialize_position_editor()
	{
		if (!position_editor_initialized && player_position_available)
		{
			position_edit[position_x] = player_position.get(position_x);
			position_edit[position_y] = player_position.get(position_y);
			position_edit[position_z] = player_position.get(position_z);
			position_editor_initialized = true;
		}
	}

	void draw_debug_command_message() const
	{
		if (debug_command_message != nullptr)
			ImGui::TextUnformatted(debug_command_message);
	}

	void draw_rynn_position_ui(const pcsx2& ps2)
	{
		if (!ImGui::CollapsingHeader("Rynn position", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::Text("Rynn world position");
		draw_coordinate("X##rynn", player_position.get(position_x));
		draw_coordinate("Y##rynn", player_position.get(position_y));
		draw_coordinate("Z##rynn", player_position.get(position_z));

		ImGui::Text("Set Rynn world position");
		ImGui::InputFloat("X", &position_edit[position_x], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Y", &position_edit[position_y], 0.0f, 0.0f, "%.3f");
		ImGui::InputFloat("Z", &position_edit[position_z], 0.0f, 0.0f, "%.3f");

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
	}

	void draw_camera_ui(const pcsx2& ps2)
	{
		if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::Text("Debug Fly: %s", fly_enabled ? "On" : "Off");
		ImGui::SameLine();
		ImGui::BeginDisabled(debug_state != debug_command_state::idle);
		if (ImGui::Button(fly_enabled ? "Disable Debug Fly" : "Enable Debug Fly"))
			start_debug_command(ps2, debug_command_state::waiting_for_fly);
		ImGui::EndDisabled();

		draw_camera_speed_ui(ps2);
		ImGui::Separator();

		ImGui::Text("Camera world position");
		if (camera_position_available)
		{
			draw_coordinate("X##camera", camera_position.get(position_x));
			draw_coordinate("Y##camera", camera_position.get(position_y));
			draw_coordinate("Z##camera", camera_position.get(position_z));
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
		if (!fly_enabled)
			ImGui::Text("Enable Debug Fly to use 'Drop Rynn at Camera'.");
	}

	void draw_fov_ui()
	{
		bool override_fov = fov_values.currently_tweaking();
		if (ImGui::Checkbox("Override field of view", &override_fov))
		{
			fov_values.toggle_tweaking();
			sentinel.increment();
		}
		ImGui::BeginDisabled(!fov_values.currently_tweaking());
		ImGui::SetNextItemWidth(240.0f);
		ImGui::SliderFloat("Field of view (degrees)", &fov, 20.0f, 180.0f, "%.2f");
		ImGui::EndDisabled();
	}

	void draw_view_settings_ui(const pcsx2& ps2)
	{
		if (!ImGui::CollapsingHeader("View settings", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::BeginDisabled(debug_state != debug_command_state::idle || !player_position_available);
		if (ImGui::Button(hud_values.get(hud_visibility) != 0 ? "Hide HUD" : "Show HUD"))
		{
			start_debug_command(ps2, debug_command_state::waiting_for_hud);
		}
		ImGui::EndDisabled();

		draw_fov_ui();
		draw_render_distance_ui(ps2);
	}

	void draw_arokh_settings_ui(const pcsx2& ps2)
	{
		if (!ImGui::CollapsingHeader("Arokh!", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::BeginDisabled(debug_state != debug_command_state::idle || !player_position_available);
		if (ImGui::Button("Teleport Arokh to Rynn"))
			start_arokh_command(ps2, arokh_action::teleport);
		// A missing companion is the only result that permits creation.
		ImGui::BeginDisabled(!arokh_missing);
		if (ImGui::Button("Spawn Arokh"))
			start_arokh_command(ps2, arokh_action::spawn);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (arokh_message != nullptr)
			ImGui::TextUnformatted(arokh_message);

		ImGui::Separator();
		draw_arokh_powers_ui(ps2);
		ImGui::Separator();
		ImGui::Text("Arokh collision: %s", arokh_collision_flag.is_on() ? "Off" : "On");
		if (ImGui::Button(arokh_collision_flag.is_on() ? "Enable Arokh Collision" : "Disable Arokh Collision"))
		{
			if (!arokh_collision_flag.is_on())
				collision_before_arokh = collision_flag.is_on();
			arokh_collision_flag.toggle();
			// Arokh also needs the existing shared movement bypass.
			collision_flag.set_on(arokh_collision_flag.is_on() || collision_before_arokh);
			sentinel.increment();
		}
		if (arokh_collision_flag.is_on())
			ImGui::TextUnformatted("Also disables Rynn collision while enabled.");
	}

	void draw_jump_height_ui(const pcsx2& ps2)
	{
		ImGui::PushID("jump_height");
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputDouble("Jump height", &jump_height,
			0.0, 0.0, "%.6g");
		if (ImGui::Button("Apply"))
			set_jump_height(ps2, jump_height);
		ImGui::SameLine();
		if (ImGui::Button("\xC3\x97" "2 & Apply"))
			set_jump_height(ps2, jump_height * 2.0);
		ImGui::SameLine();
		if (ImGui::Button("Reset to 1\xC3\x97"))
			set_jump_height(ps2, 1.0);
		ImGui::Text("Applied: %.6g\xC3\x97%s", jump_height_applied,
			jump_height != jump_height_applied ? " (unapplied edit)" : "");
		ImGui::TextUnformatted("Click Apply to use your value.");
		if (jump_height_invalid)
			ImGui::TextUnformatted("Number invalid");
		ImGui::PopID();
	}

	void draw_experimental_ui()
	{
		ImGui::Checkbox("Enable Experiments", &show_experimental_options);
		if (show_experimental_options)
		{
			ImGui::Separator();

			if (ImGui::Button(infinite_flag.is_on() ?
				"Disable infinite bow ammo/shoot speed" : "Enable infinite bow ammo/shoot speed"))
			{
				infinite_flag.toggle();
				infinite_bow_shooting.toggle_tweaking();
				infinite_bow_ammo.toggle_tweaking();
				sentinel.increment();
			}
		}
	}

	void draw_gameplay_ui(const pcsx2& ps2)
	{
		if (!ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::BeginDisabled(debug_state != debug_command_state::idle || !player_position_available ||
			ps2.get_ipc()->Status() != PINE::PCSX2::Running);
		if (ImGui::Button("Max Skills to 8"))
			start_debug_command(ps2, debug_command_state::waiting_for_hendrix);
		ImGui::EndDisabled();

		draw_jump_height_ui(ps2);
		ImGui::Separator();

		ImGui::Text("Collision: %s", collision_flag.is_on() ? "Off" : "On");
		ImGui::SameLine();
		ImGui::BeginDisabled(arokh_collision_flag.is_on());
		if (ImGui::Button(collision_flag.is_on() ? "Enable Collision" : "Disable Collision"))
		{
			collision_flag.toggle();
			sentinel.increment();
		}
		ImGui::EndDisabled();

		ImGui::Text("God mode: %s", true_god_mode.is_on() ? "On" : "Off");
		ImGui::SameLine();
		if (ImGui::Button(true_god_mode.is_on() ? "Disable True God mode" : "Enable True God mode"))
		{
			true_god_mode.toggle();
			sentinel.increment();
		}

		draw_experimental_ui();
	}

public:
	explicit drakan(const pcsx2& ps2)
		: sentinel(ps2, load_sentinel_address)
		, collision_flag(ps2, { {
			{ 0x002354F8, 0x44801800, 0x460018C7 }, // Zero movement correction X.
			{ 0x00235514, 0x44801000, 0x46001087 }, // Zero movement correction Z.
			{ 0x0023551C, 0x44800800, 0x46000847 }, // Zero movement correction Y.
			{ 0x00235558, 0x10000005, 0x0C08D5B4 }, // Skip both collision solver passes.
			{ 0x00281778, 0x10000093, 0x14400093 }  // Bypass Arokh environment response.
		} })
		, arokh_collision_flag(ps2, { {
			{ 0x002744FC, 0xE6350904, 0xE6200904 }, // Keep the flight speed multiplier at 1.
			{ 0x00280BE8, 0x03E00008, 0x27BDFE00 }, // Return before Arokh contact/impact response.
			{ 0x00280BEC, 0x00000000, 0x3C03004F }  // Return delay slot.
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
		, hud_values(ps2, hud_flag_address)
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
		initialize_position_editor();
		draw_debug_command_message();
		draw_rynn_position_ui(ps2);
		draw_camera_ui(ps2);
		draw_view_settings_ui(ps2);
		draw_arokh_settings_ui(ps2);
		draw_gameplay_ui(ps2);
	}

	void update(const pcsx2& ps2, const controller_state& c, playback& camera_playback, float time_delta) override
	{
		if (sentinel.has_reset())
		{
			position_editor_initialized = false;
			request_render_distance_reset();
			player_position_address = 0;
			player_position_available = false;
			camera_position_available = false;
			arokh_collision_flag.reset();
			collision_flag.reset();
			collision_before_arokh = false;
			fov_values.reset();
			infinite_flag.reset();
			true_god_mode.reset();
			set_jump_height(ps2, 1.0);
			set_camera_speed(ps2, 1.0f);
			infinite_bow_shooting.reset();
			infinite_bow_ammo.reset();
			arokh_missing = false;
			arokh_message = nullptr;
			arokh_powers = {};
			arokh_powers_available = false;
			arokh_power_refresh_time = 0.5f;
			arokh_power_message[0] = '\0';
			// An in-flight Arokh command still needs its normal ownership check/cleanup.
			if (debug_state != debug_command_state::waiting_for_arokh &&
				debug_state != debug_command_state::waiting_for_arokh_spawn &&
				debug_state != debug_command_state::waiting_for_arokh_power)
				debug_state = debug_command_state::idle;
			fly_enabled = false;
			debug_command_message = nullptr;

		}

		update_player_position(ps2);
		update_camera_position(ps2);
		update_debug_command(ps2);
		if (std::isfinite(time_delta) && time_delta > 0.0f)
			arokh_power_refresh_time += time_delta;
		if (!player_position_available)
		{
			arokh_powers_available = false;
			arokh_powers = {};
		}
		else if (debug_state == debug_command_state::idle && arokh_power_refresh_time >= 0.5f)
			refresh_arokh_powers(ps2);
		update_fly_mode(ps2);
		update_camera_speed(ps2);
		update_render_distance(ps2);
		update_render_distance_recovery(ps2, time_delta);
		hud_values.update();

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
		// Refresh every poll so loading a state is detected even with no recent UI changes.
		sentinel.increment();
	}

	bool needs_ui_refresh() const override
	{
		return true;
	}
};

ps2_game_static_register<drakan> r("4167D813", "Drakan - The Ancients' Gates (USA)");
