using UnrealBuildTool;
using System.Collections.Generic;

public class BPBridgeTestEditorTarget : TargetRules
{
	public BPBridgeTestEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
		ExtraModuleNames.Add("BPBridgeTest");
	}
}
