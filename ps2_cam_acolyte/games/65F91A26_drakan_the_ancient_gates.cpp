#include "imgui.h"
#include "../ps2.h"
#include "../ps2_commands.h"
#include "shared_camera.h"
#include "shared_ui.h"
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
		ps2_ipc_cmd read(ps2);
		std::array<ps2_ipc_cmd::queued_read<uint32_t>, N> queued;
		for (size_t i = 0; i < N; ++i)
			queued[i] = read.queue_read<uint32_t>(address + static_cast<uint32_t>(i * 4));
		read.send();
		std::array<uint32_t, N> words;
		for (size_t i = 0; i < N; ++i)
			words[i] = read.read(queued[i]);
		return words;
	}

	uint32_t prepare_render_mailbox(const pcsx2& ps2)
	{
		auto allocation = read_ee_value<uint32_t>(ps2, render_allocation);
		auto callback = read_ee_value<uint32_t>(ps2, render_callback);
		const bool allocate = allocation == 0;
		if (allocate)
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
			render_distance_message = "Draw distance hook is not ready.";
			return false;
		}
		if (static_cast<int32_t>(read_ee_value<uint32_t>(ps2, mailbox)) > 0)
		{
			render_distance_message = "Waiting for the previous distance change.";
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
		render_distance_message = "Distance change was not confirmed.";
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
				render_distance_message = "The level default is unavailable. Try again once the level has loaded.";
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
				render_distance_message = "Terrain code is different from the supported version. No changes applied.";
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
			// A changed snapshot cancels this action; it does not imply a state load.
			// Recovery already in progress waits for a fresh stable snapshot.
			render_distance_reset_stable_time = 0.0f;
			render_distance_reset_snapshot = {};
			render_distance_message = recovering ? nullptr :
				"Game state changed while preparing the distance. No changes applied. Try again.";
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
			nullptr : "The game changed the distance. Try again once the level has loaded.";
		ps2_ipc_cmd verify(ps2);
		std::array<ps2_ipc_cmd::queued_read<float>, 10> applied_fields;
		for (size_t i = 0; i < fields.size(); ++i)
			applied_fields[i] = verify.queue_read<float>(manager + fields[i].offset);
		verify.send();
		for (size_t i = 0; i < fields.size(); ++i)
			if (verify.read(applied_fields[i]) != fields[i].value)
				render_distance_message = "Distance or fog settings were not applied. Check for a conflicting cheat.";
		const auto applied_code = read_terrain_sector_code(ps2);
		for (size_t i = 0; i < applied_code.size(); ++i)
		{
			const auto expected = extend_sectors ? terrain_sector_patch[i].extended : terrain_sector_patch[i].original;
			if (applied_code[i] != expected)
				render_distance_message = "Terrain visibility was not applied. Check for a conflicting cheat.";
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
		{
			render_distance_reset_pending = false;
			render_distance_message = "Draw distance restored to the loaded level's default.";
		}
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
			ImGui::TextUnformatted("Restoring the loaded level's default draw distance...");
		ImGui::BeginDisabled(!render_distance_available || debug_state != debug_command_state::idle);
		ImGui::BeginDisabled(render_distance_reset_pending);
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputFloat("Distance", &render_distance_edit, 0.0f, 0.0f, "%.3f");
		if (ImGui::Button("Apply"))
			apply_render_distance(ps2, render_distance_edit);
		ImGui::SameLine();
		if (ImGui::Button("Double"))
			apply_render_distance(ps2, 0.0f, distance_action::double_current);
		ImGui::EndDisabled();
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

	// Retail USA retained debug-command wrapper. It submits FLY, DROP or HUD once
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
		waiting_for_hud,
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
		uint32_t command_text = fly_command;
		if (command == debug_command_state::waiting_for_drop)
			command_text = drop_command;
		else if (command == debug_command_state::waiting_for_hud)
			command_text = hud_command;
		install.write<uint32_t>(debug_command_buffer, command_text);
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

		draw_jump_height_ui(ps2);
		ImGui::Separator();

		ImGui::Text("Collision: %s", collision_flag.is_on() ? "Off" : "On");
		ImGui::SameLine();
		if (ImGui::Button(collision_flag.is_on() ? "Enable Collision" : "Disable Collision"))
		{
			collision_flag.toggle();
			sentinel.increment();
		}

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
			collision_flag.reset();
			fov_values.reset();
			infinite_flag.reset();
			true_god_mode.reset();
			set_jump_height(ps2, 1.0);
			set_camera_speed(ps2, 1.0f);
			infinite_bow_shooting.reset();
			infinite_bow_ammo.reset();
			debug_state = debug_command_state::idle;
			fly_enabled = false;
			debug_command_message = nullptr;

		}

		update_player_position(ps2);
		update_camera_position(ps2);
		update_debug_command(ps2);
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
