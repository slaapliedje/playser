#pragma once

#include <QThread>

// Polls gamepads with SDL3 on its own thread and turns them into key presses
// (see main.cpp), so QML's ordinary keyboard focus navigation works with a
// controller. Buttons are mapped by SDL3 gamepad button, never by controller
// name: Steam Input's virtual pads vary by device.
//
//   D-pad / left stick   arrows (with repeat)
//   A (south)            Return      B (east)   Escape
//   Y (north)            I (tracks)  Start      Menu
//   LB / RB              PageUp / PageDown (chapters)
class Gamepad : public QThread
{
    Q_OBJECT
public:
    using QThread::QThread;

signals:
    void keyPressed(int qtKey);

protected:
    void run() override;
};
