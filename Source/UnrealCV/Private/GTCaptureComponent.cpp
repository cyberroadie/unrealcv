#include "UnrealCVPrivate.h"
#include "GTCaptureComponent.h"
#include "ImageWrapper.h"
#include "ViewMode.h"

DECLARE_CYCLE_STAT(TEXT("SaveExr"), STAT_SaveExr, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("SavePng"), STAT_SavePng, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("SaveFile"), STAT_SaveFile, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("ReadPixels"), STAT_ReadPixels, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("ImageWrapper"), STAT_ImageWrapper, STATGROUP_UnrealCV);
DECLARE_CYCLE_STAT(TEXT("GetResource"), STAT_GetResource, STATGROUP_UnrealCV);

void InitCaptureComponent(USceneCaptureComponent2D* CaptureComponent)
{
	// Can not use ESceneCaptureSource::SCS_SceneColorHDR, this option will disable post-processing
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	CaptureComponent->TextureTarget = NewObject<UTextureRenderTarget2D>();
	CaptureComponent->TextureTarget->InitAutoFormat(640, 480); // TODO: Update this later

	CaptureComponent->RegisterComponentWithWorld(GWorld); // What happened for this?
	// CaptureComponent->AddToRoot(); This is not necessary since it has been attached to the Pawn.
}


void SaveExr(UTextureRenderTarget2D* RenderTarget, FString Filename)
{
	SCOPE_CYCLE_COUNTER(STAT_SaveExr)
	int32 Width = RenderTarget->SizeX, Height = RenderTarget->SizeY;
	TArray<FFloat16Color> FloatImage;
	FloatImage.AddZeroed(Width * Height);
	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadFloat16Pixels(FloatImage);

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::EXR);

	ImageWrapper->SetRaw(FloatImage.GetData(), FloatImage.GetAllocatedSize(), Width, Height, ERGBFormat::RGBA, 16);
	const TArray<uint8>& PngData = ImageWrapper->GetCompressed(ImageCompression::Uncompressed);
	{
		SCOPE_CYCLE_COUNTER(STAT_SaveFile);
		FFileHelper::SaveArrayToFile(PngData, *Filename);
	}
}

void SavePng(UTextureRenderTarget2D* RenderTarget, FString Filename)
{
	SCOPE_CYCLE_COUNTER(STAT_SavePng);
	static IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	static IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	{
		int32 Width = RenderTarget->SizeX, Height = RenderTarget->SizeY;
		TArray<FColor> Image;
		FTextureRenderTargetResource* RenderTargetResource;
		Image.AddZeroed(Width * Height);
		{
			SCOPE_CYCLE_COUNTER(STAT_GetResource);
			RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		}
		{
			SCOPE_CYCLE_COUNTER(STAT_ReadPixels);
			RenderTargetResource->ReadPixels(Image);
		}
		{
			SCOPE_CYCLE_COUNTER(STAT_ImageWrapper);
			ImageWrapper->SetRaw(Image.GetData(), Image.GetAllocatedSize(), Width, Height, ERGBFormat::BGRA, 8);
		}
		const TArray<uint8>& ImgData = ImageWrapper->GetCompressed(ImageCompression::Uncompressed);
		{
			SCOPE_CYCLE_COUNTER(STAT_SaveFile);
			FFileHelper::SaveArrayToFile(ImgData, *Filename);
		}
	}
}

UMaterial* UGTCaptureComponent::GetMaterial(FString InModeName = TEXT(""))
{
	// Load material for visualization
	static TMap<FString, FString>* MaterialPathMap = nullptr;
	if (MaterialPathMap == nullptr)
	{
		MaterialPathMap = new TMap<FString, FString>();
		// MaterialPathMap->Add(TEXT("depth"), TEXT("Material'/UnrealCV/SceneDepth.SceneDepth'"));
		MaterialPathMap->Add(TEXT("depth"), TEXT("Material'/UnrealCV/SceneDepth1.SceneDepth1'"));
		MaterialPathMap->Add(TEXT("debug"), TEXT("Material'/UnrealCV/debug.debug'"));
	}

	static TMap<FString, UMaterial*>* StaticMaterialMap = nullptr;
	if (StaticMaterialMap == nullptr)
	{
		StaticMaterialMap = new TMap<FString, UMaterial*>();
		for (auto& Elem : *MaterialPathMap)
		{
			FString ModeName = Elem.Key;
			FString MaterialPath = Elem.Value;
			ConstructorHelpers::FObjectFinder<UMaterial> Material(*MaterialPath); // ConsturctorHelpers is only available for UObject.

			if (Material.Object != NULL)
			{
				StaticMaterialMap->Add(ModeName, (UMaterial*)Material.Object);
			}
		}
	}

	UMaterial* Material = StaticMaterialMap->FindRef(InModeName);
	if (Material == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Can not recognize visualization mode %s"), *InModeName);
	}
	return Material;
}

UGTCaptureComponent* UGTCaptureComponent::Create(APawn* InPawn, FString Mode)
{
	UGTCaptureComponent* GTCapturer = NewObject<UGTCaptureComponent>();

	GTCapturer->bIsActive = true;
	// check(GTCapturer->IsComponentTickEnabled() == true);
	GTCapturer->Pawn = InPawn; // This GTCapturer should depend on the Pawn and be released together with the Pawn.
	GTCapturer->AttachTo(InPawn->GetRootComponent());
	// GTCapturer->AddToRoot();
	GTCapturer->RegisterComponentWithWorld(GWorld);

	// DEPRECATED_FORGAME(4.6, "CaptureComponent2D should not be accessed directly, please use GetCaptureComponent2D() function instead. CaptureComponent2D will soon be private and your code will not compile.")
	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>();
	GTCapturer->CaptureComponent = CaptureComponent;

	// CaptureComponent needs to be attached to somewhere immediately, otherwise it will be gc-ed
	CaptureComponent->AttachTo(InPawn->GetRootComponent());
	InitCaptureComponent(CaptureComponent);

	UMaterial* Material = GetMaterial(Mode);
	if (Mode == "lit") // For rendered images
	{
		FEngineShowFlags& ShowFlags = CaptureComponent->ShowFlags;
		FViewMode::Lit(CaptureComponent->ShowFlags);
		CaptureComponent->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();
	}
	else if (Mode == "object_mask") // For object mask
	{
		FViewMode::VertexColor(CaptureComponent->ShowFlags);
	}
	else
	{
		check(Material);
		// FViewMode::PostProcess(CaptureComponent->ShowFlags);
		// GEngine->GetDisplayGamma(), the default gamma is 2.2
		// CaptureComponent->TextureTarget->TargetGamma = 2.2;

		// FViewMode::Lit(CaptureComponent->ShowFlags);
		// CaptureComponent->ShowFlags.DisableAdvancedFeatures();
		CaptureComponent->ShowFlags = FEngineShowFlags(EShowFlagInitMode::ESFIM_All0);
		FViewMode::PostProcess(CaptureComponent->ShowFlags);

		CaptureComponent->TextureTarget->TargetGamma = 1;
		CaptureComponent->PostProcessSettings.AddBlendable(Material, 1);
	}
	return GTCapturer;
}

UGTCaptureComponent::UGTCaptureComponent()
{
	GetMaterial(); // Initialize the TMap
	PrimaryComponentTick.bCanEverTick = true;
	// bIsTicking = false;

	// Create USceneCaptureComponent2D
	// Design Choice: do I need one capture component with changing post-process materials or multiple components?
	// USceneCaptureComponent2D* CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("GTCaptureComponent"));
}

// Each GTCapturer can serve as one camera of the scene

bool UGTCaptureComponent::Capture(FString InFilename)
{
	if (!bIsPending) // TODO: Use a filename queue to replace this flag.
	{
		FlushRenderingCommands();
		bIsPending = true;
		this->Filename = InFilename;
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("A capture command is still pending."));
		return false;
	}
	// TODO: only enable USceneComponentCapture2D's rendering flag, when I require it to do so.
}

void UGTCaptureComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
// void UGTCaptureComponent::Tick(float DeltaTime) // This tick function should be called by the scene instead of been
{
	// Update rotation of each frame
	// from ab237f46dc0eee40263acbacbe938312eb0dffbb:CameraComponent.cpp:232
	check(this->Pawn); // this GTCapturer should be released, if the Pawn is deleted.
	const APawn* OwningPawn = this->Pawn;
	const AController* OwningController = OwningPawn ? OwningPawn->GetController() : nullptr;
	if (OwningController && OwningController->IsLocalPlayerController())
	{
		const FRotator PawnViewRotation = OwningPawn->GetViewRotation();
		if (!PawnViewRotation.Equals(CaptureComponent->GetComponentRotation()))
		{
			CaptureComponent->SetWorldRotation(PawnViewRotation);
		}
	}

	if (bIsPending)
	{
		FString LowerCaseFilename = this->Filename.ToLower();
		if (LowerCaseFilename.EndsWith("png"))
		{
			SavePng(CaptureComponent->TextureTarget, this->Filename);
		}
		else if (LowerCaseFilename.EndsWith("exr"))
		{
			SaveExr(CaptureComponent->TextureTarget, this->Filename);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Unrecognized image file extension %s"), *LowerCaseFilename);
		}
		// TODO: Run a callback when these operations finished.

		bIsPending = false; // Only tick once.
	}
}
