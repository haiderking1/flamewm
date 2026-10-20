-- flame's config. copy it to ~/.config/flame/init.lua and change what you
-- like: everything here is what flame does without one. super+shift+r (or
-- `flamectl reload`) loads it again, a mistake only shows in the log and
-- flame keeps going on whatever loaded before it

flame.set {
	gap = 6,
	border_width = 2,
	border_color = "#2a2a2a",
	border_focused_color = "#ff6a00",
	background = "#111111",
	repeat_rate = 30, -- keys a second
	repeat_delay = 250, -- ms
	focus_follows_mouse = true,
	drag_modifier = "super", -- held: left drag moves, right drag resizes
	cursor_size = 24,
	-- cursor_theme = "Adwaita",
	keyboard = {
		-- layout = "us,ara",
		-- options = "grp:alt_shift_toggle",
	},
}

-- the binds flame has on its own, as an example of saying them yourself.
-- binding the same keys again replaces them, flame.unbind and
-- flame.clear_binds() take them away
flame.bind("super", "Return", "foot")
flame.bind("super", "q", "foot")
flame.bind("super", "c", flame.close)
flame.bind("super", "j", function() flame.focus("next") end)
flame.bind("super", "k", function() flame.focus("prev") end)
flame.bind("super", "f", flame.fullscreen)
flame.bind("super+shift", "space", flame.toggle_floating)
flame.bind("super+shift", "q", flame.quit)
flame.bind("super+shift", "r", flame.reload)
for i = 1, 10 do
	local key = tostring(i % 10) -- 0 is the 10th
	flame.bind("super", key, function() flame.workspace(i) end)
	flame.bind("super+shift", key, function() flame.move_to_workspace(i) end)
end

-- window rules: lua patterns on app_id and title, the last match wins
-- flame.rule({ app_id = "^pavucontrol$" }, { floating = true })
-- flame.rule({ title = "Picture.in.Picture" }, { floating = true })
-- flame.rule({ app_id = "^steam$" }, { workspace = 5 })

-- things to start with flame, once
-- flame.autostart("waybar")
