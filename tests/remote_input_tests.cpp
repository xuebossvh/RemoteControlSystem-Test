#include "../RemoteCtrolSystem/Common/RemoteInput.h"
#include <iostream>
#include <stdexcept>
namespace { void Check(bool value, const char* label) { if (!value) throw std::runtime_error(label); } }
int main() {
    try {
        Check(RemoteInput::NormalizePrimaryCoordinate(0, 2560) == 0, "first physical pixel");
        Check(RemoteInput::NormalizePrimaryCoordinate(2559, 2560) == 65535, "last physical pixel");
        Check(RemoteInput::NormalizePrimaryCoordinate(123, 1) == 0, "single-pixel extent");
        Check(RemoteInput::NormalizePrimaryCoordinate(-10, 2560) == 0, "negative coordinate clamp");
        LONG previous = -1;
        for (int pixel = 0; pixel < 16384; ++pixel) {
            LONG value = RemoteInput::NormalizePrimaryCoordinate(pixel, 16384);
            Check(value >= previous && value >= 0 && value <= 65535, "monotonic bounded normalization");
            previous = value;
        }
        INPUT move = {};
        Check(RemoteInput::MakePrimaryMouseInput(0, 0, 0, move), "mouse event creation");
        Check(move.type == INPUT_MOUSE && move.mi.dx == 0 && move.mi.dy == 0, "physical origin");
        Check((move.mi.dwFlags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE)) == (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE), "absolute movement flags");
        Check(RemoteInput::IsInjectedExtraInfo(move.mi.dwExtraInfo), "movement injection marker");
        Check(!RemoteInput::MakePrimaryMouseInput(-1, 0, 0, move), "invalid movement rejected");
        INPUT up = RemoteInput::MakeKeyboardInput(VK_CONTROL, true, true);
        Check((up.ki.dwFlags & (KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY)) == (KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY), "extended key-up flags preserved");
        Check(RemoteInput::IsInjectedExtraInfo(up.ki.dwExtraInfo), "keyboard injection marker");
        Check(!RemoteInput::IsInjectedExtraInfo(0), "ordinary input is not ours");
        std::cout << "Remote input coordinate and marker tests passed (no input injected).\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
