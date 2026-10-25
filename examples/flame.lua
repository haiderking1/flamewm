-- every option flame's config has, set to what flame does on its own.
-- your config is ~/.config/flame/flame.lua: flame makes one the first time
-- it starts, take what you want from here into it. save it and flame
-- reloads right away. a mistake only shows in the log, flame keeps going on
-- whatever loaded before it

-- how it looks: the gaps between windows, their borders, and the color
-- behind everything
flame.set {
	gap = 6,
	border_width = 2,
	border_color = "#2a2a2a",
	border_focused_color = "#ff6a00",
	background = "#111111",
}

-- the wallpaper: a jpeg, png or webp, flame draws it itself
-- flame.set {
-- 	wallpaper = "~/walls/sea.jpg",
-- 	wallpaper_mode = "fill", -- "fit", "center", "stretch"
-- }

-- what locks the screen: super+l, and before the computer sleeps.
-- "swaylock" or any other locker, "" for nothing
flame.set {
	lock = "flamelock",
}

-- the keyboard: layouts, and how fast a held key repeats
flame.set {
	repeat_rate = 30, -- keys a second
	repeat_delay = 250, -- ms
	keyboard = {
		-- layout = "us,ara",
		-- options = "grp:alt_shift_toggle",
	},
}

-- the mouse: focus follows it, a held key to move and resize windows with,
-- the cursor
flame.set {
	focus_follows_mouse = true,
	drag_modifier = "super", -- held: left drag moves, right drag resizes
	cursor_size = 24,
	-- cursor_theme = "Adwaita",
}

-- X apps on a scaled screen: drawn at its real pixels and scaling
-- themselves (sharp, told the dpi), instead of being stretched (blurry).
-- apps that ignore dpi look small
flame.set {
	x_zero_scaling = false,
}

-- animations: windows opening, closing and moving around, the
-- workspace sliding over, bars and notifications coming and going.
-- `animations = false` turns them all off
flame.set {
	animations = {
		enabled = true,
		layers = true, -- bars, notifications, launchers
		open = "slide", -- slide, zoom, fade or none
		close = "slide",
		layer_open = "slide",
		layer_close = "slide",
		fade_in = true, -- on top of a slide or zoom
		fade_out = true,
		fade_in_from = 0.5,
		fade_out_from = 0.5,
		zoom_from = 0.4, -- a zoom opens from 40%
		zoom_to = 0.8, -- and closes to 80%
		workspace_vertical = false,
		focused_opacity = 1.0,
		unfocused_opacity = 1.0, -- say 0.9 and windows without focus fade a bit
		duration = { open = 400, close = 300, move = 500, workspace = 300, focus = 0 },
		-- css cubic-bezier curves: {x1, y1, x2, y2}
		curve = {
			open = { 0.46, 1.0, 0.29, 0.99 },
			close = { 0.46, 1.0, 0.29, 0.99 },
			move = { 0.46, 1.0, 0.29, 0.99 },
			workspace = { 0.46, 1.0, 0.29, 0.99 },
			focus = { 0.46, 1.0, 0.29, 0.99 },
			fade_in = { 0.46, 1.0, 0.29, 0.99 },
			fade_out = { 0.5, 0.5, 0.5, 0.5 },
		},
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
-- flame.rule({ app_id = "^mpv$" }, { animation = false })
-- flame.rule({ app_id = "^rofi$" }, { open = "zoom", close = "fade" })
-- flame.rule({ app_id = "^foot$" }, { opacity = 0.95 })

-- screens by connector name (flame logs them when they come up), "*" for
-- every screen without its own. scale: hidpi, fractional ones work too
-- flame.output("DP-1", { scale = 1.5 })
-- flame.output("*", { scale = 1 })

-- things to start with flame, once
-- flame.autostart("waybar")
