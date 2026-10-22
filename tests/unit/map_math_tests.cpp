// helpers/ui/map_math.c: the touch UI's map projection, distances and scale.
#include "test.h"
#include "ui/map_math.h"

#include <cmath>
#include <string>

namespace {
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}

TEST(map_project_tiles, "UNIT-MAP-001", "Web Mercator matches slippy-map tile numbers") {
    double x, y;
    map_project(0, 0, 0, &x, &y);
    CHECK(near(x, 128.0, 1e-9) && near(y, 128.0, 1e-9));
    // Rome, Colosseum (41.8902, 12.4922) is tile 13/4380/3044 (OSM tile formula).
    map_project(41890200, 12492200, 13, &x, &y);
    CHECK((int)(x / 256) == 4380 && (int)(y / 256) == 3044);
    // Latitude past the Mercator limit is clamped, not infinite.
    map_project(90000000, 0, 2, &x, &y);
    CHECK(std::isfinite(y) && y >= 0.0 && y < 1.0);
}

TEST(map_distance, "UNIT-MAP-002", "Distances: haversine, and the text the UI shows") {
    // Rome - Milan, about 477 km.
    double m = map_distance_m(41902800, 12496400, 45464200, 9190000);
    CHECK(near(m, 477000.0, 3000.0));
    CHECK(map_distance_m(41902800, 12496400, 41902800, 12496400) == 0.0);
    char b[16];
    map_fmt_distance(849.6, b, sizeof(b));  CHECK(std::string(b) == "850 m");
    map_fmt_distance(3240, b, sizeof(b));   CHECK(std::string(b) == "3.2 km");
    map_fmt_distance(99960, b, sizeof(b));  CHECK(std::string(b) == "100 km");
    map_fmt_distance(477400, b, sizeof(b)); CHECK(std::string(b) == "477 km");
}

TEST(map_scale_bar, "UNIT-MAP-003", "Scale bar: a 1-2-5 distance no longer than asked") {
    char l[16];
    // Equator, zoom 0: about 156 km per pixel; 100 px fits 10000 km.
    int px = map_scale(0, 0, 100, l, sizeof(l));
    CHECK(std::string(l) == "10000 km" && px <= 100 && px > 50);
    // 45 N, zoom 13: about 13.5 m per pixel; 80 px fits 1 km.
    px = map_scale(45000000, 13, 80, l, sizeof(l));
    CHECK(std::string(l) == "1 km" && px <= 80 && px >= 70);
    px = map_scale(45000000, 18, 80, l, sizeof(l));
    CHECK(std::string(l) == "20 m" && px <= 80);
}

TEST(map_fit, "UNIT-MAP-004", "Fit zoom: the largest zoom that holds the box") {
    double x0, y0, x1, y1;
    map_project(41950000, 12400000, 0, &x0, &y0);   // NW corner
    map_project(41850000, 12600000, 0, &x1, &y1);   // SE corner: 146 x 98 px at zoom 10
    int z = map_fit_zoom(x0, y0, x1, y1, 280, 160);
    CHECK(z == 10);
    CHECK(map_fit_zoom(x0, y0, x0, y0, 280, 160) == MAP_ZOOM_MAX);
    CHECK(map_fit_zoom(0, 0, 256, 256, 280, 160) == MAP_ZOOM_MIN);
}
