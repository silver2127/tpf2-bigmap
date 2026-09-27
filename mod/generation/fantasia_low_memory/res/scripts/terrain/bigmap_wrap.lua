-- Automatic buffer reuse for another mod's terrain generators (tpf2_bigmap).
--
-- This mod ships gen files under the same names as the Fantasia Map
-- Generator's, so the game uses them in place of Fantasia's. Wrap(file, probe,
-- climate, minArea) then loads the real <file> from the mod that ships the
-- script module <probe>, without copying or editing that mod: the module is
-- found on package.path, its res/ folder gives the generator's path, and the
-- file is run in a private environment so its global data() does not replace
-- the caller's. The returned generator is the original one (same name,
-- params, climate); only for maps larger than minArea square metres is its
-- updateFn result passed through terrain/bigmap_memory.Optimize, which renames
-- temporary buffers whose lifetimes do not overlap onto shared names (see
-- docs/generation-op-semantics.md). Seeds and op order are unchanged, so a map
-- is the same map; only the native buffer count drops.
--
-- When the other mod is not active the generator stays in the list but refuses
-- to generate, with the reason in the game console.
local M = {}

local function resFolder(probe)
    if type(package.searchpath) ~= "function" then return nil end
    local path = package.searchpath(probe, package.path)
    if not path then return nil end
    local tail = "/scripts/" .. probe .. ".lua"
    path = path:gsub("\\", "/")
    if path:sub(-#tail) ~= tail then return nil end
    return path:sub(1, #path - #tail)
end

local function loadIn(path, env)
    if type(loadfile) == "function" then
        local chunk, err = loadfile(path, "t", env)
        if chunk or not io then return chunk, err end
    end
    local f, err = io.open(path, "rb")
    if not f then return nil, err end
    local text = f:read("*a")
    f:close()
    return load(text, "@" .. path, "t", env)
end

local function unavailable(file, climate, why)
    local message = "[tpf2_bigmap] " .. file .. " cannot run: " .. why
    print(message)
    return {
        climate = climate,
        order = 1000,
        name = file:gsub("%.gen%.lua$", "") .. " - original mod not active",
        params = {},
        updateFn = function() error(message) end,
    }
end

local function area(params)
    local x, y = tonumber(params and params.mapSizeX), tonumber(params and params.mapSizeY)
    return (x and y) and x * y or 0
end

function M.Wrap(file, probe, climate, minArea)
    local res = resFolder(probe)
    if not res then return unavailable(file, climate, probe .. " is not on package.path") end
    local path = res .. "/config/terrain_generators/" .. file
    local env = setmetatable({}, {__index = _G})
    local chunk, err = loadIn(path, env)
    if not chunk then return unavailable(file, climate, tostring(err)) end
    local ok, loadErr = pcall(chunk)
    if not ok then return unavailable(file, climate, tostring(loadErr)) end
    if type(env.data) ~= "function" then return unavailable(file, climate, "no data()") end
    local gen = env.data()
    if type(gen) ~= "table" or type(gen.updateFn) ~= "function" then
        return unavailable(file, climate, "data() returned no updateFn")
    end
    print(string.format("[tpf2_bigmap] %s: buffer reuse armed for maps over %.0f km2", file, minArea / 1048576))
    local update = gen.updateFn
    gen.updateFn = function(params)
        local result = update(params)
        if area(params) <= minArea then return result end
        return require("terrain/bigmap_memory").Optimize(result)
    end
    return gen
end

return M
