local M = {}

M.ui_windows = {
    "system_left.ui",
    "system_right.ui",
    "chat_st2.ui",
    "chat_sys.ui",
}
M.settings_windows = {
    "options.ui",
    "gfxoptions.ui",
    "soundopt.ui",
    "controls.ui",
    "intoptions.ui",
    "authors.ui",
}

-- Recovered from the original landscape lookup/render chain.
M.map_file = "landscape/map.bin"
M.landscape_dirs = {
    "landscape",
    "landscape_hr",
    "landscape_ph",
    "landscape_rd",
}
M.model_dirs = {
    "models",
    "Models_hr",
    "Models_ph",
    "Models_rd",
}
M.static_object_dirs = {
    "params",
}
M.grassmap_dir = "landscape/grassmap"
M.grassmap_grid_size = 16
M.grassmap_tile_resolution = 256
M.grassmap_world_offset = 2048
M.grass_radius = 42.0
M.grass_spacing = 8.3333
M.grass_detail_models = {"grass_s00", "grass_s01", "grass_s02", "grass_s03"}
-- Recovered from DAT_00521488/DAT_00521498, sampled by FUN_0047a150.
M.grass_sample_offsets = {
    {x = 6.24, z = 3.73},
    {x = 2.21, z = 1.17},
    {x = 2.21, z = 5.60},
    {x = 6.24, z = 7.15},
}
M.grass_detail_count = 8
M.grass_jitter_fraction = 0.25
M.grass_scale_min = 0.6
M.grass_scale_max = 1.1
M.grass_flatness_radius = 1.5
M.grass_flatness_threshold = 0.35
M.grass_generation_margin = 16.0
M.grass_wind_amplitude = 0.018
M.grass_wind_speed = 1.35
M.terrain_microtexture = "landscape/bs_.mtx"

-- Recovered from FUN_00460570: grass-map type -> five model variants.
M.grass_patterns = {
    [1] = {"grass002", "grass002", "grass002", "grass014", "grass014"},
    [2] = {"grass018", "grass018", "grass018", "grass006", "grass003"},
    [3] = {"grass003", "grass003", "grass003", "grass009", "grass004"},
    [4] = {"grass010", "grass010", "grass002", "grass002", "grass005"},
    [5] = {"grass009", "grass009", "grass009", "grass004", "grass000"},
    [6] = {"grass016", "grass016", "grass016", "grass004", "grass011"},
    [7] = {"grass014", "grass014", "grass007", "grass007", "grass007"},
    [8] = {"grass013", "grass013", "grass013", "grass005", "grass004"},
    [9] = {"grass007", "grass007", "grass003", "grass003", "grass013"},
    [10] = {"grass002", "grass002", "grass009", "grass009", "grass003"},
    [11] = {"grass012", "grass012", "grass005", "grass005", "grass013"},
    [12] = {"grass012", "grass012", "grass012", "grass004", "grass009"},
    [13] = {"grass007", "grass007", "grass007", "grass007", "grass015"},
    [14] = {"grass017", "grass017", "grass017", "grass005", "grass005"},
    [15] = {"grass001", "grass001", "grass001", "grass001", "grass007"},
}
M.grid_width = 80
M.origin_row = 40
M.origin_column = 39
M.visible_radius = 3
M.tile_size = 100.0
M.static_object_radius = 250.0

-- ControlMove anchors camera object 1 at the player's x/z and player.y - 2.
M.camera_mode = "first_person"
M.camera_eye_height = 2.0
M.camera_look_distance = 8.0
M.camera_turn_speed = 0.003
M.camera_pitch_speed = 0.003
M.camera_min_pitch = -1.40
M.camera_max_pitch = 1.40
M.camera_fov = 60.0
-- Recovered from ControlMove in _pcontrol.lua.
M.walk_speed = 2.1
M.run_multiplier = 2.2
M.player_collision_radius = 0.32
M.player_collision_height = 1.8
M.max_step_height = 0.6
M.movement_collision_step = 0.2
M.collision_floor_normal_threshold = 0.7
M.position_send_interval_ms = 100
-- ControlMove explicitly calls view_set_z(1, 0.2).
M.near_clip = 0.2
M.far_clip = 260.0
M.fog_start = 120.0
M.fog_end = 200.0
M.clear_red = 105
M.clear_green = 157
M.clear_blue = 205

return M
