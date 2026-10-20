#include "GuiSys/CGuiModel.hpp"
#include "port_env.h"

#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiSys.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"

#ifdef TARGET_PC
#include "port_pointer.h"
#include "vr/vr_view.h"
#endif

#include <cstdio>
#include <cstdlib>

CGuiModel* CGuiModel::Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp) {
  CGuiWidgetParms parms = ReadWidgetHeader(frame, in);

  CAssetId model = in.Get< CAssetId >();
  in.Get< uint >();
  uint lightMask = in.Get< uint >();

  CGuiModel* ret = rs_new CGuiModel(parms, sp, model, lightMask, true);
  ret->ParseBaseInfo(frame, in, parms);
  return ret;
}

CGuiModel::CGuiModel(const CGuiWidgetParms& parms, CSimplePool* sp, CAssetId modelId, uint lightMask,
                     bool flag)
: CGuiWidget(parms)
, xc8_modelId(modelId)
, xcc_lightMask(lightMask) {
  const CGuiSys* gs = CGuiSys::GetGlobalGuiSys();
  if (flag && xc8_modelId != kInvalidAssetId && gs->GetUsageMode() != CGuiSys::kUM_Two) {
    xb8_model = sp->GetObj(SObjectTag('CMDL', modelId));
    xb8_model->Lock();
  }
}

CGuiModel::~CGuiModel() {
#ifdef TARGET_PC
  PortVr::VrWristForgetModel(this);
#endif
}

void CGuiModel::Draw(const CGuiWidgetDrawParms& parms) const {
  CGraphics::SetModelMatrix(GetWorldTransform());
  if (!xb8_model) {
    return;
  }
  if (!GetIsFinishedLoading()) {
    return;
  }
  CModel* const model = xb8_model->GetObject();
  if (!model) {
    return;
  }
  if (GetIsVisible()) {
#ifdef TARGET_PC
    // Port: where the model lands on screen, for mouse and touch menus.
    PortGuiHit::Record(this, CGraphics::GetModelMatrix(), model->GetBoundingBox().GetMinPoint(),
                       model->GetBoundingBox().GetMaxPoint());
#endif
#ifdef TARGET_PC
    mPortHudDrawAlpha = parms.GetAlpha();
#endif
    DrawModelPasses(parms, false);
  }

  CGuiWidget::Draw(parms);
}

// Both the regular HUD and the wrist reuse this exact material/colour path.
void CGuiModel::DrawModelPasses(const CGuiWidgetDrawParms& parms, bool overlay) const {
    CModel* const model = xb8_model->GetObject();
    CColor col = GetModifiedColor().WithAlphaModulatedBy(parms.GetAlpha());
    GetParentFrame()->EnableLights(xcc_lightMask);

    const bool cullChanged = GetIsBackfaceCullingOn();
    if (cullChanged) {
      CGraphics::SetCullMode(kCM_Front);
    }

    CModelFlags flags = CModelFlags::Normal();
    bool doDraw = true;
    switch (GetDrawFlags()) {
    case kGMDF_Shadeless:
      flags = CModelFlags::Normal();
      break;
    case kGMDF_Opaque:
      flags = CModelFlags::ColorModulate(col);
      break;
    case kGMDF_Alpha:
      flags = CModelFlags::AlphaBlended(col).DepthCompareUpdate((!overlay && GetIsAlwaysDepthRead()),
                                                              (!overlay && GetIsAlwaysDepthWrite()));
      break;
    case kGMDF_Additive:
      flags = CModelFlags::Additive(col).DepthCompareUpdate((!overlay && GetIsAlwaysDepthRead()),
                                                          (!overlay && GetIsAlwaysDepthWrite()));
      break;
    case kGMDF_AlphaAdditiveOverdraw:
      flags = CModelFlags::AlphaBlended(col).DepthCompareUpdate((!overlay && GetIsAlwaysDepthRead()), false);
      if (GetIsDepthBackwards()) {
        flags = flags.DepthBackwards();
      }
      model->Draw(overlay ? flags.DepthCompareUpdate(false, false) : flags);
      flags = CModelFlags::AdditiveRGB(col).DepthCompareUpdate((!overlay && GetIsAlwaysDepthRead()),
             (!overlay && GetIsAlwaysDepthWrite()));
      if (GetIsDepthBackwards()) {
        flags = flags.DepthBackwards();
      }
      model->Draw(overlay ? flags.DepthCompareUpdate(false, false) : flags);
      doDraw = false;
      break;
    default:
      doDraw = false;
      break;
    }

    if (doDraw) {
      if (GetIsDepthBackwards()) {
        flags = flags.DepthBackwards();
      }
      model->Draw(overlay ? flags.DepthCompareUpdate(false, false) : flags);
    }

    if (cullChanged) {
      CGraphics::SetCullMode(kCM_None);
    }
    GetParentFrame()->DisableLights();
}
#ifdef TARGET_PC
void CGuiModel::DrawWrist(const CTransform4f& transform, float alpha) const {
  if (!xb8_model.valid() || !GetIsFinishedLoading() || !xb8_model->GetObject()) return;
  CGraphics::SetModelMatrix(transform);
  DrawModelPasses(CGuiWidgetDrawParms(mPortHudDrawAlpha * alpha), true);
}
#endif

void CGuiModel::Touch() const {
  if (xb8_model && xb8_model->GetObject()) {
    xb8_model->GetObject()->Touch(0);
  }
}

bool CGuiModel::GetIsFinishedLoadingWidgetSpecific() const {
  if (!xb8_model.valid()) {
    return true;
  }
  xb8_model->TryCache();

  if (const CModel* model = xb8_model->GetObject()) {
    model->Touch(0);
    const bool loaded = model->IsLoaded(0);
    if (!loaded && port::EnvFlag("MP_LOG_HUD")) {
      static int sLogCount = 0;
      if (sLogCount < 80) {
        ++sLogCount;
        std::fprintf(stderr, "[hud] model %08x not loaded\n", xc8_modelId);
      }
    }
    return loaded;
  }

  if (port::EnvFlag("MP_LOG_HUD")) {
    static int sLogCount = 0;
    if (sLogCount < 80) {
      ++sLogCount;
      std::fprintf(stderr, "[hud] model %08x has no object\n", xc8_modelId);
    }
  }
  return false;
}

rstl::vector< CAssetId > CGuiModel::GetModelAssets() const {
  return rstl::vector< CAssetId >(1, xc8_modelId);
}

FourCC CGuiModel::GetWidgetTypeID() const { return 'MODL'; }
