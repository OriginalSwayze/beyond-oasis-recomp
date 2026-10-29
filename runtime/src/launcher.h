/* Launcher shown before the game (launcher.c). */
#ifndef LAUNCHER_H
#define LAUNCHER_H

/* Shows the launcher window; saves settings to ini_path. Returns 1 to play, 0 to quit. */
int launcher_run(const char *ini_path);
/* Renders one launcher frame, `width` pixels wide, with light direction (lx, ly) in
 * -1..1 and time t, to a PNG (for checking the look without a window). */
int launcher_screenshot(const char *png, int width, float lx, float ly, float t);

#endif
