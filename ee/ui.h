#ifndef PX_UI_H
#define PX_UI_H

#include "px_stats.h"

int ui_init(int off_x, int off_y);
void ui_move(int off_x, int off_y);
int ui_off_x(void);
int ui_off_y(void);
/* message: optional footer text; calibrating: show the screen_x/y hint */
void ui_draw(const px_stats_t *s, const char *cfg_src, float cur, float avg, const char *message, int calibrating);

#endif
