// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UeAgentInterface : ModuleRules
{
	public UeAgentInterface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateIncludePaths.AddRange(
			new string[]
			{
				System.IO.Path.Combine(EngineDirectory, "Source/Editor/MaterialEditor/Private"),
			}
		);

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
				"MainFrame",
				"GraphEditor",
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
				"RHI",
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
				"AnimationBlueprintEditor",
				"AnimGraph",
				"AdvancedPreviewScene",
				"AssetTools",
				"Persona",
				"StaticMeshEditor",
				"SkeletalMeshEditor",
				"SkeletonEditor",
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
				"PhysicsAssetEditor",
				"HairStrandsCore",
				"HairStrandsEditor",
				"MaterialEditor",
				"LevelSequence",
				"LevelSequenceEditor",
				"MovieScene",
				"MovieSceneTools",
				"MovieSceneTracks",
				"Sequencer",
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
				"PCG",
				"PCGEditor",
				"GeometryCache",
				"ComputeFramework",
				"OptimusCore",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"GeometryFramework",
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
