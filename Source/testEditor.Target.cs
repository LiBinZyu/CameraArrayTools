// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class testEditorTarget : TargetRules
{
	public testEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V4;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_3;
		ExtraModuleNames.Add("test");
	}
}