local M = {}

M.model_base = 0x30
M.hair_color_count = 4
M.tattoo_count = 4

M.genders = {
    male = {
        face_count = 13,
        hair_count = 3,
    },
    female = {
        face_count = 12,
        hair_count = 5,
    },
}

local body_camera = {
    target_x = -1.95,
    target_y = 1.42,
    target_z = -0.18,
    yaw = -0.24,
    distance = 4.25,
    pitch = 0.045,
    fov = 50.0,
}

local face_camera = {
    target_x = 0.14,
    target_y = 2.22,
    target_z = -0.18,
    yaw = 0.08,
    distance = 1.58,
    pitch = 0.01,
    fov = 50.0,
}

M.camera_profiles = {
    [0] = body_camera,
    [12] = body_camera,
    [13] = face_camera,
    [14] = {
        target_x = 0.12,
        target_y = 2.38,
        target_z = -0.18,
        yaw = 0.08,
        distance = 1.42,
        pitch = 0.0,
        fov = 50.0,
    },
    [15] = {
        target_x = 0.12,
        target_y = 2.30,
        target_z = -0.18,
        yaw = 0.08,
        distance = 1.46,
        pitch = 0.0,
        fov = 50.0,
    },
    [16] = face_camera,
}

function M.local_to_model(index)
    return M.model_base + index
end

function M.model_to_local(value, count)
    if value >= M.model_base and value < M.model_base + count then
        return value - M.model_base
    end
    return nil
end

return M
