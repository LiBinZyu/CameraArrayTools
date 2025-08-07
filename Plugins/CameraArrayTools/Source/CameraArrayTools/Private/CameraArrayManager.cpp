#include "CameraArrayManager.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
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
#include "Engine/PostProcessVolume.h"
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
    if (ReusableHdrRenderTarget||ReusableLdrRenderTarget)
    {
        ReusableHdrRenderTarget->MarkAsGarbage();
        ReusableLdrRenderTarget->MarkAsGarbage();
        ReusableHdrRenderTarget = nullptr;
        ReusableLdrRenderTarget = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void ACameraArrayManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (bIsTaskRunning) return; // 渲染时禁止修改，防止冲突

    const FName MemberPropertyName = (PropertyChangedEvent.Property != nullptr)
        ? PropertyChangedEvent.MemberProperty->GetFName()
        : NAME_None;

    if (MemberPropertyName == NAME_None) return;

    // 只有相机数量改变时，才执行完全重建
    if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, NumCameras))
    {
        CreateOrUpdateCameras();
    }
    // 只更新位置，保留手动调整过的旋转
    else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, TotalYDistance) ||
             MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, StartLocation))
    {
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FRotator CurrentRotation = Camera->GetActorRotation(); // 保存当前旋转
                const FVector NewLocation = GetCameraTransform(i).GetLocation(); // 计算新位置
                Camera->SetActorLocationAndRotation(NewLocation, CurrentRotation, false, nullptr, ETeleportType::None);
            }
        }
    }
    // 只更新旋转，保留手动调整过的位置
    else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, SharedRotation) ||
             MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, bUseLookAtTarget) ||
             MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, LookAtTarget))
    {
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FVector CurrentLocation = Camera->GetActorLocation(); // 保存当前位置
                const FRotator NewRotation = GetCameraTransform(i).GetRotation().Rotator(); // 计算新旋转
                Camera->SetActorLocationAndRotation(CurrentLocation, NewRotation, false, nullptr, ETeleportType::None);
            }
        }
    }
    // 只更新相机命名和文件夹路径
    else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, CameraNamePrefix))
    {
        const FName FolderName = FName(TEXT("CameraArray"));
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FString NewLabel = FString::Printf(TEXT("%s_%03d"), *CameraNamePrefix, i);
                Camera->SetActorLabel(NewLabel); // 更新Actor在编辑器中的名字
                Camera->SetFolderPath(FolderName); // 更新文件夹
            }
        }
    }
    // 只更新FOV
    else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, CameraFOV))
    {
        for (AActor* Camera : ManagedCameras)
        {
            if (Camera)
            {
                if (UCineCameraComponent* CineCamComponent = Camera->FindComponentByClass<UCineCameraComponent>())
                {
                    CineCamComponent->SetFieldOfView(CameraFOV);
                }
            }
        }
    }
    // 只更新Filmback的宽高比
    else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, RenderTargetX) ||
             MemberPropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, RenderTargetY))
    {
        if (RenderTargetY > 0 && RenderTargetX > 0)
        {
            const float DesiredAspectRatio = static_cast<float>(RenderTargetX) / static_cast<float>(RenderTargetY);
            for (AActor* Camera : ManagedCameras)
            {
                if (UCineCameraComponent* CineCamComponent = Camera->FindComponentByClass<UCineCameraComponent>())
                {
                    // 保持宽度，根据宽高比调整高度
                    CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth / DesiredAspectRatio;
                }
            }
        }
    }
}

void ACameraArrayManager::SyncShowFlagsWithEditorViewport()
{
    if (GEditor && ReusableCaptureComponent)
    {
        FViewport* ActiveViewport = GEditor->GetActiveViewport();
        if (ActiveViewport)
        {
            FEditorViewportClient* ViewportClient = (FEditorViewportClient*)ActiveViewport->GetClient();
            if (ViewportClient)
            {
                ReusableCaptureComponent->ShowFlags = ViewportClient->EngineShowFlags;
                UE_LOG(LogTemp, Log, TEXT("成功将截图组件的ShowFlags与编辑器视口同步。"));
            }
        }
    }
}
void ACameraArrayManager::SyncPostProcessSettings()
{
    if (IsValid(ReusableCaptureComponent))
    {
        if (IsValid(PostProcessVolumeRef))
        {
            ReusableCaptureComponent->PostProcessSettings = PostProcessVolumeRef->Settings;
            ReusableCaptureComponent->PostProcessBlendWeight = PostProcessVolumeRef->BlendWeight;
            UE_LOG(LogTemp, Log, TEXT("成功从 %s 同步后期处理设置。"), *PostProcessVolumeRef->GetName());
        }
        else
        {
            ReusableCaptureComponent->PostProcessSettings = FPostProcessSettings();
            ReusableCaptureComponent->PostProcessBlendWeight = 0.0f; // 权重为0等于没效果
        }
    }
}
#endif

void ACameraArrayManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACameraArrayManager::InitializeCaptureComponents()
{
    if (!IsValid(ReusableCaptureComponent))
    {
        ReusableCaptureComponent = NewObject<USceneCaptureComponent2D>(this, TEXT("ReusableCaptureComponent"));
        ReusableCaptureComponent->bCaptureEveryFrame = false;
        ReusableCaptureComponent->bCaptureOnMovement = false;
        ReusableCaptureComponent->RegisterComponentWithWorld(GetWorld());
    }

    // --- LDR Render Target (for PNG, JPG, BMP, TGA) ---
    if (!IsValid(ReusableLdrRenderTarget) || ReusableLdrRenderTarget->SizeX != RenderTargetX || ReusableLdrRenderTarget->SizeY != RenderTargetY)
    {
        ReusableLdrRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableLdrRenderTarget"));
        ReusableLdrRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
        ReusableLdrRenderTarget->SizeX = RenderTargetX;
        ReusableLdrRenderTarget->SizeY = RenderTargetY;
        ReusableLdrRenderTarget->bAutoGenerateMips = false;
        ReusableLdrRenderTarget->UpdateResource();
    }

    // --- HDR Render Target (for EXR, TIFF, HDR) ---
    if (!IsValid(ReusableHdrRenderTarget) || ReusableHdrRenderTarget->SizeX != RenderTargetX || ReusableHdrRenderTarget->SizeY != RenderTargetY)
    {
        ReusableHdrRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableHdrRenderTarget"));
        ReusableHdrRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
        ReusableHdrRenderTarget->SizeX = RenderTargetX;
        ReusableHdrRenderTarget->SizeY = RenderTargetY;
        ReusableHdrRenderTarget->bAutoGenerateMips = false;
        ReusableHdrRenderTarget->UpdateResource();
    }
}

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

                if (RenderTargetY > 0 && RenderTargetX > 0)
                {
                    const float DesiredAspectRatio = static_cast<float>(RenderTargetX) / static_cast<float>(RenderTargetY);
                    CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth / DesiredAspectRatio;
                }
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
#if WITH_EDITOR
            Camera->SetFolderPath(NAME_None);
#endif
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
    if (ManagedCameras.Num() <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderAllViews: 场景中没有可渲染的相机。"));
        return;
    }

#if WITH_EDITOR
    SyncShowFlagsWithEditorViewport();
    SyncPostProcessSettings();
#endif
    
    bIsTaskRunning = true;
    InitializeCaptureComponents(); 
    
    if (IsHdrFormat())
    {
        ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
    }
    else
    {
        ReusableCaptureComponent->TextureTarget = ReusableLdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }

    GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
    CurrentRenderIndex = 0;
    RenderProgress = 0;
    
    PerformSingleCapture();
    OpenOutputFolder();
}

void ACameraArrayManager::PerformSingleCapture()
{
    if (CurrentRenderIndex >= ManagedCameras.Num()) // 检查是否超出数组范围
    {
        UE_LOG(LogTemp, Log, TEXT("渲染流程完成!"));
        GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
        RenderProgress = 100;
        RenderStatus = TEXT("渲染完成");
        bIsTaskRunning = false;
        return;
    }
    
    AActor* CameraActor = ManagedCameras[CurrentRenderIndex];
    if (!IsValid(CameraActor))
    {
        UE_LOG(LogTemp, Warning, TEXT("跳过无效的相机索引 %d"), CurrentRenderIndex);
        CurrentRenderIndex++;
        PerformSingleCapture(); // 立即尝试渲染下一个
        return;
    }

    const UCineCameraComponent* CineCamComponent = CameraActor->FindComponentByClass<UCineCameraComponent>();
    if (!CineCamComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("相机 %s 没有CineCameraComponent，跳过。"), *CameraActor->GetName());
        CurrentRenderIndex++;
        PerformSingleCapture(); // 立即尝试渲染下一个
        return;
    }
    
    const FTransform CameraTransform = CameraActor->GetActorTransform();
    const float CameraRealFOV = CineCamComponent->FieldOfView;
    
    RenderProgress = FMath::RoundHalfFromZero((float)CurrentRenderIndex / ManagedCameras.Num() * 100);
    RenderStatus = FString::Printf(TEXT("渲染中... (%d/%d)"), CurrentRenderIndex + 1, ManagedCameras.Num());
    UE_LOG(LogTemp, Log, TEXT("开始渲染相机 %s"), *CameraActor->GetName());
    
    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraRealFOV;
    
    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, CurrentRenderIndex, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName, ReusableCaptureComponent->TextureTarget);
    
    CurrentRenderIndex++;
    
    FTimerDelegate TimerDel;
    TimerDel.BindUObject(this, &ACameraArrayManager::PerformSingleCapture);
    GetWorld()->GetTimerManager().SetTimer(RenderTimerHandle, TimerDel, 0.02f, false);
}

void ACameraArrayManager::SaveRenderTargetToFileAsync(const FString& FullOutputPath, const FString& FileName, UTextureRenderTarget2D* RenderTargetToSave)
{
    if (!IsValid(RenderTargetToSave))
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: Invalid RenderTargetToSave"));
        return;
    }
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*FullOutputPath))
    {
        PlatformFile.CreateDirectoryTree(*FullOutputPath);
    }

    const FString FilePath = FullOutputPath / FileName;
    const int32 Width = RenderTargetToSave->SizeX;
    const int32 Height = RenderTargetToSave->SizeY;
    const ECameraArrayImageFormat ImageFormatToSave = FileFormat;

    // --- 捕获Gamma校正参数 ---
    const bool bApplyGamma = this->bEnableLdrGammaCorrection;
    const float Gamma = this->LdrGammaValue;

    const bool bIsHdr = (RenderTargetToSave->RenderTargetFormat == RTF_RGBA16f);

    if (bIsHdr)
    {
        // --- HDR SAVING LOGIC ---
        TArray<FLinearColor> RawPixels;
        FTextureRenderTargetResource* RenderTargetResource = RenderTargetToSave->GameThread_GetRenderTargetResource();
        if (!RenderTargetResource || !RenderTargetResource->ReadLinearColorPixels(RawPixels))
        {
            UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync (HDR): 从RenderTarget读取像素失败。"));
            return;
        }

        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Width, Height, FilePath, ImageFormatToSave, RawPixels{MoveTemp(RawPixels)}]()
        {
            TArray<FLinearColor> PixelsToSave = RawPixels;
            // Force alpha to 1.0 (opaque) for all HDR formats
            for (FLinearColor& Pixel : PixelsToSave)
            {
                Pixel.A = 1.0f;
            }

            IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
            EImageFormat EngineImageFormat;
            switch (ImageFormatToSave)
            {
                case ECameraArrayImageFormat::EXR: EngineImageFormat = EImageFormat::EXR; break;
                default: UE_LOG(LogTemp, Error, TEXT("Invalid HDR format specified.")); return;
            }

            TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EngineImageFormat);
            if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(PixelsToSave.GetData(), PixelsToSave.Num() * sizeof(FLinearColor), Width, Height, ERGBFormat::RGBAF, 32))
            {
                UE_LOG(LogTemp, Error, TEXT("为 %s 编码HDR图像数据失败。"), *FilePath);
                return;
            }
            
            const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();
            if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
            {
                UE_LOG(LogTemp, Log, TEXT("成功异步保存HDR图像到: %s"), *FilePath);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("保存HDR图像文件失败: %s"), *FilePath);
            }
        });
    }
    else
    {
        // --- LDR SAVING LOGIC ---
        TArray<FColor> RawPixels;
        FTextureRenderTargetResource* RenderTargetResource = RenderTargetToSave->GameThread_GetRenderTargetResource();
        if (!RenderTargetResource || !RenderTargetResource->ReadPixels(RawPixels))
        {
            UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync (LDR): 从RenderTarget读取像素失败。"));
            return;
        }
        
        AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Width, Height, FilePath, ImageFormatToSave, RawPixels{MoveTemp(RawPixels)}, bApplyGamma, Gamma]()
        {
            TArray<FColor> PixelsToSave = RawPixels;
            if (bApplyGamma && Gamma > 0.0f && !FMath::IsNearlyEqual(Gamma, 1.0f))
            {
                const float InvGamma = 1.0f / Gamma;
                for (FColor& Pixel : PixelsToSave)
                {
                    Pixel.R = FMath::Clamp(FMath::RoundToInt(FMath::Pow(Pixel.R / 255.f, InvGamma) * 255.f), 0, 255);
                    Pixel.G = FMath::Clamp(FMath::RoundToInt(FMath::Pow(Pixel.G / 255.f, InvGamma) * 255.f), 0, 255);
                    Pixel.B = FMath::Clamp(FMath::RoundToInt(FMath::Pow(Pixel.B / 255.f, InvGamma) * 255.f), 0, 255);
                }
                UE_LOG(LogTemp, Log, TEXT("已对图像 %s 应用Gamma校正，Gamma值为: %.2f"), *FilePath, Gamma);
            }

            // 强制所有LDR格式的Alpha通道为255（不透明）
            for (FColor& Pixel : PixelsToSave)
            {
                Pixel.A = 255;
            }

            IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
            EImageFormat EngineImageFormat;
            switch (ImageFormatToSave)
            {
                case ECameraArrayImageFormat::JPEG: EngineImageFormat = EImageFormat::JPEG; break;
                case ECameraArrayImageFormat::BMP:  EngineImageFormat = EImageFormat::BMP; break;
                case ECameraArrayImageFormat::TGA:  EngineImageFormat = EImageFormat::TGA; break;
                case ECameraArrayImageFormat::PNG:
                default:                            EngineImageFormat = EImageFormat::PNG; break;
            }

            TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EngineImageFormat);
            if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(PixelsToSave.GetData(), PixelsToSave.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
            {
                UE_LOG(LogTemp, Error, TEXT("为 %s 编码LDR图像数据失败。"), *FilePath);
                return;
            }
            const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();
            if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
            {
                UE_LOG(LogTemp, Log, TEXT("成功异步保存LDR图像到: %s"), *FilePath);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("保存LDR图像文件失败: %s"), *FilePath);
            }
        });
    }
}

bool ACameraArrayManager::IsHdrFormat() const
{
    return FileFormat == ECameraArrayImageFormat::EXR/* || 
           FileFormat == ECameraArrayImageFormat::TIFF || 
           FileFormat == ECameraArrayImageFormat::HDR*/;
}

FString ACameraArrayManager::GetFileExtension() const
{
    switch (FileFormat)
    {
    case ECameraArrayImageFormat::PNG: return TEXT("png");
    case ECameraArrayImageFormat::JPEG: return TEXT("jpg");
    case ECameraArrayImageFormat::BMP: return TEXT("bmp");
    case ECameraArrayImageFormat::TGA: return TEXT("tga");
    case ECameraArrayImageFormat::EXR: return TEXT("exr");
    //case ECameraArrayImageFormat::TIFF: return TEXT("tiff");
    //case ECameraArrayImageFormat::HDR: return TEXT("hdr");
    default: return TEXT("png");
    }
}

FTransform ACameraArrayManager::GetCameraTransform(int32 CameraIndex) const
{
    FVector Location = StartLocation;
    if (NumCameras > 1)
    {
        // 如果只有一个相机，间距为0，避免除以0
        const float Spacing = (NumCameras > 1) ? (TotalYDistance * 100.0f / (NumCameras - 1)) : 0.0f;
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

void ACameraArrayManager::RenderFirstCamera()
{
    if (bIsTaskRunning) return;
    if (ManagedCameras.Num() <= 0) return;
#if WITH_EDITOR
    SyncShowFlagsWithEditorViewport();
    SyncPostProcessSettings();
#endif
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    
    if (IsHdrFormat())
    {
        ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
    }
    else
    {
        ReusableCaptureComponent->TextureTarget = ReusableLdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }

    PerformSingleCaptureForSpecificIndex(0);
    OpenOutputFolder();
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
            Camera->SetFolderPath(FName(TEXT("CameraArray")));
        }
    }
#endif
}

void ACameraArrayManager::RenderLastCamera()
{
    if (bIsTaskRunning) return;
    if (ManagedCameras.Num() <= 0) return;
#if WITH_EDITOR
    SyncShowFlagsWithEditorViewport();
    SyncPostProcessSettings();
#endif
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    
    if (IsHdrFormat())
    {
        ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
    }
    else
    {
        ReusableCaptureComponent->TextureTarget = ReusableLdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }

    PerformSingleCaptureForSpecificIndex(ManagedCameras.Num() - 1);
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
    // A single, robust check for the essential components.
    if (!GetWorld() || !IsValid(ReusableCaptureComponent) || !IsValid(ReusableLdrRenderTarget) || !IsValid(ReusableHdrRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCaptureForSpecificIndex: World或渲染组件无效!"));
        RenderStatus = TEXT("渲染失败: 内部组件错误");
        bIsTaskRunning = false;
        return;
    }

    if (!ManagedCameras.IsValidIndex(IndexToCapture) || !IsValid(ManagedCameras[IndexToCapture]))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCaptureForSpecificIndex: 要渲染的相机索引 %d 无效!"), IndexToCapture);
        bIsTaskRunning = false;
        return;
    }
    
    AActor* CameraActor = ManagedCameras[IndexToCapture];
    const UCineCameraComponent* CineCamComponent = CameraActor->FindComponentByClass<UCineCameraComponent>();

    if (!CineCamComponent)
    {
        UE_LOG(LogTemp, Error, TEXT("相机 %s 没有CineCameraComponent，无法渲染。"), *CameraActor->GetName());
        bIsTaskRunning = false;
        return;
    }
    
    if (IsHdrFormat())
    {
        ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
    }
    else
    {
        ReusableCaptureComponent->TextureTarget = ReusableLdrRenderTarget;
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }

    const FTransform CameraTransform = CameraActor->GetActorTransform();
    const float CameraRealFOV = CineCamComponent->FieldOfView;
    
    RenderProgress = 50;
    RenderStatus = FString::Printf(TEXT("渲染中... (相机 %s)"), *CameraActor->GetName());

    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraRealFOV;

    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, IndexToCapture, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName, ReusableCaptureComponent->TextureTarget);
    
    FTimerHandle TempHandle;
    GetWorld()->GetTimerManager().SetTimer(TempHandle, [this]()
    {
        RenderProgress = 100;
        RenderStatus = TEXT("渲染完成 (文件后台保存中)");
        bIsTaskRunning = false;
    }, 0.02f, false);
}