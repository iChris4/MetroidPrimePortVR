#include "GuiSys/CAuiEnergyBarT01.hpp"
#include "port_env.h"
#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiSys.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/IObjectStore.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "rstl/math.hpp"
#include "rstl/pair.hpp"

#include "port_hud_bars.h"

#include <cstdio>
#include <cstdlib>

static bool HudLogEnabled() {
  static const bool enabled = port::EnvFlag("MP_LOG_HUD");
  return enabled;
}

CGuiWidget* CAuiEnergyBarT01::Create(CGuiFrame* frame, CInputStream& in, IObjectStore* sp) {
  CGuiWidgetParms parms = ReadWidgetHeader(frame, in);
  CAssetId tex = in.Get< CAssetId >();

  CAuiEnergyBarT01* ret = rs_new CAuiEnergyBarT01(parms, sp, tex);
  ret->ParseBaseInfo(frame, in, parms);
  return ret;
}

CAuiEnergyBarT01::CAuiEnergyBarT01(const CGuiWidgetParms& parms, IObjectStore* sp,
                                   CAssetId textureId)
: CGuiWidget(parms)
, mTextureId(textureId)
, mEmptyColor(CColor::White())
, mFilledColor(CColor::White())
, mShadowColor(CColor::White())
, mCoordFunc(nullptr)
, mTesselation(1.f)
, mMaxEnergy(0.f)
, mFilledSpeed(1000.f)
, mShadowSpeed(1000.f)
, mShadowDrainDelay(0.f)
, mAlwaysResetDelayTimer(false)
, mWrapping(false)
, mSetEnergy(0.f)
, mFilledEnergy(0.f)
, mShadowEnergy(0.f)
, mShadowDrainDelayTimer(0.f)
, mPortBar(nullptr)
, mPortBarLooked(false) {
  if (CGuiSys::GetGlobalGuiSys()->GetUsageMode() != CGuiSys::kUM_Two) {
    mTexture = sp->GetObj(SObjectTag('TXTR', mTextureId));
    mTexture->Lock();
  }
}

CAuiEnergyBarT01::~CAuiEnergyBarT01() {}

void CAuiEnergyBarT01::SetMaxEnergy(const float maxEnergy) {
  mMaxEnergy = maxEnergy;
  mSetEnergy = rstl::min_val(mSetEnergy, mMaxEnergy);
  mFilledEnergy = rstl::min_val(mFilledEnergy, mMaxEnergy);
  mShadowEnergy = rstl::min_val(mShadowEnergy, mMaxEnergy);
}

void CAuiEnergyBarT01::SetCurrEnergy(const float energy, const ESetMode mode) {
  float e = CMath::Clamp(0.f, energy, mMaxEnergy);

  if (e == mSetEnergy) {
    return;
  }

  if (mAlwaysResetDelayTimer || mFilledEnergy == mShadowEnergy) {
    mShadowDrainDelayTimer = mShadowDrainDelay;
  }

  mWrapping = mode == kSM_Wrapped;
  mSetEnergy = e;
  if (mode == kSM_Instant) {
    mFilledEnergy = mSetEnergy;
  }
}

void CAuiEnergyBarT01::Update(const float dt) {

  if (mShadowDrainDelayTimer > 0.f) {
    mShadowDrainDelayTimer = rstl::max_val(mShadowDrainDelayTimer - dt, 0.f);
  }

  if (mFilledEnergy < mSetEnergy) {
    if (mWrapping) {
      mFilledEnergy -= dt * mFilledSpeed;
      if (mFilledEnergy < 0.f) {
        mFilledEnergy = rstl::max_val(mSetEnergy, mFilledEnergy + mMaxEnergy);
        mWrapping = false;
        mShadowEnergy = mMaxEnergy;
      }
    } else {
      mFilledEnergy = rstl::min_val(mSetEnergy, mFilledEnergy + dt * mFilledSpeed);
    }
  } else if (mFilledEnergy > mSetEnergy) {
    if (mWrapping) {
      mFilledEnergy += dt * mFilledSpeed;
      if (mFilledEnergy > mMaxEnergy) {
        mFilledEnergy = rstl::min_val(mSetEnergy, mFilledEnergy - mMaxEnergy);
        mWrapping = false;
        mShadowEnergy = mFilledEnergy;
      }
    } else {
      mFilledEnergy = rstl::max_val(mSetEnergy, mFilledEnergy - dt * mFilledSpeed);
    }
  }

  if (mShadowEnergy < mFilledEnergy) {
    mShadowEnergy = mFilledEnergy;
  } else if (mShadowEnergy > mFilledEnergy && mShadowDrainDelayTimer == 0.f) {
    mShadowEnergy = rstl::max_val(mFilledEnergy, mShadowEnergy - dt * mShadowSpeed);
  }

  if (mTexture) {
    mTexture->TryCache();
  }
  CGuiWidget::Update(dt);
}

rstl::pair< CVector3f, CVector3f > CAuiEnergyBarT01::DownloadBarCoordFunc(float t) {
  const float x = 12.5f * t - 6.25f;

  return rstl::pair< CVector3f, CVector3f >(CVector3f(x, 0.f, -0.2f), CVector3f(x, 0.f, 0.2f));
}

const PortHudBars::Bar* CAuiEnergyBarT01::PortBar() const {
  if (mPortBarLooked) {
    return mPortBar;
  }
  mPortBarLooked = true;
  const CGuiFrame* frame = GetParentFrame();
  if (frame == nullptr) {
    return nullptr;
  }
  const std::shared_ptr< const PortHudBars::Bars > bars = PortHudBars::ForFrame(frame->GetId());
  if (!bars) {
    return nullptr;
  }
  for (const PortHudBars::Bar& bar : *bars) {
    const rstl::string name(bar.name.c_str());
    if (GetParentFrame()->WidgetIdDB().FindWidgetID(name) == GetWidgetID()) {
      mPortBars = bars;
      mPortBar = &bar;
      break;
    }
  }
  return mPortBar;
}

void CAuiEnergyBarT01::PortDrawBar(const PortHudBars::Bar& bar, const float from,
                                   const float to, const CColor& color) const {
  // CGraphics's stream holds 240 vertices and, as the game laid its buffers out, positions run
  // into the texture coordinates after 160. A mod's strip is longer than that, so it is drawn as
  // several short strips, each starting on the station the last one ended on.
  static const int skStationsPerStrip = 48;
  int inStrip = 0;
  const auto put = [](const PortHudBars::Station& station) {
    CGraphics::StreamTexcoord(station.uvA[0], station.uvA[1]);
    CGraphics::StreamVertex(CVector3f(station.a[0], station.a[1], station.a[2]));
    CGraphics::StreamTexcoord(station.uvB[0], station.uvB[1]);
    CGraphics::StreamVertex(CVector3f(station.b[0], station.b[1], station.b[2]));
  };
  PortHudBars::Station prev = PortHudBars::Station();
  const auto emit = [&](const PortHudBars::Station& station) {
    if (inStrip == skStationsPerStrip) {
      CGraphics::StreamEnd();
      CGraphics::StreamBegin(kP_TriangleStrip);
      CGraphics::StreamColor(color);
      put(prev);
      inStrip = 1;
    }
    put(station);
    prev = station;
    ++inStrip;
  };
  size_t first = 0;
  size_t last = 0;
  PortHudBars::Inside(bar, from, to, first, last);
  CGraphics::StreamBegin(kP_TriangleStrip);
  CGraphics::StreamColor(color);
  emit(PortHudBars::Sample(bar, from));
  for (size_t i = first; i < last; ++i) {
    emit(bar.stations[i]);
  }
  emit(PortHudBars::Sample(bar, to));
  CGraphics::StreamEnd();
}

void CAuiEnergyBarT01::Draw(const CGuiWidgetDrawParms& parms) const {
  static bool sMissing = false;
  const auto logMissing = [](const char* reason) {
    if (HudLogEnabled() && !sMissing) {
      sMissing = true;
      std::fprintf(stderr, "[hud] energy bar hidden: %s\n", reason);
    }
  };
  CGraphics::SetModelMatrix(GetWorldTransform());
  if (!mTexture) {
    logMissing("no texture token");
    return;
  }
  const PortHudBars::Bar* portBar = PortBar();
  if (!mTexture->IsLoaded() || (!mCoordFunc && !portBar)) {
    logMissing(mCoordFunc || portBar ? "texture not loaded" : "no coord func");
    return;
  };
  if (!mTexture->GetObject()) {
    logMissing("no texture object");
    return;
  }
  if (HudLogEnabled() && sMissing) {
    sMissing = false;
    std::fprintf(stderr, "[hud] energy bar visible again (alpha=%.2f)\n", parms.GetAlpha());
  }
  CTexture* tex = mTexture->GetObject();
  CGraphics::SetDepthWriteMode(true, kE_LEqual, false);

  CGraphics::SetAmbientColor(CColor::White());
  // A mod's bar is drawn the way its frame asks; the game's own is always additive.
  const bool portAlpha = portBar && GetDrawFlags() == kGMDF_Alpha;
  CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, portAlpha ? kBF_InvSrcAlpha : kBF_One, kLO_Clear);

  const float dVar9 = mMaxEnergy > 0.f ? mFilledEnergy / mMaxEnergy : 0.f;
  const float dVar8 = mMaxEnergy > 0.f ? mShadowEnergy / mMaxEnergy : 0.f;
  const CColor& color = GetModifiedColor();
  CColor filledColor = CColor::Modulate(color, mFilledColor.WithAlphaModulatedBy(parms.GetAlpha()));
  CColor shadowColor = CColor::Modulate(color, mShadowColor.WithAlphaModulatedBy(parms.GetAlpha()));
  CColor emptyColor = CColor::Modulate(color, mEmptyColor.WithAlphaModulatedBy(parms.GetAlpha()));

  for (int i = 0; i < 3; ++i) {
    float dVar6 = i == 0 ? 0.f : i == 1 ? dVar9 : dVar8;
    const float dVar7 = i == 0 ? dVar9 : i == 1 ? dVar8 : 1.f;
    const CColor& useColor = i == 0 ? filledColor : i == 1 ? shadowColor : emptyColor;

    if (dVar6 == dVar7) {
      continue;
    }

    CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
    CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
    tex->Load(GX_TEXMAP0, CTexture::kCM_Repeat);
    if (portBar) {
      PortDrawBar(*portBar, dVar6, dVar7, useColor);
      continue;
    }
    CGraphics::StreamBegin(kP_TriangleStrip);
    CGraphics::StreamColor(useColor);
    rstl::pair< CVector3f, CVector3f > coord = mCoordFunc(dVar6);
    while (dVar6 < dVar7) {
      CGraphics::StreamTexcoord(dVar6, 0.f);
      CGraphics::StreamVertex(coord.first);
      CGraphics::StreamTexcoord(dVar6, 1.f);
      CGraphics::StreamVertex(coord.second);
      dVar6 += mTesselation;

      if (dVar6 >= dVar7) {
        coord = mCoordFunc(dVar7);
        CGraphics::StreamTexcoord(dVar7, 0.f);
        CGraphics::StreamVertex(coord.first);
        CGraphics::StreamTexcoord(dVar7, 1.f);
        CGraphics::StreamVertex(coord.second);
      } else {
        coord = mCoordFunc(dVar6);
      }
    }
    CGraphics::StreamEnd();
  }
  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
}

float CAuiEnergyBarT01::GetActualFraction() const {

  return mMaxEnergy == 0.f ? 0.f : mSetEnergy / mMaxEnergy;
}

FourCC CAuiEnergyBarT01::GetWidgetTypeID() const { return 'ENRG'; }
static const char* hack_string() { return "TextureId"; }
