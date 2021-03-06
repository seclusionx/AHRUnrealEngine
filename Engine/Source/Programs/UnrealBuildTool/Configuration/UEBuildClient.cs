// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace UnrealBuildTool
{
	[Serializable]
    public class UEBuildClient : UEBuildTarget
	{
		public UEBuildClient(
			string InGameName, 
			UnrealTargetPlatform InPlatform, 
			UnrealTargetConfiguration InConfiguration,
			TargetRules InRulesObject,
			List<string> InAdditionalDefinitions, 
			string InRemoteRoot, 
			List<OnlyModule> InOnlyModules,
			bool bInEditorRecompile)
			// NOTE: If we're building a monolithic binary, then the game and engine code are linked together into one
			//       program executable, so we want the application name to be the game name.  In the case of a modular
			//       binary, we use 'UnrealEngine' for our application name
            : base(
                InAppName: UEBuildTarget.GetBinaryBaseName(InGameName, InRulesObject, InPlatform, InConfiguration, "Client"),
                InGameName: InGameName,
                InPlatform: InPlatform,
                InConfiguration: InConfiguration,
                InRulesObject: InRulesObject,
                InAdditionalDefinitions: InAdditionalDefinitions,
                InRemoteRoot: InRemoteRoot,
                InOnlyModules: InOnlyModules,
				bInEditorRecompile: bInEditorRecompile
			)
        {
            if (ShouldCompileMonolithic())
            {
                if ((UnrealBuildTool.IsDesktopPlatform(Platform) == false) ||
                    (Platform == UnrealTargetPlatform.WinRT) ||
                    (Platform == UnrealTargetPlatform.WinRT_ARM))
                {
                    // We are compiling for a console...
                    // We want the output to go into the <GAME>\Binaries folder
                    if (InRulesObject.bOutputToEngineBinaries == false)
                    {
						for (int Index = 0; Index < OutputPaths.Length; Index++ )
						{
							OutputPaths[Index] = OutputPaths[Index].Replace("Engine\\Binaries", InGameName + "\\Binaries");
						}
                    }
                }
            }
        }

        protected override void SetupBinaries()
        {
            base.SetupBinaries();

            {
                // Make the game executable.
                UEBuildBinaryConfiguration Config = new UEBuildBinaryConfiguration(InType: UEBuildBinaryType.Executable,
                                                                                    InOutputFilePaths: OutputPaths,
																					InIntermediateDirectory: EngineIntermediateDirectory,
                                                                                    bInCreateImportLibrarySeparately: (ShouldCompileMonolithic() ? false : true),
                                                                                    bInAllowExports: !ShouldCompileMonolithic(),
                                                                                    InModuleNames: new List<string>() { "Launch" });

				AppBinaries.Add(new UEBuildBinaryCPP(this, Config));
            }

            // Add the other modules that we want to compile along with the executable.  These aren't necessarily
            // dependencies to any of the other modules we're building, so we need to opt in to compile them.
            {
                // Modules should properly identify the 'extra modules' they need now.
                // There should be nothing here!
            }

            // Allow the platform to setup binaries
            UEBuildPlatform.GetBuildPlatform(Platform).SetupBinaries(this);
        }

        public override void SetupDefaultGlobalEnvironment(
            TargetInfo Target,
            ref LinkEnvironmentConfiguration OutLinkEnvironmentConfiguration,
            ref CPPEnvironmentConfiguration OutCPPEnvironmentConfiguration
            )
        {
            UEBuildConfiguration.bCompileLeanAndMeanUE = true;

            // Do not include the editor
            UEBuildConfiguration.bBuildEditor = false;
            UEBuildConfiguration.bBuildWithEditorOnlyData = false;

            // Require cooked data
            UEBuildConfiguration.bBuildRequiresCookedData = true;

            // Compile the engine
            UEBuildConfiguration.bCompileAgainstEngine = true;

            // Tag it as a 'Game' build
            OutCPPEnvironmentConfiguration.Definitions.Add("UE_GAME=1");

            // no exports, so no need to verify that a .lib and .exp file was emitted by the linker.
            OutLinkEnvironmentConfiguration.bHasExports = false;

            // Disable server code
            UEBuildConfiguration.bWithServerCode = false;
        }
    }
}
