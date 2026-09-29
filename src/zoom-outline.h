#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t zoom_outline_owner_id;

typedef struct zoom_display_frame {
	double x;
	double y;
	double w;
	double h;
} zoom_display_frame;

typedef struct zoom_outline_params {
	double display_x;
	double display_y;
	double display_w;
	double display_h;
	double region_x;
	double region_y;
	double region_w;
	double region_h;
	int outline_thickness;
	int dim_opacity;
} zoom_outline_params;

uint32_t zoom_display_id_from_uuid(const char *uuid_str);
bool zoom_display_frame_for_id(uint32_t display_id, zoom_display_frame *out);
void zoom_cursor_normalized_on_display(uint32_t display_id, float *out_x, float *out_y, bool *on_display);

void zoom_outline_show_for(zoom_outline_owner_id owner, const zoom_outline_params *params);
void zoom_outline_update_for(zoom_outline_owner_id owner, const zoom_outline_params *params);
void zoom_outline_hide_for(zoom_outline_owner_id owner);
void zoom_outline_destroy_for(zoom_outline_owner_id owner);
void zoom_outline_shutdown(void);

#ifdef __cplusplus
}
#endif
