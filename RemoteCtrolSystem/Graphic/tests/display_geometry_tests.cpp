#include "../ScreenDisplay.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void Check(bool value, const char* description) {
    if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
void CheckRect(RECT rect, int left, int top, int right, int bottom, const char* description) {
    Check(rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom, description);
}
void CheckAspect(const ScreenViewGeometry& geometry, int sourceWidth, int sourceHeight) {
    const RECT rect = geometry.GetDisplayRect();
    const std::int64_t width = rect.right - rect.left, height = rect.bottom - rect.top;
    Check(width >= 1 && height >= 1, "nonempty raster rectangle");
    // One pixel is the minimum representable axis, including extreme panoramas.
    Check(std::abs(width * sourceHeight - height * sourceWidth) <= (std::max)(sourceWidth, sourceHeight),
          "source aspect ratio within one raster pixel");
}
void CheckPoint(const ScreenViewGeometry& geometry, int x, int y, int expectedX, int expectedY) {
    int remoteX = -1, remoteY = -1;
    Check(geometry.MapWindowPointToServer(x, y, remoteX, remoteY), "visible image point accepted");
    Check(remoteX == expectedX && remoteY == expectedY, "source pixel coordinate");
}
void CheckRejected(const ScreenViewGeometry& geometry, int x, int y) {
    int remoteX = -1, remoteY = -1;
    Check(!geometry.MapWindowPointToServer(x, y, remoteX, remoteY), "black bar/outside viewport rejected");
    Check(remoteX == -1 && remoteY == -1, "rejected mapping leaves output untouched");
}
}

int main() {
    ScreenViewGeometry landscape;
    CheckRejected(landscape, 0, 0);
    Check(landscape.SetSourceSize(1920, 1080) && landscape.SetViewportSize(800, 600), "landscape setup");
    CheckRect(landscape.GetDisplayRect(), 0, 75, 800, 525, "landscape fit leaves horizontal black bars");
    CheckAspect(landscape, 1920, 1080);
    CheckPoint(landscape, 0, 75, 0, 0);
    CheckPoint(landscape, 799, 524, 1919, 1079);
    CheckPoint(landscape, 400, 300, 960, 540);
    CheckRejected(landscape, 400, 74); CheckRejected(landscape, 400, 525);
    CheckRejected(landscape, -1, 300); CheckRejected(landscape, 800, 300);

    ScreenViewGeometry portrait;
    Check(portrait.SetSourceSize(1080, 1920) && portrait.SetViewportSize(800, 600), "portrait source");
    CheckRect(portrait.GetDisplayRect(), 231, 0, 569, 600, "portrait source centred with vertical black bars");
    CheckAspect(portrait, 1080, 1920);
    CheckPoint(portrait, 231, 0, 0, 0);
    CheckPoint(portrait, 568, 599, 1079, 1919);
    CheckRejected(portrait, 230, 300); CheckRejected(portrait, 569, 300);
    Check(portrait.SetViewportSize(600, 1000), "portrait viewport resize");
    CheckAspect(portrait, 1080, 1920);
    RECT rect = portrait.GetDisplayRect();
    Check(rect.left >= 0 && rect.top >= 0 && rect.right <= 600 && rect.bottom <= 1000, "whole desktop fits portrait viewport");

    Check(landscape.Zoom(2.0), "zoom in");
    CheckRect(landscape.GetDisplayRect(), -400, -150, 1200, 750, "zoom centred and clipped to viewport");
    CheckAspect(landscape, 1920, 1080);
    CheckPoint(landscape, 400, 300, 960, 540);
    CheckRejected(landscape, -1, 300);
    Check(landscape.SetViewportSize(600, 1000), "resize while zoomed");
    CheckRect(landscape.GetDisplayRect(), -300, 162, 900, 837, "resize retains zoom using original aspect");
    CheckAspect(landscape, 1920, 1080);
    landscape.Fit();
    CheckRect(landscape.GetDisplayRect(), 0, 331, 600, 669, "fit resets zoom without distortion");
    Check(landscape.SetSourceSize(1080, 1920), "remote resolution/orientation change");
    CheckAspect(landscape, 1080, 1920);
    rect = landscape.GetDisplayRect();
    Check(rect.left >= 0 && rect.top >= 0 && rect.right <= 600 && rect.bottom <= 1000, "new source still fully visible");

    // Repeated zooming recalculates from source dimensions, so rounding errors do not accumulate.
    for (int i = 0; i < 200; ++i) {
        landscape.Zoom(1.25); CheckAspect(landscape, 1080, 1920);
        landscape.Zoom(0.8); CheckAspect(landscape, 1080, 1920);
    }
    CheckRect(landscape.GetDisplayRect(), rect.left, rect.top, rect.right, rect.bottom, "zoom round trip has no drift");

    ScreenViewGeometry extreme;
    Check(extreme.SetSourceSize(16384, 1) && extreme.SetViewportSize(1, 32768), "extreme panorama");
    CheckAspect(extreme, 16384, 1);
    CheckPoint(extreme, 0, 16383, 8191, 0);
    CheckRejected(extreme, 0, 16382);
    Check(extreme.SetSourceSize(1, 16384) && extreme.SetViewportSize(32768, 1), "extreme portrait");
    CheckAspect(extreme, 1, 16384);
    CheckPoint(extreme, 16383, 0, 0, 8191);
    Check(extreme.SetSourceSize(1, 1) && extreme.SetViewportSize(1, 1), "single pixel source and viewport");
    CheckPoint(extreme, 0, 0, 0, 0);
    for (int i = 0; i < 100; ++i) extreme.Zoom(2.0);
    CheckAspect(extreme, 1, 1);
    CheckPoint(extreme, 0, 0, 0, 0);
    for (int i = 0; i < 100; ++i) extreme.Zoom(0.5);
    CheckAspect(extreme, 1, 1);
    CheckPoint(extreme, 0, 0, 0, 0);

    rect = extreme.GetDisplayRect();
    Check(!extreme.SetViewportSize(0, 600), "zero viewport rejected");
    Check(!extreme.SetViewportSize((std::numeric_limits<int>::max)(), 600), "oversized viewport rejected");
    Check(!extreme.SetSourceSize(-1, 1080), "negative source rejected");
    Check(!extreme.SetSourceSize(1920, (std::numeric_limits<int>::max)()), "oversized source rejected");
    Check(!extreme.Zoom(0) && !extreme.Zoom(std::numeric_limits<double>::infinity()) &&
          !extreme.Zoom(std::numeric_limits<double>::quiet_NaN()), "invalid zoom rejected");
    CheckRect(extreme.GetDisplayRect(), rect.left, rect.top, rect.right, rect.bottom, "invalid configuration preserves geometry");
    std::cout << "PASS: aspect-preserving fit/resize/source changes/zoom, black bars, coordinate mapping, extreme sizes\n";
}
