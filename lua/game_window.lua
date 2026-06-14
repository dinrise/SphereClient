local M = {}

M.ui_windows = {
    "system_left.ui",
    "system_leftmin.ui",
    "system_right.ui",
    "system_rightmin.ui",
    "chat.ui",
    "chat_st2.ui",
    "chat_sys.ui",
    "inventory.ui",
    "statinfo.ui",
    "puppet.ui",
    "quickitems.ui",
    "hotkeys.ui",
    "mantrabook.ui",
    "journal.ui",
    "clan.ui",
    "group.ui",
    "minimap.ui",
    "bigmap.ui",
    "help.ui",
}
M.settings_windows = {
    "options.ui",
    "gfxoptions.ui",
    "soundopt.ui",
    "controls.ui",
    "intoptions.ui",
    "authors.ui",
}
M.ui_initially_visible = {
    "system_left",
    "system_right",
    "chat_st2",
    "chat_sys",
}
M.ui_initially_checked = {
    {window = "chat_st2", control = 10},
}
M.ui_actions = {
    {window = "system_left", control = 1, action = "toggle_window", target = "statinfo"},
    {window = "system_left", control = 2, action = "toggle_window", target = "puppet"},
    {window = "system_left", control = 3, action = "toggle_window", target = "inventory"},
    {window = "system_left", control = 4, action = "cycle_pair", target = "quick_items", alternate = "hotkeys"},
    {window = "system_left", control = 6, action = "toggle_window", target = "clan"},
    {window = "system_left", control = 7, action = "toggle_window", target = "group"},
    {window = "system_left", control = 8, action = "swap_window", target = "system_leftmin"},
    {window = "system_leftmin", control = 1, action = "swap_window", target = "system_left"},

    {window = "system_right", control = 1, action = "swap_window", target = "system_rightmin"},
    {window = "system_right", control = 4, action = "toggle_window", target = "minimap"},
    {window = "system_right", control = 7, action = "toggle_chat"},
    {window = "system_right", control = 10, action = "toggle_window", target = "journal_mini"},
    {window = "system_rightmin", control = 1, action = "swap_window", target = "system_right"},

    {window = "chat_st2", control = 2, action = "toggle_chat"},
    {window = "chat_st2", control = 4, action = "toggle_chat"},
    {window = "minimap", control = 5, action = "toggle_window", target = "bigmap"},
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
-- Recovered from FUN_00430920:
-- mapX = floor((4000 + worldX) * 0.512)
-- mapZ = floor((4000 - worldZ) * 0.512)
M.grassmap_world_offset_x = 4000.0
M.grassmap_world_offset_z = 4000.0
M.grassmap_world_scale = 0.5120000243186951
M.grassmap_world_sign_x = 1
M.grassmap_world_sign_z = -1
-- FUN_00457020 switches to the second 15-pattern bank at this world height.
M.grass_highland_min_y = 300.0
M.grass_highland_max_y = 800.0
M.grass_highland_pattern_offset = 15
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
-- Recovered from FUN_00470640.
M.grass_flatness_radius = 2.45
M.grass_flatness_threshold = 0.5
M.grass_flatness_normal_y = 0.75
M.grass_generation_margin = 16.0
M.grass_wind_amplitude = 0.018
M.grass_wind_speed = 1.35
M.grass_mode_text = {"Выкл", "Вкл", "Аним"}
M.terrain_microtexture = "landscape/bs_.mtx"
-- Sky.txt t00, active through the original weather manager.
M.sky_texture = "landscape/clouds.dds"
M.sky_radius = 220.0
M.sky_height_scale = 0.55
M.sky_scroll_speed = 0.002
M.sky_red = 200
M.sky_green = 200
M.sky_blue = 200
-- Recovered from landscape/Sky.txt (c/d states) and weather.txt t00c.
M.sky_states = {
    {time=0.000, clear_red=0,  clear_green=0,   clear_blue=0,   ambient_red=53,  ambient_green=57,  ambient_blue=83,  sun_red=60,  sun_green=70,  sun_blue=100, cloud_red=105, cloud_green=105, cloud_blue=105},
    {time=0.167, clear_red=0,  clear_green=0,   clear_blue=0,   ambient_red=96,  ambient_green=102, ambient_blue=148, sun_red=60,  sun_green=70,  sun_blue=100, cloud_red=105, cloud_green=105, cloud_blue=105},
    {time=0.200, clear_red=40, clear_green=35,  clear_blue=35,  ambient_red=140, ambient_green=140, ambient_blue=140, sun_red=0,   sun_green=0,   sun_blue=0,   cloud_red=140, cloud_green=105, cloud_blue=70},
    {time=0.266, clear_red=90, clear_green=104, clear_blue=122, ambient_red=115, ambient_green=105, ambient_blue=100, sun_red=120, sun_green=60,  sun_blue=23,  cloud_red=169, cloud_green=104, cloud_blue=34},
    {time=0.335, clear_red=50, clear_green=160, clear_blue=250, ambient_red=97,  ambient_green=118, ambient_blue=142, sun_red=243, sun_green=202, sun_blue=166, cloud_red=200, cloud_green=200, cloud_blue=200},
    {time=0.667, clear_red=50, clear_green=160, clear_blue=250, ambient_red=97,  ambient_green=118, ambient_blue=142, sun_red=243, sun_green=202, sun_blue=166, cloud_red=200, cloud_green=200, cloud_blue=200},
    {time=0.733, clear_red=90, clear_green=104, clear_blue=122, ambient_red=115, ambient_green=105, ambient_blue=100, sun_red=120, sun_green=60,  sun_blue=23,  cloud_red=169, cloud_green=104, cloud_blue=34},
    {time=0.810, clear_red=40, clear_green=35,  clear_blue=35,  ambient_red=120, ambient_green=120, ambient_blue=120, sun_red=0,   sun_green=0,   sun_blue=0,   cloud_red=140, cloud_green=110, cloud_blue=70},
    {time=0.840, clear_red=0,  clear_green=0,   clear_blue=0,   ambient_red=103, ambient_green=103, ambient_blue=123, sun_red=0,   sun_green=0,   sun_blue=0,   cloud_red=115, cloud_green=115, cloud_blue=115},
    {time=0.864, clear_red=0,  clear_green=0,   clear_blue=0,   ambient_red=96,  ambient_green=102, ambient_blue=148, sun_red=60,  sun_green=70,  sun_blue=100, cloud_red=105, cloud_green=105, cloud_blue=105},
}

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
    [16] = {"grass000", "grass000", "grass000", "grass000", "grass000"},
    [17] = {"grass000", "grass000", "grass000", "grass000", "grass000"},
    [18] = {"grass000", "grass000", "grass000", "grass000", "grass000"},
    [19] = {"grass101", "grass101", "grass101", "grass101", "grass101"},
    [20] = {"grass102", "grass102", "grass102", "grass102", "grass102"},
    [21] = {"grass100", "grass100", "grass100", "grass101", "grass101"},
    [22] = {"grass101", "grass101", "grass101", "grass102", "grass102"},
    [23] = {"grass100", "grass100", "grass100", "grass102", "grass102"},
    [24] = {"grass100", "grass100", "grass100", "grass101", "grass102"},
    [25] = {"grass100", "grass100", "grass101", "grass102", "grass102"},
    [26] = {"grass101", "grass101", "grass101", "grass101", "grass101"},
    [27] = {"grass102", "grass102", "grass102", "grass102", "grass102"},
    [28] = {"grass100", "grass100", "grass100", "grass101", "grass101"},
    [29] = {"grass101", "grass101", "grass101", "grass102", "grass102"},
    [30] = {"grass100", "grass100", "grass100", "grass102", "grass102"},
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
