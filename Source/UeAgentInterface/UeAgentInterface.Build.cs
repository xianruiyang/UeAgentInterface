// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UeAgentInterface : ModuleRules
{
	public UeAgentInterface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"LevelEditor",
				"ApplicationCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"Projects",
				"HTTPServer",
				"HTTP",
				"Sockets",
				"Json",
				"JsonUtilities",
				"RenderCore",
				"Landscape",
				"Foliage",
				"NavigationSystem",
				"AssetRegistry",
				"Kismet",
				"KismetCompiler",
				"BlueprintEditorLibrary",
				"BlueprintGraph",
				"AnimationBlueprintLibrary",
				"AnimationEditor",
				"AnimGraph",
				"AssetTools",
				"Persona",
				"StaticMeshEditor",
				"UMG",
				"UMGEditor",
				"EnhancedInput",
				"InputBlueprintNodes",
				"InputCore",
				"GameplayTags",
				"PhysicsCore",
				"MaterialEditor",
				"LevelSequence",
				"MovieScene",
				"MovieSceneTracks",
				"IKRig",
				"IKRigEditor",
				"Niagara",
				"NiagaraEditor",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"GeometryCore",
				"MeshDescription",
				"StaticMeshDescription",
			}
		);
	}
}
