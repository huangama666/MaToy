#include "vnc_geometry.h"

viewport_t vnc_fit(unsigned sw, unsigned sh, unsigned mw, unsigned mh)
{
    if (!sw || !sh || !mw || !mh) return (viewport_t){0};
    unsigned w = mw, h = mh;
    if ((uint64_t)sw * mh >= (uint64_t)sh * mw) {
        h = (uint64_t)sh * mw / sw;
        if (!h) h = 1;
    } else {
        w = (uint64_t)sw * mh / sh;
        if (!w) w = 1;
    }
    return (viewport_t){(mw - w) / 2, (mh - h) / 2, w, h};
}

unsigned vnc_edge(unsigned source, unsigned scaled, unsigned original)
{
    return ((uint64_t)source * scaled + original - 1) / original;
}
