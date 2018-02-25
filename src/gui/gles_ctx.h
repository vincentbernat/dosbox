#include "dosbox.h"
#if C_GLES_RPI

#include <stdbool.h>
void pi_gles_init(int src_width, int src_height, int bpp, bool maintain_aspect);
void pi_gles_deinit(void);
void pi_gles_videoflip();

#endif
