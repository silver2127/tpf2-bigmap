-- Reuse full-resolution terrain buffers only after their LAST named access.
-- No operation, seed, resolution, final output name or in-place alias changes.
local M = {}
local keys = {"input", "input1", "input2", "output"}
local function temporary(name)
    return type(name) == "string" and name:match("^__t_%d+$") ~= nil
end

function M.Optimize(result)
    if type(result) ~= "table" or type(result.layers) ~= "table" then return result end
    local records, ordered, pinned = {}, {}, {}
    -- Pin every name referenced outside the operation array, including final
    -- height/forest/asset maps and any future metadata that names a buffer.
    local function pin(value)
        if type(value) == "string" then pinned[value] = true
        elseif type(value) == "table" then
            for _, v in pairs(value) do pin(v) end
        end
    end
    for k, v in pairs(result) do if k ~= "layers" then pin(v) end end
    for i, layer in ipairs(result.layers) do
        local p = layer.params
        if type(p) ~= "table" or
            (layer.type ~= "OP" and layer.type ~= "MIX" and layer.type ~= "FEATURE") then
            return result -- unknown schema: leave the entire pipeline alone
        end
        for _, key in ipairs(keys) do
            local name = p[key]
            if name ~= nil and type(name) ~= "string" then return result end
            if name then
                if not records[name] then
                    records[name] = {name=name, first=i, last=i, layer=layer}
                    ordered[#ordered+1] = records[name]
                end
                records[name].last = i
            end
        end
        -- An unexpected nested reference is pinned instead of guessed at.
        for k, v in pairs(p) do
            if k ~= "input" and k ~= "input1" and k ~= "input2" and k ~= "output" then pin(v) end
        end
    end
    local slots, rename, saved = {}, {}, 0
    for _, r in ipairs(ordered) do
        local p = r.layer.params
        -- MAP writes every output element. Restrict recipients to a distinct
        -- input MAP; other operations may read or partially update old output.
        local overwrites = r.layer.type == "OP" and p.type == "MAP" and
            p.output == r.name and p.input and p.input ~= r.name and
            p.input1 == nil and p.input2 == nil
        local chosen
        if temporary(r.name) and not pinned[r.name] and overwrites then
            for _, slot in ipairs(slots) do
                if slot.last < r.first then chosen = slot; break end
            end
        end
        if chosen then
            rename[r.name] = chosen.name
            chosen.last = r.last
            saved = saved + 1
        elseif temporary(r.name) and not pinned[r.name] then
            slots[#slots+1] = {name=r.name, last=r.last}
        end
    end
    if saved == 0 then return result end
    for _, layer in ipairs(result.layers) do
        for _, key in ipairs(keys) do
            local name = layer.params[key]
            if rename[name] then layer.params[key] = rename[name] end
        end
    end
    print(string.format("[tpf2_bigmap] terrain memory: %d -> %d named buffers (%d reused)",
        #ordered, #ordered-saved, saved))
    return result
end

return M
