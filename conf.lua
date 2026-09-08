return {
	config_file = "hyperde.toml",
	compositor = {
		name = "chroma",
		panel_height = 28,
	},
	window_manager = {
		name = "hwms",
		workspaces = { "1", "2", "3", "4", "5", "6", "7", "8", "9" },
		normal_border = "#3c3836ff",
		focused_border = "#cc241dff",
		border_width = 2,
		focus_follow_mouse = true,
		floating_classes = { "dmenu", "dunst" },
	},
}
