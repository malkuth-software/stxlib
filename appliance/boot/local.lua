--
-- local.lua -- the appliance's A/B root slot choice (C1 step S4), included
-- by the pinned base's loader.lua after the configuration is read
-- (stand/lua/loader.lua:50). Installed on the boot-control partition as
-- /boot/lua/local.lua. Design: appliance/disk/LAYOUT.md.
--
-- Reads the slot file on the ESP (good=, try=) and the try count on the
-- boot-control partition; rewrites the count in place (libsa UFS writes
-- overwrite and never extend, stand/libsa/ufs.c:797-823, as nextboot does,
-- stand/lua/config.lua:486-499); switches to the slot as the stock
-- boot-environment switch does (stand/lua/core.lua:595-607); sets
-- stx.boot.slot, stx.boot.tries and stx.boot.fallback for the kernel
-- environment. Every error ends in a boot of a known slot, never a stop.
--

local config = require("config")

local tries_max = 3
local tries_path = "/stx/tries"
local tries_size = 16
local slot_file_name = "/stx/slots.conf"
local slot_file_size_max = 256
-- The ESP's loader device, set in the ESP's /efi/freebsd/loader.env next to
-- rootdev (VM: disk0p1:, Pi: disk0s1:).
local esp_device = loader.getenv("stx.esp_device")

local slot_labels = { A = "STXROOTA", B = "STXROOTB" }
local slot_devices = {
	A = loader.getenv("stx.slot_a_device"),
	B = loader.getenv("stx.slot_b_device"),
}

local function say(text)
	print("STXLOADER " .. text)
end

-- Reads at most size_max bytes of a file; nil when it cannot.
local function read_small(path, size_max)
	local file = io.open(path)
	if file == nil then
		return nil
	end
	local text = io.read(file, size_max)
	io.close(file)
	return text
end

local function valid_slot(name)
	return name == "A" or name == "B"
end

-- good= and try= from the slot file; each nil when absent or invalid.
local function read_slot_file()
	if esp_device == nil then
		return nil, nil
	end
	local text = read_small(esp_device .. slot_file_name, slot_file_size_max)
	if text == nil then
		return nil, nil
	end
	local good = text:match("good=(%u)")
	local try = text:match("try=(%u)")
	if not valid_slot(good) then
		good = nil
	end
	if not valid_slot(try) then
		try = nil
	end
	return good, try
end

-- The try count, 0..9, or nil when the file is missing or malformed.
local function read_tries()
	local text = read_small(tries_path, tries_size)
	if text == nil then
		return nil
	end
	local digit = text:match("^tries=(%d)")
	if digit == nil then
		return nil
	end
	return tonumber(digit)
end

-- Rewrites the whole fixed-size file in place; true when written.
local function write_tries(count)
	local line = "tries=" .. tostring(count)
	line = line .. string.rep(" ", tries_size - 1 - #line) .. "\n"
	local file = io.open(tries_path, "w")
	if file == nil then
		return false
	end
	local written = io.write(file, line)
	io.close(file)
	return written == tries_size
end

-- The choice: slot to boot, tries to report, fallback flag, reason.
local function choose()
	local good, try = read_slot_file()
	if good == nil then
		return "A", 0, true, "slot file unreadable"
	end
	if try == nil or try == good then
		return good, 0, false, "good"
	end
	local tries = read_tries()
	if tries == nil then
		return good, 0, true, "try count unreadable"
	end
	if tries >= tries_max then
		return good, tries, true, "tries exhausted"
	end
	if not write_tries(tries + 1) then
		return good, tries, true, "try count not writable"
	end
	return try, tries + 1, false, "trying"
end

local slot, tries, fallback, reason = choose()
local device = slot_devices[slot]
if device == nil then
	say("no device for slot " .. slot .. "; staying on the current device")
else
	loader.setenv("vfs.root.mountfrom", "ufs:/dev/ufs/" .. slot_labels[slot])
	loader.setenv("currdev", device)
	config.reload()
end
loader.setenv("stx.boot.slot", slot)
loader.setenv("stx.boot.tries", tostring(tries))
loader.setenv("stx.boot.fallback", fallback and "1" or "0")
say("slot=" .. slot .. " tries=" .. tostring(tries) ..
    " fallback=" .. (fallback and "1" or "0") .. " reason=" .. reason)
