#include "CameraArrayManager.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "TimerManager.h"
#include "TextureResource.h"
#include "Engine/PostProcessVolume.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderCore.h"
#include "RenderingThread.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Selection.h"
#include "SEditorViewport.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "HighResScreenshot.h"
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
	// 清理所有定时器以防止内存泄漏
#if WITH_EDITOR
	ClearAllTimers();
#endif
	
	if (ReusableCaptureComponent)
	{
		ReusableCaptureComponent->DestroyComponent();
		ReusableCaptureComponent = nullptr;
	}
	if (ReusableHdrRenderTarget)
	{
		ReusableHdrRenderTarget->MarkAsGarbage();
		ReusableHdrRenderTarget = nullptr;
	}
	if (ReusableLdrRenderTarget)
	{
		ReusableLdrRenderTarget->MarkAsGarbage();
		ReusableLdrRenderTarget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
// 添加清理所有定时器的函数
void ACameraArrayManager::ClearAllTimers()
{
	if (GetWorld())
	{
		FTimerManager& TimerManager = GetWorld()->GetTimerManager();
		
		if (TimerManager.IsTimerActive(ScreenshotTimerHandle))
		{
			TimerManager.ClearTimer(ScreenshotTimerHandle);
		}
		
		if (TimerManager.IsTimerActive(PathTracingLogTimerHandle))
		{
			TimerManager.ClearTimer(PathTracingLogTimerHandle);
		}
		
		if (TimerManager.IsTimerActive(RenderTimerHandle))
		{
			TimerManager.ClearTimer(RenderTimerHandle);
		}
	}
	
	// 重置所有定时器句柄
	ScreenshotTimerHandle.Invalidate();
	PathTracingLogTimerHandle.Invalidate();
	RenderTimerHandle.Invalidate();
}

void ACameraArrayManager::SaveOriginalViewportState()
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("SaveOriginalViewportState: GEditor is not available."));
		return;
	}

	FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
	if (!ViewportClient)
	{
		UE_LOG(LogTemp, Warning, TEXT("SaveOriginalViewportState: Could not get active editor viewport client."));
		return;
	}

	OriginalViewportState.Location = ViewportClient->GetViewLocation();
	OriginalViewportState.Rotation = ViewportClient->GetViewRotation();
	OriginalViewportState.FOV = ViewportClient->ViewFOV;
	OriginalViewportState.bIsRealtime = ViewportClient->IsRealtime();
	OriginalViewportState.bIsInGameView = ViewportClient->IsInGameView();
	OriginalViewportState.ViewportType = ViewportClient->ViewportType;
	OriginalViewportState.bIsValid = true;

	UE_LOG(LogTemp, Log, TEXT("Saved original viewport state: Location(%s), Rotation(%s), FOV(%f)"),
		*OriginalViewportState.Location.ToString(), *OriginalViewportState.Rotation.ToString(), OriginalViewportState.FOV);
}

void ACameraArrayManager::RestoreOriginalViewportState()
{
	if (!OriginalViewportState.bIsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("RestoreOriginalViewportState: No valid original viewport state to restore."));
		return;
	}

	if (!GEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("RestoreOriginalViewportState: GEditor is not available."));
		return;
	}

	FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
	if (!ViewportClient)
	{
		UE_LOG(LogTemp, Warning, TEXT("RestoreOriginalViewportState: Could not get active editor viewport client."));
		return;
	}

	ViewportClient->SetViewLocation(OriginalViewportState.Location);
	ViewportClient->SetViewRotation(OriginalViewportState.Rotation);
	ViewportClient->ViewFOV = OriginalViewportState.FOV;
	ViewportClient->SetRealtime(OriginalViewportState.bIsRealtime);
	ViewportClient->SetGameView(OriginalViewportState.bIsInGameView);
	ViewportClient->ViewportType = OriginalViewportState.ViewportType;
	ViewportClient->Invalidate();

	UE_LOG(LogTemp, Log, TEXT("Restored original viewport state."));
}

void ACameraArrayManager::LockEditorProperties()
{
	bIsRenderingLocked = true;
	// 这个变量会在编辑器中被检测到，从而禁用所有属性编辑
	// 我们将在PostEditChangeProperty中检查这个变量
}

void ACameraArrayManager::UnlockEditorProperties()
{
	bIsRenderingLocked = false;
}

void ACameraArrayManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 如果正在渲染过程中，禁止所有属性修改
	if (bIsRenderingLocked)
	{
		UE_LOG(LogTemp, Warning, TEXT("Property editing is disabled during rendering."));
		return;
	}

	if (bIsTaskRunning)
	{
		return; // 渲染时禁止修改，防止冲突
	}

	const FName MemberPropertyName = (PropertyChangedEvent.Property != nullptr)
		                                 ? PropertyChangedEvent.MemberProperty->GetFName()
		                                 : NAME_None;

	if (MemberPropertyName == NAME_None)
	{
		return;
	}

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
					CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth /
						DesiredAspectRatio;
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
			FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient());
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
		ReusableCaptureComponent->bAlwaysPersistRenderingState = true;
		ReusableCaptureComponent->bUseRayTracingIfEnabled = true;
		ReusableCaptureComponent->RegisterComponentWithWorld(GetWorld());
	}

	// --- LDR Render Target (for PNG, JPG, BMP, TGA) ---
	if (!IsValid(ReusableLdrRenderTarget) || ReusableLdrRenderTarget->SizeX != RenderTargetX || ReusableLdrRenderTarget
		->SizeY != RenderTargetY)
	{
		if (ReusableLdrRenderTarget)
		{
			ReusableLdrRenderTarget->MarkAsGarbage();
		}
		ReusableLdrRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableLdrRenderTarget"));
		ReusableLdrRenderTarget->RenderTargetFormat = RTF_RGBA8;
		ReusableLdrRenderTarget->SizeX = RenderTargetX;
		ReusableLdrRenderTarget->SizeY = RenderTargetY;
		ReusableLdrRenderTarget->bAutoGenerateMips = false;
		ReusableLdrRenderTarget->UpdateResource();
	}

	// --- HDR Render Target (for EXR, TIFF, HDR) ---
	if (!IsValid(ReusableHdrRenderTarget) || ReusableHdrRenderTarget->SizeX != RenderTargetX || ReusableHdrRenderTarget
		->SizeY != RenderTargetY)
	{
		if (ReusableHdrRenderTarget)
		{
			ReusableHdrRenderTarget->MarkAsGarbage();
		}
		ReusableHdrRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableHdrRenderTarget"));
		ReusableHdrRenderTarget->RenderTargetFormat = RTF_RGBA16f;
		ReusableHdrRenderTarget->SizeX = RenderTargetX;
		ReusableHdrRenderTarget->SizeY = RenderTargetY;
		ReusableHdrRenderTarget->bAutoGenerateMips = false;
		ReusableHdrRenderTarget->UpdateResource();
	}
}

void ACameraArrayManager::CreateOrUpdateCameras()
{
	// 在创建新相机前清理现有定时器
#if WITH_EDITOR
	ClearAllTimers();
#endif
	if (bIsTaskRunning)
	{
		return; // 渲染时禁止修改，防止冲突
	}
	
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
		ACineCameraActor* NewCamera = World->SpawnActor<ACineCameraActor>(
			ACineCameraActor::StaticClass(), CameraTransform, SpawnParams);

		if (NewCamera)
		{
			UCineCameraComponent* CineCamComponent = NewCamera->GetCineCameraComponent();
			if (CineCamComponent)
			{
				CineCamComponent->SetFieldOfView(CameraFOV);

				if (RenderTargetY > 0 && RenderTargetX > 0)
				{
					const float DesiredAspectRatio = static_cast<float>(RenderTargetX) / static_cast<float>(
						RenderTargetY);
					CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth /
						DesiredAspectRatio;
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

/*void ACameraArrayManager::RenderAllViews()
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


    ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
    ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;

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

    // Hide all managed cameras from the capture
    ReusableCaptureComponent->HiddenActors.Empty();
    for (AActor* Cam : ManagedCameras)
    {
        if (IsValid(Cam))
        {
            ReusableCaptureComponent->HiddenActors.Add(Cam);
        }
    }
    
    // 检查ShowFlags中是否启用了路径追踪
    const bool bIsPathTracing = ReusableCaptureComponent->ShowFlags.PathTracing;

    if (bIsPathTracing && PathTracingRenderTime > 0.0f)
    {
        // 路径追踪需要时间来累积采样
        UE_LOG(LogTemp, Log, TEXT("已启用路径追踪。相机 %s 将进行 %.2f 秒的累积采样。"), *CameraActor->GetName(), PathTracingRenderTime);

        // 启用每帧捕获以进行累积
        ReusableCaptureComponent->bCaptureEveryFrame = true;

        const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
        const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, CurrentRenderIndex, *GetFileExtension());
        UTextureRenderTarget2D* RenderTargetToSave = ReusableCaptureComponent->TextureTarget;

        // 设置一个计时器，在延迟后停止捕获并继续下一步
        FTimerHandle AccumulationTimerHandle;
        FTimerDelegate TimerDelegate;
        TimerDelegate.BindLambda([this, FullOutputPath, FileName, RenderTargetToSave, CameraName = CameraActor->GetName()]()
        {
            if (IsValid(ReusableCaptureComponent))
            {
                // 停止连续捕获
                ReusableCaptureComponent->bCaptureEveryFrame = false;
                ReusableCaptureComponent-> bAlwaysPersistRenderingState = false;
            }

            UE_LOG(LogTemp, Log, TEXT("相机 %s 的累积采样已完成，正在保存图像。"), *CameraName);
            SaveRenderTargetToFileAsync(FullOutputPath, FileName, RenderTargetToSave);

            // 移动到下一个相机
            CurrentRenderIndex++;
            
            // 调度下一次捕获。使用SetTimerForNextTick以避免深度递归。
            FTimerDelegate NextCaptureDel;
            NextCaptureDel.BindUObject(this, &ACameraArrayManager::PerformSingleCapture);
            GetWorld()->GetTimerManager().SetTimerForNextTick(NextCaptureDel);
        });

        GetWorld()->GetTimerManager().SetTimer(AccumulationTimerHandle, TimerDelegate, PathTracingRenderTime, false);
    }
    else
    {
        // 针对非路径追踪（或零延迟）的标准捕获
        ReusableCaptureComponent->CaptureScene();

        const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
        const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, CurrentRenderIndex, *GetFileExtension());
        
        SaveRenderTargetToFileAsync(FullOutputPath, FileName, ReusableCaptureComponent->TextureTarget);
        
        CurrentRenderIndex++;
        
        // 在下一Tick调度下一次捕-
        FTimerDelegate TimerDel;
        TimerDel.BindUObject(this, &ACameraArrayManager::PerformSingleCapture);
        GetWorld()->GetTimerManager().SetTimerForNextTick(TimerDel);
    }
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
    const bool bSaveAsHdr = IsHdrFormat();

    // 获取底层 RHI 纹理（必须在渲染线程访问）
    FTextureRenderTargetResource* RTResource = RenderTargetToSave->GameThread_GetRenderTargetResource();
    if (!RTResource)
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: 无法获取 RenderTarget 资源"));
        return;
    }

    // 将读取操作放到渲染线程执行（不会阻塞 Game Thread）
    ENQUEUE_RENDER_COMMAND(FCaptureAndReadbackCommand)(
        [RTTexture = RTResource->GetRenderTargetTexture(), Width, Height, FilePath, ImageFormatToSave, bSaveAsHdr](FRHICommandListImmediate& RHICmdList)
        {
            if (!RTTexture)
            {
                UE_LOG(LogTemp, Error, TEXT("FCaptureAndReadbackCommand: RTTexture为空"));
                return;
            }

            // 要读取的矩形
            FIntRect Rect(0, 0, Width, Height);

            // 读取 LDR（FColor） 或 HDR（FLinearColor） 数据到临时容器
            if (bSaveAsHdr)
            {
                TArray<FFloat16Color> Float16Pixels;
                FReadSurfaceDataFlags ReadFlags;
                ReadFlags.SetLinearToGamma(false);

                // 从RHI读取表面数据到Float16Pixels数组
                RHICmdList.ReadSurfaceFloatData(RTTexture, Rect, Float16Pixels, ReadFlags);
                
                TArray<FLinearColor> LinearPixels;
                LinearPixels.SetNumUninitialized(Float16Pixels.Num());
                for (int32 i = 0; i < Float16Pixels.Num(); ++i)
                {
                    LinearPixels[i] = FLinearColor(Float16Pixels[i]);
                }

                // 把转换后的线性像素交给后台线程进行编码和保存
                TArray<FLinearColor> PixelsToSave = MoveTemp(LinearPixels);
                AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [PixelsToSave = MoveTemp(PixelsToSave), FilePath, Width, Height, ImageFormatToSave]() mutable
                {
                    // 强制 alpha = 1
                    TArray<FLinearColor> Local = MoveTemp(PixelsToSave);
                    for (FLinearColor& P : Local) P.A = 1.0f;

                    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
                    EImageFormat EngineImageFormat;
                    switch (ImageFormatToSave)
                    {
                        case ECameraArrayImageFormat::EXR: EngineImageFormat = EImageFormat::EXR; break;
                        default: UE_LOG(LogTemp, Error, TEXT("HDR: 不支持的格式")); return;
                    }

                    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EngineImageFormat);
                    if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw((const void*)Local.GetData(), Local.Num() * sizeof(FLinearColor), Width, Height, ERGBFormat::RGBAF, 32))
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
                // LDR: 读取为 FColor（引擎会在渲染线程做 linear->gamma/tonemap）
                TArray<FColor> OutData;
                FReadSurfaceDataFlags ReadFlags;
                ReadFlags.SetLinearToGamma(true); // 请求引擎在读取时做线性->sRGB转换

                // 从 RHI 读取表面数据到 OutData（在渲染线程）
                RHICmdList.ReadSurfaceData(RTTexture, Rect, OutData, ReadFlags);

                // 将像素数组交给后台线程进行编码与文件写入
                TArray<FColor> PixelsToSave = MoveTemp(OutData);
                AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [PixelsToSave = MoveTemp(PixelsToSave), FilePath, ImageFormatToSave, Width, Height]() mutable
                {
                    TArray<FColor> Local = MoveTemp(PixelsToSave);
                    for (FColor& P : Local) P.A = 255;

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
                    if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw((const void*)Local.GetData(), Local.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
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
    ); // ENQUEUE_RENDER_COMMAND
}*/

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

/*void ACameraArrayManager::RenderFirstCamera()
{
    if (bIsTaskRunning) return;
    if (ManagedCameras.Num() <= 0) return;
#if WITH_EDITOR
    SyncShowFlagsWithEditorViewport();
    SyncPostProcessSettings();
#endif
    bIsTaskRunning = true;
    InitializeCaptureComponents();

    PerformSingleCaptureForSpecificIndex(0);
    OpenOutputFolder();
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

    PerformSingleCaptureForSpecificIndex(ManagedCameras.Num() - 1);
    OpenOutputFolder();
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
    
    ReusableCaptureComponent->TextureTarget = ReusableHdrRenderTarget;
    ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;

    const FTransform CameraTransform = CameraActor->GetActorTransform();
    const float CameraRealFOV = CineCamComponent->FieldOfView;
    
    RenderProgress = 50;
    RenderStatus = FString::Printf(TEXT("渲染中... (相机 %s)"), *CameraActor->GetName());

    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraRealFOV;

    // Hide all managed cameras from the capture
    ReusableCaptureComponent->HiddenActors.Empty();
    for (AActor* Cam : ManagedCameras)
    {
        if (IsValid(Cam))
        {
            ReusableCaptureComponent->HiddenActors.Add(Cam);
        }
    }

    const bool bIsPathTracing = ReusableCaptureComponent->ShowFlags.PathTracing;

    if (bIsPathTracing && PathTracingRenderTime > 0.0f)
    {
        // 1. Enable continuous capture.
        ReusableCaptureComponent->bCaptureEveryFrame = false;

        const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
        const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, IndexToCapture, *GetFileExtension());
        UTextureRenderTarget2D* RenderTargetToSave = ReusableCaptureComponent->TextureTarget;

        UE_LOG(LogTemp, Log, TEXT("Starting 2-second accumulation for Path Tracing for camera %s."), *CameraActor->GetName());

        // 2. Set a timer to stop the capture and save the file after a delay.
        FTimerHandle FinalizeCaptureHandle;
        FTimerDelegate TimerDelegate;
        TimerDelegate.BindLambda([this, FullOutputPath, FileName, RenderTargetToSave, CameraName = CameraActor->GetName()]()
        {
            // 3. Stop continuous capture.
            if (IsValid(ReusableCaptureComponent))
            {
                ReusableCaptureComponent->bCaptureEveryFrame = false;
                ReusableCaptureComponent->bAlwaysPersistRenderingState = false;
            }

            // 4. Save the accumulated result to a file.
            UE_LOG(LogTemp, Log, TEXT("Accumulation finished for %s. Saving image to %s."), *CameraName, *FullOutputPath);
            SaveRenderTargetToFileAsync(FullOutputPath, FileName, RenderTargetToSave);

            // 5. Update status and unlock the task runner.
            RenderProgress = 100;
            RenderStatus = TEXT("Render complete (file saving in background)");
            bIsTaskRunning = false;
        });

        // Hardcoded 2-second delay for path tracing accumulation.
        GetWorld()->GetTimerManager().SetTimer(FinalizeCaptureHandle, TimerDelegate, PathTracingRenderTime, false);
    }
}*/

#if WITH_EDITOR
void ACameraArrayManager::TakeHighResScreenshots()
{
	if (bIsTaskRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakeHighResScreenshots: A task is already running."));
		return;
	}
	if (ManagedCameras.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakeHighResScreenshots: No managed cameras to capture."));
		return;
	}
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("TakeHighResScreenshots: GEditor is not available."));
		return;
	}

	// 在开始新任务前清理所有定时器
	ClearAllTimers();
	
	// 保存原始视口状态
	SaveOriginalViewportState();
	
	// 锁定编辑器属性
	LockEditorProperties();
	
	bIsTaskRunning = true;
	CurrentScreenshotIndex = 0;
	RenderProgress = 0;
	RenderStatus = TEXT("开始高清截图...");
	UE_LOG(LogTemp, Log, TEXT("Starting high-resolution screenshot capture for %d cameras."), ManagedCameras.Num());

	// Start the capture sequence
	TakeNextHighResScreenshot();
}

void ACameraArrayManager::TakeNextHighResScreenshot()
{
	if (CurrentScreenshotIndex > ManagedCameras.Num())
	{
		UE_LOG(LogTemp, Log, TEXT("High-resolution screenshot process completed."));
		GetWorld()->GetTimerManager().ClearTimer(ScreenshotTimerHandle);
		RenderProgress = 100;
		RenderStatus = TEXT("高清截图完成");
		bIsTaskRunning = false;
		
		// 恢复原始视口状态
		RestoreOriginalViewportState();
		
		// 解锁编辑器属性
		UnlockEditorProperties();
		
		return;
	}

	AActor* CameraActor = ManagedCameras[CurrentScreenshotIndex];
	if (!IsValid(CameraActor))
	{
		UE_LOG(LogTemp, Warning, TEXT("Skipping invalid camera at index %d"), CurrentScreenshotIndex);
		CurrentScreenshotIndex++;

		FTimerDelegate NextScreenshotDel;
		NextScreenshotDel.BindUObject(this, &ACameraArrayManager::TakeNextHighResScreenshot);
		GetWorld()->GetTimerManager().SetTimerForNextTick(NextScreenshotDel);
		return;
	}

	FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->
		GetClient());
	if (!ViewportClient)
	{
		UE_LOG(LogTemp, Error, TEXT("Could not get active editor viewport client. Aborting screenshot task."));
		RenderStatus = TEXT("错误：找不到视口");
		bIsTaskRunning = false;
		return;
	}

	// --- 定位视口 ---
	const FTransform CameraTransform = CameraActor->GetActorTransform();
	ViewportClient->SetViewLocation(CameraTransform.GetLocation());
	ViewportClient->SetViewRotation(CameraTransform.GetRotation().Rotator());
	ViewportClient->ViewFOV = CameraFOV;
	ViewportClient->SetGameView(true);
	ViewportClient->SetRealtime(true); // 确保视口实时更新以进行累积
	ViewportClient->ViewportType = LVT_Perspective;

	ViewportClient->Invalidate();

	const bool bIsPathTracing = ViewportClient->EngineShowFlags.PathTracing;
	int32 SamplesPerPixel = 1;
	if (PostProcessVolumeRef)
	{
		SamplesPerPixel = PostProcessVolumeRef->Settings.PathTracingSamplesPerPixel;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("PostProcessVolumeRef is not set. Using default Samples Per Pixel."));
	}

	if (bIsPathTracing)
	{
		const FString CameraLabel = CameraActor->GetActorLabel();
		UE_LOG(LogTemp, Log, TEXT("Starting Path Tracing accumulation for camera %s. Waiting for samples to complete."),
		       *CameraLabel);
		RenderStatus = FString::Printf(TEXT("路径追踪... (%d/%d)"), CurrentScreenshotIndex + 1, ManagedCameras.Num());
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.HighResScreenshotDelay"))->Set(SamplesPerPixel);

		GetWorld()->GetTimerManager().ClearTimer(ScreenshotTimerHandle);

		// Create a timer to periodically check the path tracing progress.
		FTimerDelegate ProgressCheckDelegate;
		ProgressCheckDelegate.BindLambda([this]()
		{
			int32 CurrentSPP, TotalSPP;
			const float Progress = GetPathTracingProgress(CurrentSPP, TotalSPP);
			LogPathTracingProgress();

			// When accumulation is complete, take the screenshot.
			if (Progress >= 1.0f)
			{
				GetWorld()->GetTimerManager().ClearTimer(ScreenshotTimerHandle);
				
				ENQUEUE_RENDER_COMMAND(FHighResScreenshotRenderCommand)(
					[this](FRHICommandListImmediate& RHICmdList)
					{
						AsyncTask(ENamedThreads::GameThread, [this]()
						{
							if (!this || !GetWorld())
							{
								return; // Safety check
							}

							const FString SaveFilename = FString::Printf(
								TEXT("%s_%03d"), *CameraNamePrefix, CurrentScreenshotIndex);

							FHighResScreenshotConfig& HRConfig = GetHighResScreenshotConfig();
							if (IsHdrFormat())
							{
								HRConfig.bCaptureHDR = true;
								const FString HdrFilename = SaveFilename + TEXT(".") + GetFileExtension();
								HRConfig.FilenameOverride = FPaths::Combine(
									FPaths::ProjectSavedDir(), OutputPath, HdrFilename);
								HRConfig.SetResolution(RenderTargetX, RenderTargetY, 1.0f);
								HRConfig.bDumpBufferVisualizationTargets = false;
							}
							else
							{
								HRConfig.bCaptureHDR = false;
								const FString LDRFilename = SaveFilename + TEXT(".") + GetFileExtension();
								HRConfig.FilenameOverride = FPaths::Combine(
									FPaths::ProjectSavedDir(), OutputPath, LDRFilename);
								HRConfig.SetResolution(RenderTargetX, RenderTargetY, 1.0f);
								HRConfig.bDumpBufferVisualizationTargets = false;
							}
							
							CurrentScreenshotIndex++;

							// Schedule the next screenshot for the next tick.
							FTimerDelegate NextScreenshotDel;
							NextScreenshotDel.BindUObject(this, &ACameraArrayManager::TakeNextHighResScreenshot);
							GetWorld()->GetTimerManager().SetTimerForNextTick(NextScreenshotDel);
						});
					}
				);
			}
		});

		GetWorld()->GetTimerManager().SetTimer(ScreenshotTimerHandle, ProgressCheckDelegate, 0.1f, true);
	}
	else
	{
		GetWorld()->GetTimerManager().ClearTimer(ScreenshotTimerHandle);
		
		const FString SaveFilename = FString::Printf(
								TEXT("%s_%03d"), *CameraNamePrefix, CurrentScreenshotIndex);
		FHighResScreenshotConfig& HRConfig = GetHighResScreenshotConfig();
		HRConfig.bCaptureHDR = false;
		const FString LDRFilename = SaveFilename + TEXT(".") + GetFileExtension();
		HRConfig.FilenameOverride = FPaths::Combine(
			FPaths::ProjectSavedDir(), OutputPath, LDRFilename);
		HRConfig.SetResolution(RenderTargetX, RenderTargetY, 1.0f);
		HRConfig.bDumpBufferVisualizationTargets = false;

		// 安排下一次截图
		CurrentScreenshotIndex++;
		FTimerDelegate NextScreenshotDel;
		NextScreenshotDel.BindUObject(this, &ACameraArrayManager::TakeNextHighResScreenshot);
		GetWorld()->GetTimerManager().SetTimer(ScreenshotTimerHandle, NextScreenshotDel, 0.1f, false);
	}
}

void ACameraArrayManager::TakeFirstCameraScreenshot()
{
	if (ManagedCameras.Num() > 0)
	{
		TakeSingleHighResScreenshot(0);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("TakeFirstCameraScreenshot: No managed cameras available to capture."));
	}
}

void ACameraArrayManager::TakeLastCameraScreenshot()
{
	if (ManagedCameras.Num() > 0)
	{
		TakeSingleHighResScreenshot(ManagedCameras.Num() - 1);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("TakeLastCameraScreenshot: No managed cameras available to capture."));
	}
}

void ACameraArrayManager::TakeSingleHighResScreenshot(int32 CameraIndex)
{
	if (bIsTaskRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("TakeSingleHighResScreenshot: A task is already running."));
		return;
	}
	if (!ManagedCameras.IsValidIndex(CameraIndex) || !IsValid(ManagedCameras[CameraIndex]))
	{
		UE_LOG(LogTemp, Error, TEXT("TakeSingleHighResScreenshot: Invalid camera index %d."), CameraIndex);
		return;
	}
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("TakeSingleHighResScreenshot: GEditor is not available."));
		return;
	}

	ClearAllTimers();
	SaveOriginalViewportState();
	LockEditorProperties();
	bIsTaskRunning = true;
	RenderStatus = FString::Printf(TEXT("准备为相机 %d 截图..."), CameraIndex);
	UE_LOG(LogTemp, Log, TEXT("Starting single high-resolution screenshot for camera index %d."), CameraIndex);

	// --- 2. Position the Viewport ---
	AActor* CameraActor = ManagedCameras[CameraIndex];
	FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());

	if (!ViewportClient)
	{
		UE_LOG(LogTemp, Error, TEXT("Could not get active editor viewport client. Aborting."));
		UnlockEditorProperties();
		RestoreOriginalViewportState();
		bIsTaskRunning = false;
		return;
	}
	
	const FTransform CameraTransform = CameraActor->GetActorTransform();
	ViewportClient->SetViewLocation(CameraTransform.GetLocation());
	ViewportClient->SetViewRotation(CameraTransform.GetRotation().Rotator());
	ViewportClient->ViewFOV = CameraFOV;
	ViewportClient->SetGameView(true);
	ViewportClient->SetRealtime(true);
	ViewportClient->ViewportType = LVT_Perspective;
	ViewportClient->Invalidate();

	// --- Configure and Take Screenshot ---
	const bool bIsPathTracing = ViewportClient->EngineShowFlags.PathTracing;

	auto FinalizeScreenshot = [this, CameraIndex]()
	{
		// This lambda contains the logic to configure, take the shot, and clean up.
		const FString SaveFilename = FString::Printf(TEXT("%s_%03d"), *CameraNamePrefix, CameraIndex);

		FHighResScreenshotConfig& HRConfig = GetHighResScreenshotConfig();
		if (IsHdrFormat())
		{
			HRConfig.bCaptureHDR = true;
			const FString HdrFilename = SaveFilename + TEXT(".") + GetFileExtension();
			HRConfig.FilenameOverride = FPaths::Combine(FPaths::ProjectSavedDir(), OutputPath, HdrFilename);
		}
		else
		{
			HRConfig.bCaptureHDR = false;
			const FString LDRFilename = SaveFilename + TEXT(".") + GetFileExtension();
			HRConfig.FilenameOverride = FPaths::Combine(FPaths::ProjectSavedDir(), OutputPath, LDRFilename);
		}
		HRConfig.SetResolution(RenderTargetX, RenderTargetY, 1.0f);
		HRConfig.bDumpBufferVisualizationTargets = false;

		// The screenshot is now configured. The actual capture will happen at the end of the frame.
		// We can now set a timer to restore the state on the next frame to ensure the capture completes.
		FTimerHandle RestoreTimerHandle;
		FTimerDelegate RestoreDelegate;
		RestoreDelegate.BindLambda([this]()
		{
			UE_LOG(LogTemp, Log, TEXT("Single screenshot process completed."));
			RenderStatus = TEXT("完成");
			UnlockEditorProperties();
			RestoreOriginalViewportState();
			bIsTaskRunning = false;
			OpenOutputFolder();
		});
		GetWorld()->GetTimerManager().SetTimer(RestoreTimerHandle, RestoreDelegate, 0.1f, false);
	};

	if (bIsPathTracing)
	{
		RenderStatus = FString::Printf(TEXT("路径追踪... (相机 %d)"), CameraIndex);
		
		// Set a timer to check for path tracing completion
		FTimerDelegate ProgressCheckDelegate;
		ProgressCheckDelegate.BindLambda([this, FinalizeScreenshot]()
		{
			int32 CurrentSPP, TotalSPP;
			const float Progress = GetPathTracingProgress(CurrentSPP, TotalSPP);
			LogPathTracingProgress();

			if (Progress >= 1.0f)
			{
				GetWorld()->GetTimerManager().ClearTimer(ScreenshotTimerHandle);
				FinalizeScreenshot();
			}
		});
		GetWorld()->GetTimerManager().SetTimer(ScreenshotTimerHandle, ProgressCheckDelegate, 0.1f, true);
	}
	else
	{
		RenderStatus = FString::Printf(TEXT("光栅化... (相机 %d)"), CameraIndex);
		FinalizeScreenshot();
	}
}

float ACameraArrayManager::GetPathTracingProgress(int32& CurrentSPP, int32& TotalSPP)
{
	CurrentSPP = 0;
	TotalSPP = 1;
	
	if (!GEditor)
	{
		return 0.0f;
	}

	// 获取当前激活的编辑器视口
	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	if (!ActiveViewport)
	{
		return 0.0f;
	}

	// 将 FViewport 转换为 FEditorViewportClient
	FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(ActiveViewport->GetClient());
	if (!ViewportClient)
	{
		return 0.0f;
	}

	// 检查当前视口是否是路径追踪模式
	const bool bIsPathTracing = ViewportClient->GetViewMode() == VMI_PathTracing;
	if (!bIsPathTracing)
	{
		return 0.0f;
	}

	// 获取视口的世界
	UWorld* World = ViewportClient->GetWorld();
	if (!World || !World->Scene)
	{
		return 0.0f;
	}
	
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			ViewportClient->Viewport,
			World->Scene,
			ViewportClient->EngineShowFlags)
		.SetRealtimeUpdate(ViewportClient->IsRealtime()));
	
	FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);

	if (!View || !View->State)
	{
		return 0.0f;
	}
	const FSceneViewStateInterface* ViewState = View->State;

	// 当前采样数 (SPP)
	CurrentSPP = ViewState->GetPathTracingSampleIndex();

	// 总采样数 (SPP)
	TotalSPP = View->FinalPostProcessSettings.PathTracingSamplesPerPixel;

	if (TotalSPP > 0)
	{
		// 返回进度，范围限制在 0.0 和 1.0 之间
		return FMath::Clamp(static_cast<float>(CurrentSPP) / static_cast<float>(TotalSPP), 0.0f, 1.0f);
	}

	return 0.0f;
}

void ACameraArrayManager::LogPathTracingProgress()
{
	int32 CurrentSPP, TotalSPP;
	// 获取进度
	const float Progress = GetPathTracingProgress(CurrentSPP, TotalSPP);

	// 只有在能获取到有效SPP总数时才打印，避免无效日志
	if (TotalSPP > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("路径追踪累积进度: %d / %d 采样 (%.1f%%)"), CurrentSPP, TotalSPP, Progress * 100.0f);
	}
}

#endif