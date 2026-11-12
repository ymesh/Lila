#include "LilaLinkRender.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "ContentStreaming.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Editor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/DataDrivenPlatformInfoRegistry.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"

namespace
{
/** ForwardLevelUp's settings (github.com/Vaei/ForwardLevelUp); a stock engine has none of them. */
const TCHAR* const ForwardLevelUpVariables[] = {
	TEXT("r.Mobile.ClusteredDecals"),
	TEXT("r.Mobile.PrimitiveAO"),
	TEXT("r.Mobile.HeightFieldAO"),
	TEXT("r.Mobile.HeightFieldGI"),
	TEXT("r.Mobile.DistanceFieldAO"),
	TEXT("r.Mobile.Forward.NormalTarget"),
	TEXT("r.Mobile.MSAA.PerSampleDepthFade"),
	TEXT("r.WideCustomResolve"),
};

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

TSharedPtr<FJsonValue> ConsoleValue(const TCHAR* Name)
{
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
	if (!Var)
	{
		return MakeShared<FJsonValueNull>();
	}
	return MakeShared<FJsonValueNumber>(Var->GetFloat());
}

bool ReadVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!Params->TryGetArrayField(Field, A) || A->Num() != 3)
	{
		return false;
	}
	Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
	return true;
}

TSharedPtr<FJsonValue> RendererInfo(const TSharedPtr<FJsonObject>&, FString& OutError)
{
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("rendererInfo: no editor world");
		return nullptr;
	}
	FName LevelName;
	GetFeatureLevelName(World->GetFeatureLevel(), LevelName);
	const EShaderPlatform Platform = GEditor->GetActiveShaderPlatform();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("featureLevel"), LevelName.ToString());
	Out->SetStringField(TEXT("shaderPlatform"), FDataDrivenShaderPlatformInfo::GetName(Platform).ToString());
	Out->SetBoolField(TEXT("mobile"), IsMobilePlatform(Platform));
	Out->SetBoolField(TEXT("previewActive"), GEditor->IsPreviewPlatformActive());
	Out->SetStringField(TEXT("preview"), GEditor->PreviewPlatform.GetFriendlyName().ToString());
	Out->SetField(TEXT("desktopForward"), ConsoleValue(TEXT("r.ForwardShading")));
	// 0 forward, 1 deferred.
	Out->SetField(TEXT("mobileShadingPath"), ConsoleValue(TEXT("r.Mobile.ShadingPath")));
	TSharedRef<FJsonObject> Flu = MakeShared<FJsonObject>();
	bool bAny = false;
	for (const TCHAR* Name : ForwardLevelUpVariables)
	{
		TSharedPtr<FJsonValue> V = ConsoleValue(Name);
		bAny |= V->Type != EJson::Null;
		Flu->SetField(Name, V);
	}
	Out->SetBoolField(TEXT("forwardLevelUp"), bAny);
	int32 SkyLights = 0;
	for (TObjectIterator<USkyLightComponent> It; It; ++It)
	{
		SkyLights += It->GetWorld() == World && It->IsRegistered();
	}
	// Faces turned from the sun get their light from a sky light. In mobile preview captures a SkyAtmosphere draws black, so
	// a sky light capturing the scene captures black there and only a cubemap sky light lights those faces.
	Out->SetNumberField(TEXT("skyLights"), SkyLights);
	Out->SetObjectField(TEXT("forwardLevelUpSettings"), Flu);
	return MakeShared<FJsonValueObject>(Out);
}

/** The preview menu entry for mode: an exact preview shader platform name, or "mobile" for an ES3.1 one (Android first). */
const FPreviewPlatformMenuItem* FindPreview(const FString& Mode, TArray<FString>& OutNames)
{
	const FPreviewPlatformMenuItem* Pick = nullptr;
	for (const FPreviewPlatformMenuItem& Item : FDataDrivenPlatformInfoRegistry::GetAllPreviewPlatformMenuItems())
	{
		const EShaderPlatform Sp = FDataDrivenShaderPlatformInfo::GetShaderPlatformFromName(Item.PreviewShaderPlatformName);
		if (Sp >= SP_NumPlatforms)
		{
			continue;
		}
		const FString Name = Item.PreviewShaderPlatformName.ToString();
		OutNames.Add(Name);
		if (Mode != TEXT("mobile"))
		{
			if (Name == Mode)
			{
				return &Item;
			}
			continue;
		}
		const bool bMobile = IsMobilePlatform(Sp) && GetMaxSupportedFeatureLevel(Sp) == ERHIFeatureLevel::ES3_1;
		if (bMobile && (!Pick || (Item.PlatformName == TEXT("Android") && Pick->PlatformName != TEXT("Android"))))
		{
			Pick = &Item;
		}
	}
	return Pick;
}

TSharedPtr<FJsonValue> SetPreviewPlatform(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkRender::SetPreviewPlatform);
	if (!GEditor)
	{
		OutError = TEXT("setPreviewPlatform: no editor");
		return nullptr;
	}
	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode))
	{
		OutError = TEXT("setPreviewPlatform: give mode (mobile, off or a preview shader platform name)");
		return nullptr;
	}
	if (Mode == TEXT("off"))
	{
		GEditor->SetPreviewPlatform(FPreviewPlatformInfo(GMaxRHIFeatureLevel, GMaxRHIShaderPlatform), false);
	}
	else
	{
		TArray<FString> Names;
		const FPreviewPlatformMenuItem* Pick = FindPreview(Mode, Names);
		if (!Pick)
		{
			OutError = FString::Printf(TEXT("setPreviewPlatform: no preview platform matches %s (have: %s)"), *Mode, *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}
		const EShaderPlatform Sp = FDataDrivenShaderPlatformInfo::GetShaderPlatformFromName(Pick->PreviewShaderPlatformName);
		GEditor->SetPreviewPlatform(FPreviewPlatformInfo(GetMaxSupportedFeatureLevel(Sp), Sp, Pick->PlatformName, Pick->ShaderFormat,
										Pick->DeviceProfileName, true, Pick->PreviewShaderPlatformName),
			false);
	}
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	FString Ignored;
	return RendererInfo(Params, Ignored);
}

TSharedPtr<FJsonValue> CaptureView(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkRender::CaptureView);
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("captureView: no editor world");
		return nullptr;
	}
	FString Path;
	FVector Location;
	if (!Params->TryGetStringField(TEXT("path"), Path) || !ReadVector(Params, TEXT("location"), Location))
	{
		OutError = TEXT("captureView: give path and location [x, y, z] (cm, Unreal axes)");
		return nullptr;
	}
	FRotator Rotation = FRotator::ZeroRotator;
	FVector LookAt, Angles;
	if (ReadVector(Params, TEXT("lookAt"), LookAt))
	{
		Rotation = (LookAt - Location).Rotation();
	}
	else if (ReadVector(Params, TEXT("rotation"), Angles))
	{
		Rotation = FRotator(Angles.X, Angles.Y, Angles.Z);
	}
	int32 W = 1280, H = 720, Warmup = 8;
	double Fov = 90.0;
	Params->TryGetNumberField(TEXT("width"), W);
	Params->TryGetNumberField(TEXT("height"), H);
	Params->TryGetNumberField(TEXT("fov"), Fov);
	Params->TryGetNumberField(TEXT("warmup"), Warmup);
	W = FMath::Clamp(W, 16, 8192);
	H = FMath::Clamp(H, 16, 8192);

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	Spawn.bHideFromSceneOutliner = true;
	ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(Location, Rotation, Spawn);
	if (!Actor)
	{
		OutError = TEXT("captureView: could not spawn a scene capture");
		return nullptr;
	}
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->RenderTargetFormat = RTF_RGBA8_SRGB;
	Target->InitAutoFormat(W, H);
	Target->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();
	Capture->TextureTarget = Target;
	Capture->CaptureSource = SCS_FinalColorLDR;
	Capture->FOVAngle = float(Fov);
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	// Keeps temporal history and exposure between the warm-up captures.
	Capture->bAlwaysPersistRenderingState = true;

	// The first capture requests the shaders this view needs; compile them and stream textures before the real ones.
	Capture->CaptureScene();
	FlushRenderingCommands();
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	IStreamingManager::Get().StreamAllResources(5.0f);
	for (int32 I = 0; I < Warmup; ++I)
	{
		Capture->CaptureScene();
	}
	FlushRenderingCommands();

	TArray<FColor> Pixels;
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	const bool bRead = Resource && Resource->ReadPixels(Pixels) && Pixels.Num() == W * H;
	Actor->Destroy();
	if (!bRead)
	{
		OutError = TEXT("captureView: reading the capture failed");
		return nullptr;
	}
	FLinearColor Sum(0, 0, 0, 0);
	for (FColor& C : Pixels)
	{
		C.A = 255;
		Sum += FLinearColor(C.R / 255.0f, C.G / 255.0f, C.B / 255.0f, 1.0f);
	}
	Sum /= float(Pixels.Num());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), W, H)))
	{
		OutError = FString::Printf(TEXT("captureView: could not write %s"), *Path);
		return nullptr;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), Path);
	Out->SetNumberField(TEXT("width"), W);
	Out->SetNumberField(TEXT("height"), H);
	Out->SetArrayField(TEXT("meanColor"), {MakeShared<FJsonValueNumber>(Sum.R), MakeShared<FJsonValueNumber>(Sum.G), MakeShared<FJsonValueNumber>(Sum.B)});
	FString Ignored;
	Out->SetField(TEXT("renderer"), RendererInfo(Params, Ignored));
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddRenderMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("rendererInfo"), LilaLink::Sync(&RendererInfo));
	Methods.Add(TEXT("setPreviewPlatform"), LilaLink::Sync(&SetPreviewPlatform));
	Methods.Add(TEXT("captureView"), LilaLink::Sync(&CaptureView));
}
