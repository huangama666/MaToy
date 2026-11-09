#pragma once
#include <stdint.h>

typedef struct { unsigned x, y, w, h; } viewport_t;
viewport_t vnc_fit(unsigned source_w, unsigned source_h, unsigned max_w, unsigned max_h);
unsigned vnc_edge(unsigned source, unsigned scaled, unsigned original);
