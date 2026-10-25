-- every option flame's config has, set to what flame does on its own.
-- your config is ~/.config/flame/flame.lua: flame makes one the first time
-- it starts, take what you want from here into it. save it and flame
-- reloads right away. a mistake only shows in the log, flame keeps going on
-- whatever loaded before it

-- how it looks: the gaps between windows, their borders, and how much
-- windows without focus fade
flame.look {
	gap = 6,
	border_width = 2,
	border_color = "#2a2a2a",
	border_focused_color = "#ff6a00",
	unfocused_opacity = 1.0, -- say 0.9 and windows without focus fade a bit
}

-- the wallpaper: a jpeg, png or webp, flame draws it itself
-- flame.wallpaper {
-- 	path = "~/walls/sea.jpg",
-- 	mode = "fill", -- "fit", "center", "stretch"
-- }

-- what locks the screen: super+l, and before the computer sleeps.
-- "swaylock" or any other locker, "" for nothing
flame.lockscreen {
	command = "flamelock",
}

-- the keyboard: layouts, and how fast a held key repeats
-- flame.keyboard {
-- 	layout = "us,ara",
-- 	options = "grp:alt_shift_toggle",
-- 	repeat_rate = 30, -- keys a second
-- 	repeat_delay = 250, -- ms
-- }

-- the mouse: focus follows it, a held key to move and resize windows with,
-- the cursor
flame.mouse {
	focus_follows = true,
	drag_modifier = "super", -- held: left drag moves, right drag resizes
	cursor_size = 24,
	-- cursor_theme = "Adwaita",
}

-- X apps: on a scaled screen drawn at its real pixels and scaling
-- themselves (sharp, told the dpi) instead of stretched (blurry), apps that
-- ignore dpi look small. global_keys: which keys X apps see while a wayland
-- window has focus (push to talk): "always", "never", "modifiers", "combos"
flame.x11 {
	zero_scaling = false,
	global_keys = "always",
}

-- glass: how far apps' blur reaches, and a fine grain over it so dark blur
-- doesn't band (0 for none)
flame.blur {
	size = 18,
	noise = 0.004,
}

-- animations: on or off for all of them, curves named once ({x1, y1, x2,
-- y2}, like css cubic-bezier), then each thing that moves: how long (ms),
-- along which curve, how it looks
flame.animations {
	enabled = true,
	curves = {
		smooth = { 0.46, 1.0, 0.29, 0.99 },
		linear = { 0, 0, 1, 1 },
	},
	open      = { ms = 400, curve = "smooth", style = "slide" }, -- slide, zoom 40%, fade, none
	close     = { ms = 300, curve = "smooth", style = "slide" },
	move      = { ms = 500, curve = "smooth" },
	workspace = { ms = 300, curve = "smooth", style = "slide" }, -- or slidevert
	fade_in   = { from = 0.5, curve = "smooth" },
	fade_out  = { from = 0.5, curve = "linear" },
	layers    = { open = "slide", close = "slide" }, -- bars, notifications, launchers
}

-- the binds flame has on its own, as an example of saying them yourself.
-- binding the same keys again replaces them, flame.unbind and
-- flame.clear_binds() take them away
flame.bind("super", "q", "foot")
flame.bind("super", "c", flame.close)
flame.bind("super", "j", function() flame.focus("next") end)
flame.bind("super", "k", function() flame.focus("prev") end)
flame.bind("super", "f", flame.fullscreen)
flame.bind("super+shift", "space", flame.toggle_floating)
flame.bind("super+shift", "q", flame.quit)
for i = 1, 10 do
	local key = tostring(i % 10) -- 0 is the 10th
	flame.bind("super", key, function() flame.workspace(i) end)
	flame.bind("super+shift", key, function() flame.move_to_workspace(i) end)
end

-- window rules: lua patterns on app_id and title, the last match wins
-- flame.rule({ app_id = "^pavucontrol$" }, { floating = true })
-- flame.rule({ title = "Picture.in.Picture" }, { floating = true })
-- flame.rule({ app_id = "^steam$" }, { workspace = 5 })
-- flame.rule({ app_id = "^mpv$" }, { animation = false })
-- flame.rule({ app_id = "^rofi$" }, { open = "zoom", close = "fade" })
-- flame.rule({ app_id = "^foot$" }, { opacity = 0.95 })

-- screens by connector name (flame logs them when they come up), "*" for
-- every screen without its own. scale: hidpi, fractional ones work too
-- flame.output("DP-1", { scale = 1.5 })
-- flame.output("*", { scale = 1 })

-- things to start with flame, once
-- flame.autostart("waybar")
