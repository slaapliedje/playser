#pragma once

#include <QThread>
#include <QtQml/qqmlregistration.h>

// Polls gamepads with SDL3 on its own thread and turns them into key presses
// (see main.cpp), so QML's ordinary keyboard focus navigation works with a
// controller. Buttons are mapped by SDL3 gamepad button, never by controller
// name: Steam Input's virtual pads vary by device.
//
//   D-pad / left stick   arrows (with repeat)
//   A (south)            Return      B (east)   Escape
//   Y (north)            I (tracks)  Start      Menu
//   LB / RB              PageUp / PageDown (chapters)
//
// style follows the pad last used, so the UI can draw its buttons.
class Gamepad : public QThread
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")
    // "xbox", "playstation" or "nintendo" (button labels and shapes)
    Q_PROPERTY(QString style READ style NOTIFY styleChanged)

public:
    using QThread::QThread;

    QString style() const { return m_style; }

signals:
    void keyPressed(int qtKey);
    void styleChanged();

protected:
    void run() override;

private:
    void setStyle(const QString &style); // from the SDL thread

    QString m_style = QStringLiteral("xbox");
};
