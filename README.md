ttywm - a window manager that looks like a tty
==============================================
Every screen is a full screen shell, in the console's own font and colours.
Windows float on top of it in frames drawn with that font:

	┌─[ firefox ]──────────────[x]┐
	│                             │
	└─────────────────────────────┘

The bottom line shows the network, volume, battery, date, time and which tty
you're on. Click the volume to mute, scroll on it to change it, click the
network to set up wifi.

X11 only, one C file, needs libX11, libXft, xterm, and rofi for the apps menu.

Install
-------
	make
	sudo make install

then pick ttywm on your login screen, or put this in ~/.xinitrc and run startx:

	exec ttywm-session

Settings
--------
Everything is in ~/.config/ttywm/config, made the first time ttywm starts.
It's plain words, like

	window colour = light green
	battery = off
	super+w = firefox

Change a line and save: ttywm picks it up straight away.

Keys
----
	Super+Return        terminal
	Super+d             apps menu
	Super+q             close window
	Super+Tab           next window, then the shell
	Super+f             full screen
	Super+1..9          go to tty1..tty9
	Super+Shift+1..9    send window to that tty
	Super+Shift+e       log out
	Super+drag          move
	Super+right drag    resize
	volume keys         volume

Click [x] to close, drag the title to move, drag the bottom right corner to
resize, click the shell to type in it. Exit the shell and you get a new one.
