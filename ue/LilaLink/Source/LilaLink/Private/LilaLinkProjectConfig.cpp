#include "LilaLinkProjectConfig.h"

#include "LilaLinkLog.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

FString LilaLink::UpdateProjectIni(UObject* Settings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkProjectConfig::UpdateProjectIni);

	if (!Settings)
	{
		return FString();
	}
	const FString File = FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename());
	FString Before;
	const bool bExisted = FFileHelper::LoadFileToString(Before, *File);
	Settings->TryUpdateDefaultConfigFile();
	FString After;
	FFileHelper::LoadFileToString(After, *File);
	if (bExisted && Before == After)
	{
		return FString();
	}
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("LilaLink/ConfigBackups"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	// Several saves within a second (settings classes sharing DefaultPlugins.ini) each keep their own copy.
	const FString Stem = Dir / FPaths::GetBaseFilename(File) + TEXT("-") + FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const TCHAR* Extension = bExisted ? TEXT(".ini") : TEXT(".absent");
	FString Backup = Stem + Extension;
	for (int32 N = 2; IFileManager::Get().FileExists(*Backup); ++N)
	{
		Backup = FString::Printf(TEXT("%s-%d%s"), *Stem, N, Extension);
	}
	const bool bSaved = bExisted ? FFileHelper::SaveStringToFile(Before, *Backup)
								 : FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s did not exist before LilaLink wrote it: delete it to go back."), *File), *Backup);
	UE_LOG(LogLilaLink, Log, TEXT("%s changed; it was %s"), *File, bSaved ? *(TEXT("backed up to ") + Backup) : TEXT("not backed up (Saved is not writable)"));
	return bSaved ? Backup : FString();
}

TArray<FName> LilaLink::SetLaneProfileUser(const UWorld* World, const FString& User, const TArray<FName>& Profiles)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkProjectConfig::SetLaneProfileUser);

	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("LilaLink/LaneProfileUsers.json"));
	const FString Level = World ? World->GetPackage()->GetName() : FString();
	TMap<FString, TArray<FString>> Users;
	TSet<FString> Held;
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (FFileHelper::LoadFileToString(Text, *File) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
	{
		const TSharedPtr<FJsonObject>* Listed = nullptr;
		if (Root->TryGetObjectField(TEXT("users"), Listed))
		{
			for (const auto& [Key, Value] : (*Listed)->Values)
			{
				TArray<FString>& Names = Users.Add(FString(Key));
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (Value->TryGetArray(Array))
				{
					for (const TSharedPtr<FJsonValue>& Name : *Array)
					{
						Names.AddUnique(Name->AsString());
						Held.Add(Name->AsString());
					}
				}
			}
		}
	}
	// Keys are "<level package>|<user>": package names never hold a '|'.
	const FString Key = Level + TEXT("|") + User;
	Users.Remove(Key);
	if (!Profiles.IsEmpty())
	{
		TArray<FString>& Names = Users.Add(Key);
		for (const FName& Name : Profiles)
		{
			Names.AddUnique(Name.ToString());
			Held.Add(Name.ToString());
		}
	}
	for (auto It = Users.CreateIterator(); It; ++It)
	{
		FString Owner, Rest;
		It.Key().Split(TEXT("|"), &Owner, &Rest);
		if (Owner != Level && !FPackageName::DoesPackageExist(Owner))
		{
			It.RemoveCurrent();
		}
	}
	TSet<FString> Used;
	for (const auto& [Owner, Names] : Users)
	{
		Used.Append(Names);
	}
	TArray<FName> Unused;
	for (const FString& Name : Held)
	{
		if (!Used.Contains(Name))
		{
			Unused.Add(FName(*Name));
		}
	}

	if (Users.IsEmpty())
	{
		IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), /*RequireExists=*/false, /*Tree=*/false);
		return Unused;
	}
	Users.KeySort(TLess<FString>());
	TSharedRef<FJsonObject> Listed = MakeShared<FJsonObject>();
	for (const auto& [Owner, Names] : Users)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Name : Names)
		{
			Values.Add(MakeShared<FJsonValueString>(Name));
		}
		Listed->SetArrayField(Owner, Values);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("users"), Listed);
	FString Written;
	FJsonSerializer::Serialize(Out, TJsonWriterFactory<>::Create(&Written));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(Written, *File))
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s could not be written: lane profiles Lila added are not tracked and stay in the project"), *File);
	}
	return Unused;
}
