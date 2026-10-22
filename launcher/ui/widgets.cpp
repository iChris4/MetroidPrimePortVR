// SPDX-License-Identifier: GPL-3.0-or-later

#include "widgets.h"

#include <QComboBox>
#include <QAbstractSpinBox>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QWheelEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

#include <cmath>

namespace PrimedGunLauncher {

WheelBlocker* WheelBlocker::Instance() {
  static WheelBlocker* blocker = new WheelBlocker;
  return blocker;
}

bool WheelBlocker::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Wheel &&
      (qobject_cast<QSlider*>(watched) != nullptr ||
       qobject_cast<QAbstractSpinBox*>(watched) != nullptr ||
       qobject_cast<QComboBox*>(watched) != nullptr)) {
    // Forward directly to the containing page, including when a control has focus.
    // Ignoring alone is not sufficient on every platform or editable combo box.
    auto* widget = qobject_cast<QWidget*>(watched);
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
      if (auto* scroll = qobject_cast<QAbstractScrollArea*>(parent)) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        QWheelEvent forwarded(scroll->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),
                              wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                              wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
                              wheel->source());
        QApplication::sendEvent(scroll->viewport(), &forwarded);
        event->accept();
        return true;
      }
    }
    event->ignore();
    return true;
  }
  return QObject::eventFilter(watched, event);
}

FloatRow::FloatRow(const QString& label, double min, double max, double step,
                   std::function<void(float)> onChange, QWidget* tag, QWidget* parent)
    : QWidget(parent), m_step(step) {
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(8);
  auto* text = new QLabel(label, this);
  text->setMinimumWidth(170);
  text->setMaximumWidth(170);
  m_slider = new QSlider(Qt::Horizontal, this);
  m_spin = new QDoubleSpinBox(this);
  m_spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
  m_slider->installEventFilter(WheelBlocker::Instance());
  m_spin->installEventFilter(WheelBlocker::Instance());
  m_spin->setRange(min, max);
  m_spin->setSingleStep(step);
  m_spin->setDecimals(step < 0.1 ? 3 : 2);
  m_spin->setMinimumWidth(76);
  m_spin->setMaximumWidth(76);
  m_slider->setRange(static_cast<int>(std::lround(min / step)),
                     static_cast<int>(std::lround(max / step)));
  row->addWidget(text, 0);
  row->addWidget(m_slider, 1);
  row->addWidget(m_spin, 0);
  auto* minus = new QPushButton(QStringLiteral("-"), this);
  auto* plus = new QPushButton(QStringLiteral("+"), this);
  minus->setFixedWidth(28);
  plus->setFixedWidth(28);
  row->addWidget(minus);
  row->addWidget(plus);
  if (tag != nullptr) {
    row->addWidget(tag);
  }

  connect(m_slider, &QSlider::valueChanged, this, [this, onChange](int v) {
    const double value = v * m_step;
    if (m_spin->value() != value) {
      const QSignalBlocker blocker(m_spin);
      m_spin->setValue(value);
    }
    onChange(static_cast<float>(m_spin->value()));
  });
  connect(m_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, onChange](double v) {
    const int sliderValue = static_cast<int>(std::lround(v / m_step));
    if (m_slider->value() != sliderValue) {
      const QSignalBlocker blocker(m_slider);
      m_slider->setValue(sliderValue);
    }
    onChange(static_cast<float>(v));
  });
  connect(minus, &QPushButton::clicked, this, [this] { m_spin->setValue(m_spin->value() - m_step); });
  connect(plus, &QPushButton::clicked, this, [this] { m_spin->setValue(m_spin->value() + m_step); });
}

void FloatRow::SetValue(double value) {
  const QSignalBlocker spinBlocker(m_spin);
  const QSignalBlocker sliderBlocker(m_slider);
  m_spin->setValue(value);
  m_slider->setValue(static_cast<int>(std::lround(value / m_step)));
}

ScaledImageLabel::ScaledImageLabel(QWidget* parent) : QLabel(parent) {
  setAlignment(Qt::AlignCenter);
  setMinimumSize(240, 160);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ScaledImageLabel::SetSourcePixmap(const QPixmap& pixmap) {
  m_source = pixmap;
  UpdateScaledPixmap();
}

void ScaledImageLabel::resizeEvent(QResizeEvent* event) {
  QLabel::resizeEvent(event);
  UpdateScaledPixmap();
}

void ScaledImageLabel::UpdateScaledPixmap() {
  if (m_source.isNull() || width() <= 0 || height() <= 0) {
    return;
  }
  setPixmap(m_source.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace PrimedGunLauncher
