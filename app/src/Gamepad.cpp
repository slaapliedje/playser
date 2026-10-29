#include "Gamepad.h"

#include <SDL3/SDL.h>

namespace {

constexpr int kRepeatDelayMs = 400;
constexpr int kRepeatRateMs = 110;
constexpr int kStickThreshold = 16000;

int keyForButton(int button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return Qt::Key_Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Qt::Key_Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Qt::Key_Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Qt::Key_Right;
    case SDL_GAMEPAD_BUTTON_SOUTH: return Qt::Key_Return;
    case SDL_GAMEPAD_BUTTON_EAST: return Qt::Key_Escape;
    case SDL_GAMEPAD_BUTTON_NORTH: return Qt::Key_I;
    case SDL_GAMEPAD_BUTTON_START: return Qt::Key_Menu;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Qt::Key_PageUp;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Qt::Key_PageDown;
    default: return 0;
    }
}

// Steam Input's virtual pads report the real controller's type
QString styleFor(SDL_Gamepad *pad)
{
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return QStringLiteral("playstation");
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return QStringLiteral("nintendo");
    default:
        return QStringLiteral("xbox");
    }
}

bool repeats(int key)
{
    return key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right;
}

} // namespace

void Gamepad::setStyle(const QString &style)
{
    QMetaObject::invokeMethod(this, [this, style] {
        if (style == m_style)
            return;
        m_style = style;
        emit styleChanged();
    });
}

void Gamepad::run()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        qWarning("gamepad: SDL_Init failed: %s", SDL_GetError());
        return;
    }

    int heldKey = 0;           // direction currently held (button or stick)
    Uint64 nextRepeat = 0;
    int stickKey = 0;

    auto press = [&](int key) {
        emit keyPressed(key);
        if (repeats(key)) {
            heldKey = key;
            nextRepeat = SDL_GetTicks() + kRepeatDelayMs;
        }
    };

    while (!isInterruptionRequested()) {
        SDL_Event e;
        const int timeout = heldKey ? 20 : 100;
        while (SDL_WaitEventTimeout(&e, timeout)) {
            switch (e.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which)) {
                    qInfo("gamepad: connected %s (%s)", SDL_GetGamepadName(pad),
                          SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)));
                    setStyle(styleFor(pad));
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gdevice.which))
                    SDL_CloseGamepad(pad);
                heldKey = stickKey = 0;
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gbutton.which))
                    setStyle(styleFor(pad));
                if (int key = keyForButton(e.gbutton.button))
                    press(key);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (keyForButton(e.gbutton.button) == heldKey)
                    heldKey = 0;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY)
                    break;
                SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gaxis.which);
                if (!pad)
                    break;
                const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
                const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
                int key = 0;
                if (qAbs(x) > kStickThreshold || qAbs(y) > kStickThreshold)
                    key = qAbs(x) > qAbs(y) ? (x > 0 ? Qt::Key_Right : Qt::Key_Left)
                                            : (y > 0 ? Qt::Key_Down : Qt::Key_Up);
                if (key != stickKey) {
                    if (stickKey == heldKey)
                        heldKey = 0;
                    stickKey = key;
                    if (key)
                        press(key);
                }
                break;
            }
            default:
                break;
            }
            if (isInterruptionRequested())
                break;
        }
        if (heldKey && SDL_GetTicks() >= nextRepeat) {
            emit keyPressed(heldKey);
            nextRepeat = SDL_GetTicks() + kRepeatRateMs;
        }
    }
    SDL_Quit();
}
