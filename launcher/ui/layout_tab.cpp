// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's Layout tab: the controller bindings picture. The port's
// PrimedGun control scheme (platform/vr/vr_pad.cpp) uses the same bindings.

#include "tabs.h"

#include "tab_context.h"
#include "widgets.h"
#include "settings_model.h"
#include <QLabel>

#include <QPixmap>
#include <QTabWidget>
#include <QVBoxLayout>

namespace PrimedGunLauncher {

void BuildLayoutTab(TabContext& ctx, QTabWidget* tabs) {
  auto* page = new QWidget(tabs);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(14, 10, 14, 10);
  layout->addWidget(ctx.Section(QObject::tr("Controller Layout")));
  auto* customNotice = new QLabel(QObject::tr("Custom layout is active. View or edit your bindings in Controller > Customize controller layout. The picture below shows the standard layout."));
  customNotice->setWordWrap(true);
  customNotice->setObjectName("PrimedGunNotice");
  layout->addWidget(customNotice);
  ctx.AddRefresher([customNotice, &ctx] { customNotice->setVisible(ctx.model.Bool("vr_custom_layout_enabled")); });
  customNotice->setVisible(ctx.model.Bool("vr_custom_layout_enabled"));
  auto* picture = new ScaledImageLabel(page);
  picture->SetSourcePixmap(QPixmap(QStringLiteral(":/controller_layout.png")));
  layout->addWidget(picture, 1);
  tabs->addTab(page, QObject::tr("Layout"));
}

} // namespace PrimedGunLauncher
