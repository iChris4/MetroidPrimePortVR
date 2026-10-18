#ifndef _CSCRIPTDOOR
#define _CSCRIPTDOOR

#include "MetroidPrime/CPhysicsActor.hpp"

class CScriptDoor : public CPhysicsActor {
public:
  rstl::optional_object< CAABox > GetTouchBounds() const override;
  CVector3f GetOrbitPosition(const CStateManager& mgr) const override;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId other, CStateManager& mgr) override;
  void Think(float dt, CStateManager& mgr) override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  DECLARE_TYPES_MATCH_OR_ACCEPT;

  enum EDoorAnimType {
    kDAT_Open,
    kDAT_Close,
    kDAT_Ready,
  };

  enum EDoorOpenCondition {
    kDOC_NotReady,
    kDOC_Loading,
    kDOC_Ready,
  };

  CScriptDoor(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
              const CTransform4f& xf, const CModelData& modelData,
              const CActorParameters& actorParameters, const CVector3f& orbitPosition,
              const CAABox& bounds, const bool active, const bool open,
              const bool projectilesCollide, float animationLength, const bool ballDoor);

  void SetDoorAnimation(EDoorAnimType state);

  rstl::optional_object< CAABox > GetProjectileBounds() const;

  EDoorOpenCondition GetDoorOpenCondition(CStateManager& mgr);
  void OpenDoor(TUniqueId uid, CStateManager& mgr);

  const TUniqueId GetConnectedDockID() const { return mDockId; }
  bool IsOpen() const { return mIsOpen; }
#ifdef TARGET_PC
  // The door state a save state carries, one field per close path the door can be
  // captured in the middle of (see PortSaveState::DoorSaveState). `restore` puts
  // the door back in it without sending any script message: what a live open or
  // close sent is already in the saved game state, and Think sends whatever the
  // restored state still owes from its own branch.
  struct PortDoorFlags {
    bool open = false;
    bool normalClosePending = false;   // !mIsOpen && mWasOpen
    bool refusedClosePending = false;  // mClosing
    bool doClose = false;              // mDoClose
  };
  void PortRestoreDoorState(const PortDoorFlags& state);
  // Open or closed with no close in flight: the original single-flag restore.
  void PortRestoreOpen(bool open) { PortRestoreDoorState(PortDoorFlags{open, false, false, false}); }
  // True while the door reports closed but still owes Closed from its close
  // animation (Think's animation-end branch clears mWasOpen as it sends them).
  bool PortClosingPending() const { return !mIsOpen && mWasOpen; }
  // True while a refused opening still owes its Closed (Think's mClosing branch,
  // which skips the dock decrement).
  bool PortRefusedClosePending() const { return mClosing; }
  // True while another door's close blocks this one from opening again.
  bool PortDoClosePending() const { return mDoClose; }
#endif
  bool IsBallDoor() const { return mBallDoor; }
  void SetDoClose(const bool close) { mDoClose = close; }

  bool IsConnectedToArea(const CStateManager& mgr, TAreaId area) const;
  void ForceClosed(CStateManager& mgr);

private:
  float mAnimLength;
  float mAnimTime;
  EDoorAnimType mDoorState;
  CAABox x264_;
  TUniqueId mPartner1;
  TUniqueId mPartner2;
  TUniqueId mPrevDoor;
  TUniqueId mDockId;
  CAABox mModelBounds;
  CVector3f mOrbitPos;

  bool mClosing : 1;
  bool mWasOpen : 1;
  bool mIsOpen : 1;
  bool mConditionsMet : 1;
  bool mProjectilesCollide : 1;
  bool mBallDoor : 1;
  bool mDoClose : 1;
};
CHECK_SIZEOF(CScriptDoor, (VERSION >= VERSION_GM8P_00 ? 0x2c0 : 0x2b0))

#endif // _CSCRIPTDOOR
