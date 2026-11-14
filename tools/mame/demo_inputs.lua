-- MAME 0.264 reference capture for Dragon Blaze (development tool).
-- Plays the same input sequence as `dragnblz_headless --inputs demo` and
-- dumps work RAM (big-endian) plus a snapshot after the listed frames.
-- usage: mame dragnblz -nodrc -autoboot_script demo_inputs.lua -nvram_directory <empty dir> ...
local frame = 0
local dumps = {}
for _, f in ipairs({1800, 2400, 3000, 3600, 4200, 4800}) do dumps[f] = true end
local inp = manager.machine.ioport.ports[":INPUTS"].fields
local function set(name, on) inp[name]:set_value(on and 1 or 0) end
local function apply(k)
  set("Coin 1", k >= 1500 and k < 1505)
  set("1 Player Start", k >= 1600 and k < 1606)
  local play = k >= 1700
  local right = play and (k // 60) % 2 == 1
  set("P1 Right", right)
  set("P1 Left", play and not right)
  set("P1 Up", play and (k // 45) % 3 == 0)
  set("P1 Button 1", play and k % 8 < 4)
  set("P1 Button 2", play and k % 600 >= 300 and k % 600 < 303)
end
apply(0)
fs = emu.add_machine_frame_notifier(function()
  frame = frame + 1
  if dumps[frame] then
    manager.machine.video:snapshot()
    local s = manager.machine.devices[":maincpu"].spaces["program"]
    local f = io.open(string.format("demo_ram_%d.bin", frame), "wb"); local t = {}
    for a = 0, 0xFFFFF, 4 do t[#t+1] = string.pack(">I4", s:read_u32(0x06000000 + a)) end
    f:write(table.concat(t)); f:close()
  end
  apply(frame)
  if frame == 4800 then manager.machine:exit() end
end)
