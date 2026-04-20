-- UE4 Actor List Viewer (Console Text Version)
-- Display Actor data from UE4 Level in console

print("=== UE4 Actor List Viewer ===\n")

-- Module base address (adjust according to actual situation)
local libUE4_base = 0x722cc6e000

-- Offset definitions (from GameOffsets.h)
local GWorld_offset = 0xb036900
local UWorldToPersistentLevel = 0x30
local ULevelToAActors = 0x98
local RootComponentOffset = 0x130
local RelativeLocationOffset = 0x11C

-- Calculate GWorld address
local GWorld_addr = libUE4_base + GWorld_offset
print(string.format("GWorld Address: 0x%X", GWorld_addr))

-- Read GWorld pointer
local gworld_ptr = mem.readLong(GWorld_addr)
if not gworld_ptr or gworld_ptr == 0 then
    print("Error: GWorld pointer is null")
    return
end
print(string.format("GWorld Pointer: 0x%X", gworld_ptr))

-- Read PersistentLevel
local level_addr = mem.readLong(gworld_ptr + UWorldToPersistentLevel)
if not level_addr or level_addr == 0 then
    print("Error: PersistentLevel is null")
    return
end
print(string.format("PersistentLevel Address: 0x%X", level_addr))

-- Read Actors TArray
local actors_array_addr = level_addr + ULevelToAActors
local actors_data = mem.readLong(actors_array_addr)
local actors_count = mem.readInt(actors_array_addr + 8)

print(string.format("\nActors Array Address: 0x%X", actors_data))
print(string.format("Total Actors: %d\n", actors_count))
print(string.rep("=", 100))

-- Read and display Actors
local max_display = math.min(actors_count, 50)
local valid_count = 0
local with_location = 0

for i = 0, max_display - 1 do
    local actor_addr = mem.readLong(actors_data + i * 8)

    if actor_addr and actor_addr ~= 0 then
        valid_count = valid_count + 1

        -- Read RootComponent and location
        local root_comp = mem.readLong(actor_addr + RootComponentOffset)
        local has_location = false
        local loc_x, loc_y, loc_z = 0, 0, 0

        if root_comp and root_comp ~= 0 then
            loc_x = mem.readFloat(root_comp + RelativeLocationOffset)
            loc_y = mem.readFloat(root_comp + RelativeLocationOffset + 4)
            loc_z = mem.readFloat(root_comp + RelativeLocationOffset + 8)

            if loc_x and loc_y and loc_z then
                has_location = true
                with_location = with_location + 1
            end
        end

        -- Display Actor info
        print(string.format("[%3d] Actor: 0x%012X | RootComp: 0x%012X",
            i, actor_addr, root_comp or 0))

        if has_location then
            print(string.format("      Location: X=%8.2f, Y=%8.2f, Z=%8.2f", loc_x, loc_y, loc_z))
        else
            print("      Location: None")
        end

        print("")
    end
end

print(string.rep("=", 100))
print(string.format("\nStatistics:"))
print(string.format("  Total Actors: %d", actors_count))
print(string.format("  Scanned: %d", max_display))
print(string.format("  Valid Actors: %d", valid_count))
print(string.format("  With Location: %d", with_location))

if actors_count > max_display then
    print(string.format("  Not Displayed: %d", actors_count - max_display))
end

print("\nData reading completed!")
