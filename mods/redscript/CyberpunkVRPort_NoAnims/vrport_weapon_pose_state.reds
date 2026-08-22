// Native bridge for the non-VRIK ADS muzzle stabilizer. This file intentionally stays in the
// global namespace because CyberpunkVR_Hands registers a plain global native.

native func SetVRWeaponPoseState(weaponState: Int32, aimInRemaining: Float) -> Int32;
native func SetVRWeaponRaiseTransition(active: Int32) -> Int32;

func VRPortPublishWeaponPoseState(scriptInterface: ref<StateGameScriptInterface>) -> Void {
  if !IsDefined(scriptInterface) || !IsDefined(scriptInterface.localBlackboard) { return; }
  SetVRWeaponPoseState(
    scriptInterface.localBlackboard.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Weapon),
    scriptInterface.localBlackboard.GetFloat(GetAllBlackboardDefs().PlayerStateMachine.AimInTimeRemaining)
  );
}

@wrapMethod(ReadyEvents)
protected final func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  SetVRWeaponRaiseTransition(0);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(SafeEvents)
protected final func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  SetVRWeaponRaiseTransition(0);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(PublicSafeEvents)
protected final func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  SetVRWeaponRaiseTransition(0);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(PublicSafeToReadyEvents)
protected final func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  SetVRWeaponRaiseTransition(1);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(AimingStateEvents)
protected func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(AimingStateEvents)
protected final func OnUpdate(timeDelta: Float, stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(timeDelta, stateContext, scriptInterface);
  VRPortPublishWeaponPoseState(scriptInterface);
}

@wrapMethod(AimingStateEvents)
protected func OnExit(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  VRPortPublishWeaponPoseState(scriptInterface);
}
