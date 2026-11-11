// FPV video channels per platform, from Oscar Liang's 5.8 GHz channel chart
// (https://oscarliang.com/fpv-channels/), as in the Android app's Channels tab
// (app/src/main/assets/bridge/channels-view.js). GPL-3.0-or-later.
#pragma once
#include <stdint.h>

struct Channel {
    const char *name;
    uint16_t mhz;
};

struct Platform {
    const char *id;     // kept in settings
    const char *label;  // row label
    uint8_t bw;         // MHz
    uint8_t count;
    Channel ch[10];
};

static const Platform PLATFORMS[] = {
    {"R", "Analog R", 30, 8, {{"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769}, {"R5", 5806}, {"R6", 5843}, {"R7", 5880}, {"R8", 5917}}},
    {"F", "Analog F", 30, 8, {{"F1", 5740}, {"F2", 5760}, {"F3", 5780}, {"F4", 5800}, {"F5", 5820}, {"F6", 5840}, {"F7", 5860}, {"F8", 5880}}},
    {"A", "Analog A", 30, 8, {{"A8", 5725}, {"A7", 5745}, {"A6", 5765}, {"A5", 5785}, {"A4", 5805}, {"A3", 5825}, {"A2", 5845}, {"A1", 5865}}},
    {"B", "Analog B", 30, 8, {{"B1", 5733}, {"B2", 5752}, {"B3", 5771}, {"B4", 5790}, {"B5", 5809}, {"B6", 5828}, {"B7", 5847}, {"B8", 5866}}},
    {"E", "Analog E", 30, 8, {{"E4", 5645}, {"E3", 5665}, {"E2", 5685}, {"E1", 5705}, {"E5", 5885}, {"E6", 5905}, {"E7", 5925}, {"E8", 5945}}},
    {"v1-25", "DJI V1 20M", 20, 8, {{"1", 5660}, {"2", 5695}, {"3", 5735}, {"4", 5770}, {"5", 5805}, {"8", 5839}, {"6", 5878}, {"7", 5914}}},
    {"v1-50", "DJI V1 40M", 40, 4, {{"1", 5695}, {"2", 5770}, {"8", 5839}, {"3", 5878}}},
    {"o3-10", "DJI O3 10M", 10, 7, {{"1", 5669}, {"2", 5705}, {"3", 5768}, {"4", 5804}, {"5", 5839}, {"6", 5876}, {"7", 5912}}},
    {"o3-20", "DJI O3 20M", 20, 7, {{"1", 5669}, {"2", 5705}, {"3", 5768}, {"4", 5804}, {"5", 5839}, {"6", 5876}, {"7", 5912}}},
    {"o3-40", "DJI O3 40M", 40, 3, {{"1", 5677}, {"2", 5794}, {"3", 5902}}},
    // HDZero: Raceband only (F2 and F4 would overlap R3-R5 in one row).
    {"hdz", "HDZero", 27, 8, {{"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769}, {"R5", 5806}, {"R6", 5843}, {"R7", 5880}, {"R8", 5917}}},
    {"ws-race", "WS Race", 16, 8, {{"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769}, {"R5", 5806}, {"R6", 5843}, {"R7", 5880}, {"R8", 5917}}},
    {"ws-25", "WS 25M", 20, 8, {{"1", 5660}, {"2", 5695}, {"3", 5735}, {"4", 5770}, {"5", 5805}, {"8", 5839}, {"6", 5878}, {"7", 5914}}},
    {"ws-50", "WS 50M", 40, 4, {{"1", 5695}, {"2", 5770}, {"8", 5839}, {"3", 5878}}},
};
static const int PLATFORM_COUNT = sizeof(PLATFORMS) / sizeof(PLATFORMS[0]);
// The rows shown until the user picks others (indexes into PLATFORMS).
static const uint8_t DEFAULT_ROWS[] = {0, 5, 8};  // Analog R, DJI V1 20M, DJI O3 20M
