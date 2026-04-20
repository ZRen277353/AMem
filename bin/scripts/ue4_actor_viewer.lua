-- UE4 Actor Viewer (ImGui callback version)
-- Fixes:
-- 1) Use imgui.createWindow + callback so UI is rendered every frame.
-- 2) Use incremental refresh to avoid long blocking reads in one frame.

print("=== Initializing UE4 Actor Viewer ===")

-- Get module base address dynamically
local libUE4_base = module.getBase("libUE4.so")
if not libUE4_base or libUE4_base == 0 then
    print("Error: Failed to get libUE4.so base address")
    return
end
print(string.format("libUE4.so base: 0x%X", libUE4_base))

-- Offsets (from GameOffsets.h)
local GWorld_offset = 0xb036900
local GNames_offset = 0xae7b500
local UWorldToPersistentLevel = 0x30
local ULevelToAActors = 0x98
local RootComponentOffset = 0x130
local RelativeLocationOffset = 0x11C
local UObjectToClassPrivate = 0x10
local UObjectToNamePrivate = 0x18

if not _G.UE4ActorViewer then
    _G.UE4ActorViewer = {
        windowId = nil,
        callbackName = "drawUE4ActorViewerWindow",

        actors = {},
        showOnlyWithLocation = true,
        maxDisplay = 200,

        gworld = 0,
        level = 0,
        totalCount = 0,
        lastRefreshTime = 0,
        lastMessage = "",

        -- incremental refresh state
        refreshing = false,
        refreshIndex = 0,
        refreshLimit = 0,
        actorsData = 0,
        readPerFrame = 10,
        frameBudgetMs = 5,

        -- auto refresh
        autoRefresh = false,
        autoRefreshMs = 3000,
        nextAutoRefreshAt = 0,

        initialized = false
    }
end

local viewer = _G.UE4ActorViewer

local function nowMs()
    return time()
end

local function clamp(v, minV, maxV)
    if v < minV then return minV end
    if v > maxV then return maxV end
    return v
end

-- Get name entry from GNames table
local function GetNameEntry(id)
    if id < 0 then return 0 end

    local gname = libUE4_base + GNames_offset
    if gname == 0 then return 0 end

    local blockBit = 16
    local blocks = 0x40
    local chunckMask = bit.lshift(1, blockBit) - 1
    local stride = 2

    local block_offset = bit.rshift(id, blockBit) * 8
    local chunck_offset = bit.band(id, chunckMask) * stride

    local chunck = mem.readLong(gname + blocks + block_offset)
    if not chunck or chunck == 0 then return 0 end

    return chunck + chunck_offset
end

-- Get name string from name entry
local function GetNameEntryString(entry)
    if not entry or entry == 0 then return "" end

    local header = mem.readShort(entry)
    if not header then return "" end

    local strLen = bit.rshift(header, 6)
    if strLen <= 0 or strLen > 255 then return "" end

    local pStr = entry + 2
    local nameStr = mem.readString(pStr, strLen)
    if not nameStr then return "" end

    return nameStr
end

-- Get name by ID
local function GetNameByID(id)
    local entry = GetNameEntry(id)
    return GetNameEntryString(entry)
end

-- Get class name of an actor
local function GetClassName(actorAddr)
    if not actorAddr or actorAddr == 0 then return "Unknown" end

    local clazz = mem.readLong(actorAddr + UObjectToClassPrivate)
    if not clazz or clazz == 0 then return "Unknown" end

    local nameId = mem.readInt(clazz + UObjectToNamePrivate)
    if not nameId then return "Unknown" end

    return GetNameByID(nameId)
end

function viewer.startRefresh()
    viewer.actors = {}
    viewer.refreshing = false
    viewer.refreshIndex = 0
    viewer.refreshLimit = 0
    viewer.actorsData = 0

    local GWorld_addr = libUE4_base + GWorld_offset
    local gworld_ptr = mem.readLong(GWorld_addr)
    if not gworld_ptr or gworld_ptr == 0 then
        viewer.lastMessage = "Error: GWorld pointer is null"
        return false, viewer.lastMessage
    end

    local level_addr = mem.readLong(gworld_ptr + UWorldToPersistentLevel)
    if not level_addr or level_addr == 0 then
        viewer.lastMessage = "Error: PersistentLevel is null"
        return false, viewer.lastMessage
    end

    local actors_array_addr = level_addr + ULevelToAActors
    local actors_data = mem.readLong(actors_array_addr)
    local actors_count = mem.readInt(actors_array_addr + 8)

    if not actors_data or actors_data == 0 then
        viewer.lastMessage = "Error: Actors array pointer is null"
        return false, viewer.lastMessage
    end
    if not actors_count or actors_count < 0 then
        viewer.lastMessage = "Error: Invalid actors count"
        return false, viewer.lastMessage
    end

    viewer.gworld = gworld_ptr
    viewer.level = level_addr
    viewer.totalCount = actors_count
    viewer.actorsData = actors_data
    viewer.refreshLimit = math.min(actors_count, viewer.maxDisplay)
    viewer.refreshIndex = 0
    viewer.refreshing = true
    viewer.lastMessage = string.format("Refreshing... 0/%d", viewer.refreshLimit)
    return true, viewer.lastMessage
end

function viewer.stepRefresh()
    if not viewer.refreshing then
        return
    end

    local startMs = nowMs()
    local processed = 0

    while viewer.refreshing do
        if processed >= viewer.readPerFrame then
            break
        end
        if nowMs() - startMs >= viewer.frameBudgetMs then
            break
        end
        if viewer.refreshIndex >= viewer.refreshLimit then
            viewer.refreshing = false
            viewer.lastRefreshTime = os.time()
            viewer.lastMessage = string.format("Loaded %d/%d Actors", #viewer.actors, viewer.totalCount)
            break
        end

        local i = viewer.refreshIndex
        local actor_addr = mem.readLong(viewer.actorsData + i * 8)
        if actor_addr and actor_addr ~= 0 then
            local actor = {
                index = i,
                addr = actor_addr,
                rootComp = 0,
                hasLoc = false,
                x = 0,
                y = 0,
                z = 0,
                className = "Unknown"
            }

            -- Get class name
            actor.className = GetClassName(actor_addr)

            local root_comp = mem.readLong(actor_addr + RootComponentOffset)
            if root_comp and root_comp ~= 0 then
                actor.rootComp = root_comp
                actor.x = mem.readFloat(root_comp + RelativeLocationOffset) or 0
                actor.y = mem.readFloat(root_comp + RelativeLocationOffset + 4) or 0
                actor.z = mem.readFloat(root_comp + RelativeLocationOffset + 8) or 0
                actor.hasLoc = true
            end

            table.insert(viewer.actors, actor)
        end

        viewer.refreshIndex = viewer.refreshIndex + 1
        processed = processed + 1
    end

    if viewer.refreshing then
        viewer.lastMessage = string.format("Refreshing... %d/%d", viewer.refreshIndex, viewer.refreshLimit)
    end
end

function viewer.draw(windowId)
    viewer.maxDisplay = clamp(viewer.maxDisplay, 1, 2000)
    viewer.readPerFrame = clamp(viewer.readPerFrame, 1, 512)
    viewer.frameBudgetMs = clamp(viewer.frameBudgetMs, 1, 20)
    viewer.autoRefreshMs = clamp(viewer.autoRefreshMs, 500, 60000)

    if viewer.autoRefresh and not viewer.refreshing then
        local now = nowMs()
        if viewer.nextAutoRefreshAt == 0 or now >= viewer.nextAutoRefreshAt then
            viewer.startRefresh()
            viewer.nextAutoRefreshAt = now + viewer.autoRefreshMs
        end
    end

    viewer.stepRefresh()

    imgui.text("UE4 Actor Viewer")
    imgui.separator()

    imgui.text(string.format("Window ID: %d", windowId))
    imgui.text(string.format("GWorld: 0x%X | Level: 0x%X", viewer.gworld, viewer.level))
    imgui.text(string.format("Total: %d | Loaded: %d", viewer.totalCount, #viewer.actors))
    if viewer.lastRefreshTime > 0 then
        imgui.text(string.format("Last Refresh: %s", os.date("%H:%M:%S", viewer.lastRefreshTime)))
    end
    if viewer.lastMessage and viewer.lastMessage ~= "" then
        imgui.text(viewer.lastMessage)
    end
    if viewer.refreshing then
        local percent = 100.0
        if viewer.refreshLimit > 0 then
            percent = (viewer.refreshIndex * 100.0) / viewer.refreshLimit
        end
        imgui.text(string.format("Progress: %.1f%%", percent))
    end

    imgui.separator()

    if imgui.button(viewer.refreshing and "Refreshing..." or "Refresh Data") then
        viewer.startRefresh()
        viewer.nextAutoRefreshAt = nowMs() + viewer.autoRefreshMs
    end

    imgui.sameLine()
    viewer.showOnlyWithLocation = imgui.checkbox("Show Only With Location", viewer.showOnlyWithLocation)

    imgui.sameLine()
    viewer.autoRefresh = imgui.checkbox("Auto Refresh", viewer.autoRefresh)

    local newMaxDisplay = nil
    newMaxDisplay = imgui.inputInt("Max Display", viewer.maxDisplay)
    if newMaxDisplay and newMaxDisplay > 0 then
        viewer.maxDisplay = clamp(newMaxDisplay, 1, 2000)
    end

    local newReadPerFrame = nil
    newReadPerFrame = imgui.inputInt("Read/Frame", viewer.readPerFrame)
    if newReadPerFrame and newReadPerFrame > 0 then
        viewer.readPerFrame = clamp(newReadPerFrame, 1, 512)
    end

    local newBudgetMs = nil
    newBudgetMs = imgui.inputInt("Frame Budget (ms)", viewer.frameBudgetMs)
    if newBudgetMs and newBudgetMs > 0 then
        viewer.frameBudgetMs = clamp(newBudgetMs, 1, 20)
    end

    imgui.separator()

    local displayCount = 0
    if imgui.beginTable("ActorTable", 6, 1) then
        imgui.tableNextRow()
        imgui.tableNextColumn(); imgui.text("Index")
        imgui.tableNextColumn(); imgui.text("Class Name")
        imgui.tableNextColumn(); imgui.text("Actor Address")
        imgui.tableNextColumn(); imgui.text("RootComponent")
        imgui.tableNextColumn(); imgui.text("Location (X, Y, Z)")
        imgui.tableNextColumn(); imgui.text("Action")

        for _, actor in ipairs(viewer.actors) do
            if not viewer.showOnlyWithLocation or actor.hasLoc then
                displayCount = displayCount + 1

                imgui.tableNextRow()
                imgui.tableNextColumn()
                imgui.text(string.format("%d", actor.index))

                imgui.tableNextColumn()
                imgui.text(actor.className or "Unknown")

                imgui.tableNextColumn()
                imgui.text(string.format("0x%X", actor.addr))

                imgui.tableNextColumn()
                if actor.rootComp ~= 0 then
                    imgui.text(string.format("0x%X", actor.rootComp))
                else
                    imgui.textColored({0.5, 0.5, 0.5, 1.0}, "None")
                end

                imgui.tableNextColumn()
                if actor.hasLoc then
                    imgui.text(string.format("%.1f, %.1f, %.1f", actor.x, actor.y, actor.z))
                else
                    imgui.textColored({0.5, 0.5, 0.5, 1.0}, "None")
                end

                imgui.tableNextColumn()
                if imgui.smallButton("Details##" .. tostring(actor.index)) then
                    print(string.format("\n=== Actor #%d Details ===", actor.index))
                    print(string.format("Address: 0x%X", actor.addr))
                    print(string.format("RootComponent: 0x%X", actor.rootComp))
                    if actor.hasLoc then
                        print(string.format("Location: (%.2f, %.2f, %.2f)", actor.x, actor.y, actor.z))
                    else
                        print("Location: None")
                    end

                    -- Try to read Character-specific data (BP_PlayerBase_C)
                    local health_comp = mem.readLong(actor.addr + 0x4c0)
                    if health_comp and health_comp ~= 0 then
                        print(string.format("\n--- Character Data ---"))
                        print(string.format("HealthComponent: 0x%X", health_comp))

                        local hp = mem.readFloat(health_comp + 0xB0)
                        local maxhp = mem.readFloat(health_comp + 0xB8)
                        local stamina = mem.readFloat(health_comp + 0xB4)

                        if hp and maxhp then
                            print(string.format("Health: %.1f / %.1f (%.1f%%)", hp, maxhp, (hp / maxhp) * 100))
                        end
                        if stamina then
                            print(string.format("Stamina: %.1f", stamina))
                        end

                        -- Read Team and Dead status
                        local team = mem.readByte(actor.addr + 0x4c8)
                        local dead = mem.readByte(actor.addr + 0x4c9)
                        if team ~= nil then
                            print(string.format("Team: %d", team))
                        end
                        if dead ~= nil then
                            print(string.format("Dead: %s", dead == 1 and "Yes" or "No"))
                        end
                    end

                    -- Try to read weapon data (BP_BaseCharacter_C offsets)
                    local current_weapon = mem.readLong(actor.addr + 0x510)
                    local primary_weapon = mem.readLong(actor.addr + 0x518)
                    local secondary_weapon = mem.readLong(actor.addr + 0x520)

                    if current_weapon and current_weapon ~= 0 then
                        print(string.format("\n--- Weapon Data ---"))
                        print(string.format("CurrentWeapon: 0x%X", current_weapon))

                        -- Read weapon properties (ABP_BaseWeapon_C)
                        local ammo = mem.readInt(current_weapon + 0x24C)
                        local ammo_per_mag = mem.readInt(current_weapon + 0x248)
                        local damage = mem.readFloat(current_weapon + 0x250)
                        local weapon_range = mem.readFloat(current_weapon + 0x25C)

                        if ammo and ammo_per_mag then
                            print(string.format("Ammo: %d / %d", ammo, ammo_per_mag))
                        end
                        if damage then
                            print(string.format("Damage: %.1f", damage))
                        end
                        if weapon_range then
                            print(string.format("Range: %.1f", weapon_range))
                        end
                    end

                    if primary_weapon and primary_weapon ~= 0 then
                        print(string.format("PrimaryWeapon: 0x%X", primary_weapon))
                    end
                    if secondary_weapon and secondary_weapon ~= 0 then
                        print(string.format("SecondaryWeapon: 0x%X", secondary_weapon))
                    end
                end
            end
        end

        imgui.endTable()
    end

    imgui.separator()
    imgui.text(string.format("Displaying: %d Actors", displayCount))
end

function drawUE4ActorViewerWindow(windowId)
    viewer.draw(windowId)
end

local function ensureWindow()
    if viewer.windowId and imgui.isWindowOpen(viewer.windowId) then
        return
    end
    viewer.windowId = imgui.createWindow("UE4 Actor Viewer", viewer.callbackName)
    print(string.format("UE4 Actor Viewer window created, id=%d", viewer.windowId))
end

if not viewer.initialized then
    viewer.initialized = true
    local ok, msg = viewer.startRefresh()
    if not ok then
        print(msg)
    else
        print(msg)
    end
else
    -- Re-run script should not lose existing state; just restart refresh once.
    viewer.startRefresh()
end

ensureWindow()
print("Script loaded. Open 'UE4 Actor Viewer' window from Lua ImGui windows.")
