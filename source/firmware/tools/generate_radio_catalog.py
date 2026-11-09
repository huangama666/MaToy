"""Regenerate the fixed catalogue from the checked station inventory."""
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
data = json.loads((root / 'assets/radio/stations.json').read_text())
stations, regions = data['stations'], data['regions']
assert len({item['id'] for item in stations}) == len(stations)
offset = 0
for region in regions:
    assert region['first'] == offset and region['count'] > 0
    offset += region['count']
assert offset == len(stations) <= 65535
default = next(i for i, item in enumerate(stations) if item['id'] == 266)
quote = lambda value: json.dumps(value, ensure_ascii=False)
header = """// Public station catalogue. Source and probe date are in assets/radio/stations.json.
#pragma once

#include <stdint.h>

typedef struct { uint32_t id; const char *name; const char *frequency; } radio_station_t;
typedef struct { const char *name; uint16_t first, count; } radio_region_t;

"""
header += f'#define RADIO_CHANNEL_COUNT {len(stations)}\n#define RADIO_REGION_COUNT {len(regions)}\n#define RADIO_DEFAULT_STATION {default}\n\n'
header += 'static const radio_station_t radio_stations[RADIO_CHANNEL_COUNT] = {\n'
for item in stations:
    header += f"    {{{item['id']}, {quote(item['name'])}, {quote(item['frequency'])}}},\n"
header += '};\n\nstatic const radio_region_t radio_regions[RADIO_REGION_COUNT] = {\n'
for item in regions:
    header += f"    {{{quote(item['name'])}, {item['first']}, {item['count']}}},\n"
header += '};\n'
(root / 'main/radio_catalog.h').write_text(header)
print(f'{len(stations)} stations, {len(regions)} regions')
