// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UeAgentInterface : ModuleRules
{
	public UeAgentInterface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

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
				"Settings",
				"EngineSettings",
				"HTTPServer",
				"HTTP",
				"Sockets",
				"Json",
				"JsonUtilities",
				"ImageCore",
				"AIModule",
				"GameplayTasks",
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
				"StateTreeModule",
				"StateTreeEditorModule",
				"GameplayStateTreeModule",
				"SmartObjectsModule",
				"WorldConditions",
				"PropertyBindingUtils",
				"PhysicsCore",
				"MaterialEditor",
				"LevelSequence",
				"MovieScene",
				"MovieSceneTools",
				"MovieSceneTracks",
				"SequencerScriptingEditor",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"IKRig",
				"IKRigEditor",
				"RigVM",
				"RigVMDeveloper",
				"Niagara",
				"NiagaraEditor",
				"GeometryCache",
				"ComputeFramework",
				"OptimusCore",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"GeometryCore",
				"MeshDescription",
				"StaticMeshDescription",
				"MediaAssets",
				"Paper2D",
				"TextureGraph",
				"TextureGraphEngine",
				"AudioEditor",
				"AudioMixer",
				"MetasoundEngine",
				"MetasoundEditor",
				"MetasoundFrontend",
				"DataLayerEditor",
				"WorldPartitionEditor",
				"TargetPlatform",
			}
		);
	}
}
