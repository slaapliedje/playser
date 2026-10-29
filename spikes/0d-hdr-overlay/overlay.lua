-- Spike 0d: composite a premultiplied BGRA UI bar (rendered elsewhere, SDR)
-- over HDR video with overlay-add, the mechanism the app would use for its
-- OSD while mpv owns an HDR swapchain.
local dir = mp.get_script_directory() or mp.get_opt("overlay-dir")
local file = (mp.get_opt("overlay-file") or "/home/deck/discnav-test/overlay.bgra")
local function show()
    local w, h = mp.get_osd_size()
    local y = (h or 2160) - 400 - 80
    mp.command_native({"overlay-add", 0, 0, y, file, 0, "bgra", 3840, 400, 3840 * 4})
    mp.msg.info("overlay-add at y=" .. y .. " osd " .. tostring(w) .. "x" .. tostring(h))
end
mp.register_event("file-loaded", function() mp.add_timeout(1, show) end)
