#include "CameraArrayManager.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "TimerManager.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Selection.h"
#endif

ACameraArrayManager::ACameraArrayManager()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ACameraArrayManager::BeginPlay()
{
	Super::BeginPlay();
	InitializeCaptureComponents();
}

void ACameraArrayManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (ReusableCaptureComponent)
    {
        ReusableCaptureComponent->DestroyComponent();
        ReusableCaptureComponent = nullptr;
    }
    if (ReusableRenderTarget)
    {
        ReusableRenderTarget->MarkAsGarbage();
        ReusableRenderTarget = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

void ACameraArrayManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACameraArrayManager::InitializeCaptureComponents()
{
    if (!IsValid(ReusableCaptureComponent))
    {
        ReusableCaptureComponent = NewObject<USceneCaptureComponent2D>(this, TEXT("ReusableCaptureComponent"));
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        ReusableCaptureComponent->bCaptureEveryFrame = false;
        ReusableCaptureComponent->bCaptureOnMovement = false;
        
        ReusableCaptureComponent->ShowFlags.SetAtmosphere(true);
        ReusableCaptureComponent->ShowFlags.SetBSP(true);
        ReusableCaptureComponent->ShowFlags.SetSkeletalMeshes(true);
        ReusableCaptureComponent->ShowFlags.SetStaticMeshes(true);
        ReusableCaptureComponent->ShowFlags.SetLighting(true);
        ReusableCaptureComponent->ShowFlags.SetSkyLighting(true);
        ReusableCaptureComponent->ShowFlags.SetParticles(true);
        ReusableCaptureComponent->ShowFlags.SetTranslucency(true);
        ReusableCaptureComponent->ShowFlags.SetAntiAliasing(true);
        
        ReusableCaptureComponent->RegisterComponentWithWorld(GetWorld());
    }

    if (!IsValid(ReusableRenderTarget) || ReusableRenderTarget->SizeX != RenderTargetX || ReusableRenderTarget->SizeY != RenderTargetY)
    {
        ReusableRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableRenderTarget"));
        ReusableRenderTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
        ReusableRenderTarget->SizeX = RenderTargetX;
        ReusableRenderTarget->SizeY = RenderTargetY;
        ReusableRenderTarget->InitAutoFormat(RenderTargetX, RenderTargetY);
        ReusableRenderTarget->UpdateResource();
    }

    ReusableCaptureComponent->TextureTarget = ReusableRenderTarget;
}

// *** FIX: Re-adding the previously omitted function definitions ***

void ACameraArrayManager::CreateOrUpdateCameras()
{
    ClearAllCameras();

    UWorld* const World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateOrUpdateCameras: 获取UWorld失败。"));
        return;
    }
    
    if (NumCameras <= 0)
    {
        UE_LOG(LogTemp, Log, TEXT("CreateOrUpdateCameras: NumCameras为0，不创建相机。"));
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;

    for (int32 i = 0; i < NumCameras; ++i)
    {
        const FTransform CameraTransform = GetCameraTransform(i);
        ACineCameraActor* NewCamera = World->SpawnActor<ACineCameraActor>(ACineCameraActor::StaticClass(), CameraTransform, SpawnParams);
        
        if (NewCamera)
        {
            UCineCameraComponent* CineCamComponent = NewCamera->GetCineCameraComponent();
            if (CineCamComponent)
            {
                CineCamComponent->SetFieldOfView(CameraFOV);
            }

            FString CameraLabel = FString::Printf(TEXT("%s_%03d"), *CameraNamePrefix, i);
#if WITH_EDITOR
            NewCamera->SetActorLabel(CameraLabel);
#endif
            ManagedCameras.Add(NewCamera);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("CreateOrUpdateCameras: 生成相机 %d 失败。"), i);
        }
    }

    OrganizeCamerasInFolder();
    UE_LOG(LogTemp, Log, TEXT("CreateOrUpdateCameras: 成功创建或更新了 %d 个相机。"), NumCameras);
}

void ACameraArrayManager::ClearAllCameras()
{
    if (bIsTaskRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClearAllCameras: 无法在渲染任务进行中清除相机。"));
        return;
    }
    
    UWorld* const World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClearAllCameras: 获取UWorld失败。"));
        return;
    }

    int32 DestroyedCount = 0;
    for (AActor* Camera : ManagedCameras)
    {
        if (IsValid(Camera)) 
        {
            World->DestroyActor(Camera);
            DestroyedCount++;
        }
    }
    
    ManagedCameras.Empty();
    UE_LOG(LogTemp, Log, TEXT("ClearAllCameras: 成功销毁了 %d 个相机。"), DestroyedCount);
}

void ACameraArrayManager::RenderAllViews()
{
    if (bIsTaskRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderAllViews: 另一个渲染任务已在进行中。"));
        return;
    }
    if (NumCameras <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderAllViews: 相机数量为0。"));
        return;
    }

    bIsTaskRunning = true;
    InitializeCaptureComponents(); 
    
    GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
    CurrentRenderIndex = 0;
    RenderProgress = 0;
    
    PerformSingleCapture();
    OpenOutputFolder();
}

void ACameraArrayManager::PerformSingleCapture()
{
    if (CurrentRenderIndex >= NumCameras)
    {
        UE_LOG(LogTemp, Log, TEXT("渲染流程完成!"));
        GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
        RenderProgress = 100;
        RenderStatus = TEXT("渲染完成");
        bIsTaskRunning = false;
        return;
    }
    
    if (!GetWorld() || !IsValid(ReusableCaptureComponent) || !IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCapture: World或渲染组件无效!"));
        RenderStatus = TEXT("渲染失败: 内部组件错误");
        bIsTaskRunning = false;
        return;
    }

    RenderProgress = FMath::RoundHalfFromZero((float)CurrentRenderIndex / NumCameras * 100);
    RenderStatus = FString::Printf(TEXT("渲染中... (%d/%d)"), CurrentRenderIndex + 1, NumCameras);
    UE_LOG(LogTemp, Log, TEXT("开始渲染相机索引 %d"), CurrentRenderIndex);

    const FTransform CameraTransform = GetCameraTransform(CurrentRenderIndex);
    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraFOV;
    
    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, CurrentRenderIndex, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName);
    
    CurrentRenderIndex++;
    
    FTimerDelegate TimerDel;
    TimerDel.BindUObject(this, &ACameraArrayManager::PerformSingleCapture);
    GetWorld()->GetTimerManager().SetTimer(RenderTimerHandle, TimerDel, 0.02f, false);
}

void ACameraArrayManager::SaveRenderTargetToFileAsync(const FString& FullOutputPath, const FString& FileName)
{
    if (!IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: 可复用的RenderTarget无效。"));
        return;
    }
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*FullOutputPath))
    {
        PlatformFile.CreateDirectoryTree(*FullOutputPath);
    }

    const FString FilePath = FullOutputPath / FileName;
    const int32 Width = ReusableRenderTarget->SizeX;
    const int32 Height = ReusableRenderTarget->SizeY;
    const ECameraArrayImageFormat ImageFormatToSave = FileFormat;

    TArray<FColor> RawPixels;
    FTextureRenderTargetResource* RenderTargetResource = 
    ReusableRenderTarget->GameThread_GetRenderTargetResource();

    if (!RenderTargetResource || !RenderTargetResource->ReadPixels(RawPixels))
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: 从RenderTarget读取像素失败。"));
        return;
    }
    
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Width, Height, FilePath, ImageFormatToSave, RawPixels{MoveTemp(RawPixels)}]()
    {
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        
        ::EImageFormat EngineImageFormat;
        switch (ImageFormatToSave)
        {
            case ECameraArrayImageFormat::JPEG: EngineImageFormat = ::EImageFormat::JPEG; break;
            case ECameraArrayImageFormat::BMP:  EngineImageFormat = ::EImageFormat::BMP; break;
            case ECameraArrayImageFormat::PNG:
            default:                            EngineImageFormat = ::EImageFormat::PNG; break;
        }

        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EngineImageFormat);
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(RawPixels.GetData(), RawPixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
        {
            UE_LOG(LogTemp, Error, TEXT("为 %s 编码图像数据失败。"), *FilePath);
            return;
        }

        const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();
        if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
        {
            UE_LOG(LogTemp, Log, TEXT("成功异步保存图像到: %s"), *FilePath);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("保存图像文件失败: %s"), *FilePath);
        }
    });
}

FString ACameraArrayManager::GetFileExtension() const
{
    switch (FileFormat)
    {
    case ECameraArrayImageFormat::PNG: return TEXT("png");
    case ECameraArrayImageFormat::JPEG: return TEXT("jpg");
    case ECameraArrayImageFormat::BMP: return TEXT("bmp");
    default: return TEXT("png");
    }
}

FTransform ACameraArrayManager::GetCameraTransform(int32 CameraIndex) const
{
    FVector Location = StartLocation;
    if (NumCameras > 1)
    {
        const float TotalDistanceCenti = TotalYDistance * 100.0f;
        const float Spacing = (NumCameras > 1) ? (TotalDistanceCenti / (NumCameras - 1)) : 0.0f;
        Location.Y += CameraIndex * Spacing;
    }

    FRotator Rotation = SharedRotation;
    if (bUseLookAtTarget && LookAtTarget != nullptr)
    {
        FVector TargetLocation = LookAtTarget->GetActorLocation();
        FVector Direction = (TargetLocation - Location).GetSafeNormal();
        Rotation = Direction.ToOrientationRotator();
    }
    
    return FTransform(Rotation, Location);
}

void ACameraArrayManager::SelectFirstCamera()
{
    if (ManagedCameras.Num() > 0 && IsValid(ManagedCameras[0]))
    {
#if WITH_EDITOR
        if (GEditor)
        {
            GEditor->SelectNone(true, true);
            GEditor->SelectActor(ManagedCameras[0], true, true);
        }
#endif
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SelectFirstCamera: 没有可用的相机。"));
    }
}

void ACameraArrayManager::SelectLastCamera()
{
    if (ManagedCameras.Num() > 0)
    {
        const int32 LastIndex = ManagedCameras.Num() - 1;
        if (IsValid(ManagedCameras[LastIndex]))
        {
#if WITH_EDITOR
            if (GEditor)
            {
                GEditor->SelectNone(true, true);
                GEditor->SelectActor(ManagedCameras[LastIndex], true, true);
            }
#endif
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SelectLastCamera: 最后一个相机无效。"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SelectLastCamera: 没有可用的相机。"));
    }
}

void ACameraArrayManager::OrganizeCamerasInFolder()
{
#if WITH_EDITOR
    for (AActor* Camera : ManagedCameras)
    {
        if (IsValid(Camera))
        {
            Camera->SetFolderPath(FName(*FString::Printf(TEXT("CameraArray/%s"), *CameraNamePrefix)));
        }
    }
#endif
}

void ACameraArrayManager::RenderFirstCamera()
{
    if (bIsTaskRunning) return;
    if (NumCameras <= 0) return;
    
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    PerformSingleCaptureForSpecificIndex(0);
    OpenOutputFolder();
}

void ACameraArrayManager::RenderLastCamera()
{
    if (bIsTaskRunning) return;
    if (NumCameras <= 0) return;
    
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    const int32 LastCameraIndex = NumCameras - 1;
    PerformSingleCaptureForSpecificIndex(LastCameraIndex);
    OpenOutputFolder();
}

void ACameraArrayManager::OpenOutputFolder()
{
    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*FullOutputPath))
    {
        PlatformFile.CreateDirectoryTree(*FullOutputPath);
    }
    
    FPlatformProcess::ExploreFolder(*FullOutputPath);
    UE_LOG(LogTemp, Log, TEXT("已打开输出文件夹: %s"), *FullOutputPath);
}

void ACameraArrayManager::PerformSingleCaptureForSpecificIndex(int32 IndexToCapture)
{
    if (!GetWorld() || !IsValid(ReusableCaptureComponent) || !IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCaptureForSpecificIndex: World或渲染组件无效!"));
        RenderStatus = TEXT("渲染失败: 内部组件错误");
        bIsTaskRunning = false;
        return;
    }
    
    RenderProgress = 50;
    RenderStatus = FString::Printf(TEXT("渲染中... (相机 %d)"), IndexToCapture + 1);

    const FTransform CameraTransform = GetCameraTransform(IndexToCapture);
    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraFOV;

    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, IndexToCapture, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName);

    RenderProgress = 100;
    RenderStatus = TEXT("渲染完成 (文件后台保存中)");
    bIsTaskRunning = false;
}