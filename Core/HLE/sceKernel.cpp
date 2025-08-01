// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Log/LogManager.h"
#include "Common/System/OSD.h"

#include "Core/Core.h"
#include "Core/Config.h"
#include "Core/CwCheat.h"
#include "Core/MemMapHelpers.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSInt.h"
#include "Core/MIPS/JitCommon/JitCommon.h"

#include "Core/FileSystems/FileSystem.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/PSPLoaders.h"
#include "Core/CoreTiming.h"
#include "Core/Reporting.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"

#include "__sceAudio.h"
#include "sceAtrac.h"
#include "sceAudio.h"
#include "sceAudiocodec.h"
#include "sceCcc.h"
#include "sceCtrl.h"
#include "sceDisplay.h"
#include "sceFont.h"
#include "sceGe.h"
#include "sceIo.h"
#include "sceJpeg.h"
#include "sceKernel.h"
#include "sceKernelAlarm.h"
#include "sceKernelHeap.h"
#include "sceKernelInterrupt.h"
#include "sceKernelThread.h"
#include "sceKernelMemory.h"
#include "sceKernelModule.h"
#include "sceKernelMutex.h"
#include "sceKernelMbx.h"
#include "sceKernelMsgPipe.h"
#include "sceKernelSemaphore.h"
#include "sceKernelEventFlag.h"
#include "sceKernelVTimer.h"
#include "sceKernelTime.h"
#include "sceMp3.h"
#include "sceMpeg.h"
#include "sceNet.h"
#include "sceNp.h"
#include "sceNetAdhoc.h"
#include "sceNetAdhocMatching.h"
#include "scePower.h"
#include "sceUtility.h"
#include "sceUmd.h"
#include "sceReg.h"
#include "sceRtc.h"
#include "sceSsl.h"
#include "sceSas.h"
#include "scePsmf.h"
#include "sceImpose.h"
#include "sceUsb.h"
#include "sceUsbGps.h"
#include "sceUsbCam.h"
#include "sceUsbMic.h"
#include "scePspNpDrm_user.h"
#include "sceVaudio.h"
#include "sceHeap.h"
#include "sceDmac.h"
#include "sceMp4.h"
#include "sceAac.h"
#include "sceOpenPSID.h"
#include "sceHttp.h"
#include "Core/Util/PPGeDraw.h"
#include "sceHttp.h"

/*
17: [MIPS32 R4K 00000000 ]: Loader: Type: 1 Vaddr: 00000000 Filesz: 2856816 Memsz: 2856816 
18: [MIPS32 R4K 00000000 ]: Loader: Loadable Segment Copied to 0898dab0, size 002b9770
19: [MIPS32 R4K 00000000 ]: Loader: Type: 1 Vaddr: 002b9770 Filesz: 14964 Memsz: 733156 
20: [MIPS32 R4K 00000000 ]: Loader: Loadable Segment Copied to 08c47220, size 000b2fe4
*/

static bool kernelRunning = false;
KernelObjectPool kernelObjects;
KernelStats kernelStats;
u32 registeredExitCbId;
u32 g_GPOBits;  // Really just 8 bits on the real hardware.
u32 g_GPIBits;  // Really just 8 bits on the real hardware.

void __KernelInit()
{
	if (kernelRunning)
	{
		ERROR_LOG(Log::sceKernel, "Can't init kernel when kernel is running");
		return;
	}
	INFO_LOG(Log::sceKernel, "Initializing kernel...");

	__KernelTimeInit();
	__InterruptsInit();
	__KernelMemoryInit();
	__KernelThreadingInit();
	__KernelAlarmInit();
	__KernelVTimerInit();
	__KernelEventFlagInit();
	__KernelMbxInit();
	__KernelMutexInit();
	__KernelSemaInit();
	__KernelMsgPipeInit();
	__IoInit();
	__JpegInit();
	__AudioInit();
	__Mp3Init();
	__SasInit();
	__AtracInit();
	__CccInit();
	__DisplayInit();
	__GeInit();
	__PowerInit();
	__UtilityInit();
	__UmdInit();
	__MpegInit();
	__PsmfInit();
	__CtrlInit();
	__RtcInit();
	__SslInit();
	__ImposeInit();
	__UsbInit();
	__FontInit();
	__NetInit();
	__NetAdhocInit();
	__NetAdhocMatchingInit();
	__VaudioInit();
	__CheatInit();
	__HeapInit();
	__DmacInit();
	__AudioCodecInit();
	__VideoPmpInit();
	__UsbGpsInit();
	__UsbCamInit();
	__UsbMicInit();
	__OpenPSIDInit();
	__HttpInit();
	__NpInit();
	__RegInit();
	
	SaveState::Init();  // Must be after IO, as it may create a directory
	Reporting::Init();

	// "Internal" PSP libraries
	__PPGeInit();

	kernelRunning = true;
	g_GPOBits = 0;
	INFO_LOG(Log::sceKernel, "Kernel initialized.");
}

void __KernelShutdown()
{
	if (!kernelRunning) {
		INFO_LOG(Log::sceKernel, "Can't shut down kernel - not running");
		return;
	}
	kernelObjects.List();
	INFO_LOG(Log::sceKernel, "Shutting down kernel - %i kernel objects alive", kernelObjects.GetCount());
	hleCurrentThreadName = NULL;
	kernelObjects.Clear();

	__RegShutdown();
	__HttpShutdown();
	__OpenPSIDShutdown();
	__UsbCamShutdown();
	__UsbMicShutdown();
	__UsbGpsShutdown();

	__AudioCodecShutdown();
	__VideoPmpShutdown();
	__AACShutdown();
	__NetAdhocShutdown();
	__NetAdhocMatchingShutdown();
	__NetShutdown();
	__FontShutdown();

	__Mp3Shutdown();
	__MpegShutdown();
	__PsmfShutdown();
	__PPGeShutdown();

	__CtrlShutdown();
	__UtilityShutdown();
	__GeShutdown();
	__SasShutdown();
	__DisplayShutdown();
	__AtracShutdown();
	__AudioShutdown();
	__IoShutdown();
	__HeapShutdown();
	__KernelMutexShutdown();
	__KernelThreadingShutdown();
	__KernelMemoryShutdown();
	__InterruptsShutdown();
	__CheatShutdown();
	__KernelModuleShutdown();

	CoreTiming::ClearPendingEvents();
	CoreTiming::UnregisterAllEvents();
	Reporting::Shutdown();
	SaveState::Shutdown();

	kernelRunning = false;
}

void __KernelDoState(PointerWrap &p)
{
	{
		auto s = p.Section("Kernel", 1, 2);
		if (!s)
			return;

		Do(p, kernelRunning);
		kernelObjects.DoState(p);

		if (s >= 2)
			Do(p, registeredExitCbId);
	}

	{
		auto s = p.Section("Kernel Modules", 1);
		if (!s)
			return;

		__InterruptsDoState(p);
		// Memory needs to be after kernel objects, which may free kernel memory.
		__KernelMemoryDoState(p);
		__KernelThreadingDoState(p);
		__KernelAlarmDoState(p);
		__KernelVTimerDoState(p);
		__KernelEventFlagDoState(p);
		__KernelMbxDoState(p);
		__KernelModuleDoState(p);
		__KernelMsgPipeDoState(p);
		__KernelMutexDoState(p);
		__KernelSemaDoState(p);
		__KernelTimeDoState(p);
	}

	{
		auto s = p.Section("HLE Modules", 1);
		if (!s)
			return;

		__AtracDoState(p);
		__AudioDoState(p);
		__CccDoState(p);
		__CtrlDoState(p);
		__DisplayDoState(p);
		__FontDoState(p);
		__GeDoState(p);
		__ImposeDoState(p);
		__IoDoState(p);
		__JpegDoState(p);
		__Mp3DoState(p);
		__MpegDoState(p);
		__NetDoState(p);
		__NetAdhocDoState(p);
		__PowerDoState(p);
		__PsmfDoState(p);
		__PsmfPlayerDoState(p);
		__RtcDoState(p);
		__SasDoState(p);
		__SslDoState(p);
		__UmdDoState(p);
		__UtilityDoState(p);
		__UsbDoState(p);
		__VaudioDoState(p);
		__HeapDoState(p);

		__PPGeDoState(p);
		__CheatDoState(p);
		__sceAudiocodecDoState(p);
		__VideoPmpDoState(p);
		__AACDoState(p);
		__UsbGpsDoState(p);
		__UsbMicDoState(p);
		__RegDoState(p);

		// IMPORTANT! Add new sections last!
	}

	{
		auto s = p.Section("Kernel Cleanup", 1);
		if (!s)
			return;

		__InterruptsDoStateLate(p);
		__KernelThreadingDoStateLate(p);
		Reporting::DoState(p);
	}
}

bool __KernelIsRunning() {
	return kernelRunning;
}

std::string __KernelStateSummary() {
	return __KernelThreadingSummary();
}

void sceKernelExitGame() {
	INFO_LOG(Log::sceKernel, "sceKernelExitGame");
	__KernelSwitchOffThread("game exited");
	Core_Stop();

	g_OSD.Show(OSDType::MESSAGE_INFO, "sceKernelExitGame()", 0.0f, "kernelexit");
	hleNoLogVoid();
}

void sceKernelExitGameWithStatus()
{
	INFO_LOG(Log::sceKernel, "sceKernelExitGameWithStatus");
	__KernelSwitchOffThread("game exited");
	Core_Stop();

	g_OSD.Show(OSDType::MESSAGE_INFO, "sceKernelExitGameWithStatus()");
	hleNoLogVoid();
}

u32 sceKernelDevkitVersion()
{
	int firmwareVersion = g_Config.iFirmwareVersion;
	int major = firmwareVersion / 100;
	int minor = (firmwareVersion / 10) % 10;
	int revision = firmwareVersion % 10;
	int devkitVersion = (major << 24) | (minor << 16) | (revision << 8) | 0x10;

	return hleLogDebug(Log::sceKernel, devkitVersion, "%d.%d.%d", major, minor, revision);
}

u32 sceKernelRegisterKprintfHandler() {
	return hleLogWarning(Log::sceKernel, 0, "UNIMPL");
}

int sceKernelRegisterDefaultExceptionHandler() {
	return hleLogWarning(Log::sceKernel, 0, "UNIMPL");
}

void sceKernelSetGPO(u32 ledBits) {
	// Sets debug LEDs. Some games do interesting stuff with this, like a metronome in Parappa.
	// Shows up as a vertical strip of LEDs at the side of the screen, if enabled.
	g_GPOBits = ledBits;
	DEBUG_LOG(Log::sceKernel, "sceKernelSetGPO: %08x", ledBits);
	return hleNoLogVoid();
}

u32 sceKernelGetGPI() {
	// Always returns 0 on production systems.
	// On developer systems, there are 8 switches that control the lower 8 bits of the return value.
	return hleLogDebug(Log::sceKernel, g_GPIBits);
}

// #define LOG_CACHE

// Don't even log these by default, they're spammy and we probably won't
// need to emulate them. Useful for invalidating cached textures though,
// and in the future display lists (although hashing takes care of those
// for now).
int sceKernelDcacheInvalidateRange(u32 addr, int size)
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU,"sceKernelDcacheInvalidateRange(%08x, %i)", addr, size);
#endif
	if (size < 0 || (int) addr + size < 0)
		return hleNoLog(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

	if (size > 0)
	{
		if ((addr % 64) != 0 || (size % 64) != 0)
			return hleNoLog(SCE_KERNEL_ERROR_CACHE_ALIGNMENT);

		if (addr != 0)
			gpu->InvalidateCache(addr, size, GPU_INVALIDATE_HINT);
	}
	hleEatCycles(190);
	return hleNoLog(0);
}

int sceKernelIcacheInvalidateRange(u32 addr, int size) {
	if (size != 0)
		currentMIPS->InvalidateICache(addr, size);
	return hleLogDebug(Log::CPU, 0);
}

int sceKernelDcacheWritebackAll()
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU,"sceKernelDcacheWritebackAll()");
#endif
	// Some games seem to use this a lot, it doesn't make sense
	// to zap the whole texture cache.
	gpu->InvalidateCache(0, -1, GPU_INVALIDATE_ALL);
	hleEatCycles(3524);
	hleReSchedule("dcache writeback all");
	return hleNoLog(0);
}

int sceKernelDcacheWritebackRange(u32 addr, int size)
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU,"sceKernelDcacheWritebackRange(%08x, %i)", addr, size);
#endif
	if (size < 0)
		return hleLogError(Log::sceKernel, SCE_KERNEL_ERROR_INVALID_SIZE);

	if (size > 0 && addr != 0) {
		gpu->InvalidateCache(addr, size, GPU_INVALIDATE_HINT);
	}
	hleEatCycles(165);
	return hleNoLog(0);
}

int sceKernelDcacheWritebackInvalidateRange(u32 addr, int size)
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU,"sceKernelDcacheInvalidateRange(%08x, %i)", addr, size);
#endif
	if (size < 0)
		return hleLogError(Log::sceKernel, SCE_KERNEL_ERROR_INVALID_SIZE);

	if (size > 0 && addr != 0) {
		gpu->InvalidateCache(addr, size, GPU_INVALIDATE_HINT);
	}
	hleEatCycles(165);
	return hleNoLog(0);
}

int sceKernelDcacheWritebackInvalidateAll()
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU,"sceKernelDcacheInvalidateAll()");
#endif
	gpu->InvalidateCache(0, -1, GPU_INVALIDATE_ALL);
	hleEatCycles(1165);
	hleReSchedule("dcache invalidate all");
	return hleLogDebug(Log::CPU, 0, "Dcache invalidated");
}

u32 sceKernelIcacheInvalidateAll()
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU, "Icache invalidated - should clear JIT someday");
#endif
	// Note that this doesn't actually fully invalidate all with such a large range.
	currentMIPS->InvalidateICache(0, 0x3FFFFFFF);
	return hleLogDebug(Log::CPU, 0, "Icache invalidated");
}

u32 sceKernelIcacheClearAll()
{
#ifdef LOG_CACHE
	NOTICE_LOG(Log::CPU, "Icache cleared - should clear JIT someday");
#endif
	// Note that this doesn't actually fully invalidate all with such a large range.
	currentMIPS->InvalidateICache(0, 0x3FFFFFFF);
	return hleLogDebug(Log::CPU, 0, "Icache cleared");
}

void KernelObject::GetQuickInfo(char *ptr, int size) {
	strcpy(ptr, "-");
}

void KernelObject::GetLongInfo(char *ptr, int size) const {
	strcpy(ptr, "-");
}

KernelObjectPool::KernelObjectPool() {
	memset(occupied, 0, sizeof(bool)*maxCount);
	nextID = initialNextID;
}

SceUID KernelObjectPool::Create(KernelObject *obj, int rangeBottom, int rangeTop) {
	if (rangeTop > maxCount)
		rangeTop = maxCount;
	if (nextID >= rangeBottom && nextID < rangeTop)
		rangeBottom = nextID++;

	for (int i = rangeBottom; i < rangeTop; i++) {
		if (!occupied[i]) {
			occupied[i] = true;
			pool[i] = obj;
			pool[i]->uid = i + handleOffset;
			return i + handleOffset;
		}
	}

	ERROR_LOG_REPORT(Log::sceKernel, "Unable to allocate kernel object, too many objects slots in use.");
	return 0;
}

void KernelObjectPool::Clear() {
	for (int i = 0; i < maxCount; i++) {
		// brutally clear everything, no validation
		if (occupied[i])
			delete pool[i];
		pool[i] = nullptr;
		occupied[i] = false;
	}
	nextID = initialNextID;
}

void KernelObjectPool::List() {
	for (int i = 0; i < maxCount; i++) {
		if (occupied[i]) {
			char buffer[256];
			if (pool[i]) {
				pool[i]->GetQuickInfo(buffer, sizeof(buffer));
				DEBUG_LOG(Log::sceKernel, "KO %i: %s \"%s\": %s", i + handleOffset, pool[i]->GetTypeName(), pool[i]->GetName(), buffer);
			} else {
				ERROR_LOG(Log::sceKernel, "KO %i: bad object", i + handleOffset);
			}
		}
	}
}

int KernelObjectPool::GetCount() const {
	int count = 0;
	for (int i = 0; i < maxCount; i++) {
		if (occupied[i])
			count++;
	}
	return count;
}

void KernelObjectPool::DoState(PointerWrap &p) {
	auto s = p.Section("KernelObjectPool", 1);
	if (!s)
		return;

	int _maxCount = maxCount;
	Do(p, _maxCount);

	if (_maxCount != maxCount) {
		p.SetError(p.ERROR_FAILURE);
		ERROR_LOG(Log::sceKernel, "Unable to load state: different kernel object storage.");
		return;
	}

	if (p.mode == p.MODE_READ) {
		hleCurrentThreadName = nullptr;
		kernelObjects.Clear();
	}

	Do(p, nextID);
	DoArray(p, occupied, maxCount);
	for (int i = 0; i < maxCount; ++i) {
		if (!occupied[i])
			continue;

		int type;
		if (p.mode == p.MODE_READ) {
			Do(p, type);
			pool[i] = CreateByIDType(type);

			// Already logged an error.
			if (pool[i] == nullptr)
				return;

			pool[i]->uid = i + handleOffset;
		} else {
			type = pool[i]->GetIDType();
			Do(p, type);
		}
		pool[i]->DoState(p);
		if (p.error >= p.ERROR_FAILURE)
			break;
	}
}

KernelObject *KernelObjectPool::CreateByIDType(int type) {
	// Used for save states.  This is ugly, but what other way is there?
	switch (type) {
	case SCE_KERNEL_TMID_Alarm:
		return __KernelAlarmObject();
	case SCE_KERNEL_TMID_EventFlag:
		return __KernelEventFlagObject();
	case SCE_KERNEL_TMID_Mbox:
		return __KernelMbxObject();
	case SCE_KERNEL_TMID_Fpl:
		return __KernelMemoryFPLObject();
	case SCE_KERNEL_TMID_Vpl:
		return __KernelMemoryVPLObject();
	case PPSSPP_KERNEL_TMID_PMB:
		return __KernelMemoryPMBObject();
	case PPSSPP_KERNEL_TMID_Module:
		return __KernelModuleObject();
	case SCE_KERNEL_TMID_Mpipe:
		return __KernelMsgPipeObject();
	case SCE_KERNEL_TMID_Mutex:
		return __KernelMutexObject();
	case SCE_KERNEL_TMID_LwMutex:
		return __KernelLwMutexObject();
	case SCE_KERNEL_TMID_Semaphore:
		return __KernelSemaphoreObject();
	case SCE_KERNEL_TMID_Callback:
		return __KernelCallbackObject();
	case SCE_KERNEL_TMID_Thread:
		return __KernelThreadObject();
	case SCE_KERNEL_TMID_VTimer:
		return __KernelVTimerObject();
	case SCE_KERNEL_TMID_Tlspl:
	case SCE_KERNEL_TMID_Tlspl_v0:
		return __KernelTlsplObject();
	case PPSSPP_KERNEL_TMID_File:
		return __KernelFileNodeObject();
	case PPSSPP_KERNEL_TMID_DirList:
		return __KernelDirListingObject();
	case SCE_KERNEL_TMID_ThreadEventHandler:
		return __KernelThreadEventHandlerObject();

	default:
		ERROR_LOG(Log::SaveState, "Unable to load state: could not find object type %d.", type);
		return NULL;
	}
}

struct SystemStatus {
	SceSize_le size;
	SceUInt_le status;
	SceUInt_le clockPart1;
	SceUInt_le clockPart2;
	SceUInt_le perfcounter1;
	SceUInt_le perfcounter2;
	SceUInt_le perfcounter3;
};

static int sceKernelReferSystemStatus(u32 statusPtr) {
	auto status = PSPPointer<SystemStatus>::Create(statusPtr);
	if (status.IsValid()) {
		memset((SystemStatus *)status, 0, sizeof(SystemStatus));
		status->size = sizeof(SystemStatus);
		// TODO: Fill in the struct!
		status.NotifyWrite("SystemStatus");
	}
	return hleLogDebug(Log::sceKernel, 0);
}

// Unused - believed to be the returned struct from sceKernelReferThreadProfiler.
struct DebugProfilerRegs {
	u32 enable;
	u32 systemck;
	u32 cpuck;
	u32 internal;
	u32 memory;
	u32 copz;
	u32 vfpu;
	u32 sleep;
	u32 bus_access;
	u32 uncached_load;
	u32 uncached_store;
	u32 cached_load;
	u32 cached_store;
	u32 i_miss;
	u32 d_miss;
	u32 d_writeback;
	u32 cop0_inst;
	u32 fpu_inst;
	u32 vfpu_inst;
	u32 local_bus;
};

static u32 sceKernelReferThreadProfiler() {
	// This seems to simply has no parameter:
	// https://pspdev.github.io/pspsdk/group__ThreadMan.html#ga8fd30da51b9dc0507ac4dae04a7e4a17
	// In testing it just returns null in around 140-150 cycles.  See issue #17623.
	hleEatCycles(140);
	return hleLogDebug(Log::sceKernel, 0);
}

static int sceKernelReferGlobalProfiler() {
	// See sceKernelReferThreadProfiler(), similar.
	hleEatCycles(140);
	return hleLogDebug(Log::sceKernel, 0);
}

const HLEFunction ThreadManForUser[] =
{
	{0X55C20A00, &WrapI_CUUU<sceKernelCreateEventFlag>,              "sceKernelCreateEventFlag",                  'i', "sxxx"    },
	{0X812346E4, &WrapU_IU<sceKernelClearEventFlag>,                 "sceKernelClearEventFlag",                   'x', "ix"      },
	{0XEF9E4C70, &WrapU_I<sceKernelDeleteEventFlag>,                 "sceKernelDeleteEventFlag",                  'x', "i"       },
	{0X1FB15A32, &WrapU_IU<sceKernelSetEventFlag>,                   "sceKernelSetEventFlag",                     'x', "ix"      },
	{0X402FCF22, &WrapI_IUUUU<sceKernelWaitEventFlag>,               "sceKernelWaitEventFlag",                    'i', "ixxpp",  HLE_NOT_IN_INTERRUPT },
	{0X328C546A, &WrapI_IUUUU<sceKernelWaitEventFlagCB>,             "sceKernelWaitEventFlagCB",                  'i', "ixxpp",  HLE_NOT_IN_INTERRUPT },
	{0X30FD48F0, &WrapI_IUUU<sceKernelPollEventFlag>,                "sceKernelPollEventFlag",                    'i', "ixxp"    },
	{0XCD203292, &WrapU_IUU<sceKernelCancelEventFlag>,               "sceKernelCancelEventFlag",                  'x', "ixp"     },
	{0XA66B0120, &WrapU_IU<sceKernelReferEventFlagStatus>,           "sceKernelReferEventFlagStatus",             'x', "ix"      },

	{0X8FFDF9A2, &WrapI_IIU<sceKernelCancelSema>,                    "sceKernelCancelSema",                       'i', "iix"     },
	{0XD6DA4BA1, &WrapI_CUIIU<sceKernelCreateSema>,                  "sceKernelCreateSema",                       'i', "sxiip"   },
	{0X28B6489C, &WrapI_I<sceKernelDeleteSema>,                      "sceKernelDeleteSema",                       'i', "i"       },
	{0X58B1F937, &WrapI_II<sceKernelPollSema>,                       "sceKernelPollSema",                         'i', "ii"      },
	{0XBC6FEBC5, &WrapI_IU<sceKernelReferSemaStatus>,                "sceKernelReferSemaStatus",                  'i', "ip"      },
	{0X3F53E640, &WrapI_II<sceKernelSignalSema>,                     "sceKernelSignalSema",                       'i', "ii"      },
	{0X4E3A1105, &WrapI_IIU<sceKernelWaitSema>,                      "sceKernelWaitSema",                         'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X6D212BAC, &WrapI_IIU<sceKernelWaitSemaCB>,                    "sceKernelWaitSemaCB",                       'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },

	{0X60107536, &WrapI_U<sceKernelDeleteLwMutex>,                   "sceKernelDeleteLwMutex",                    'i', "x"       },
	{0X19CFF145, &WrapI_UCUIU<sceKernelCreateLwMutex>,               "sceKernelCreateLwMutex",                    'i', "xsxix"   },
	{0X4C145944, &WrapI_IU<sceKernelReferLwMutexStatusByID>,         "sceKernelReferLwMutexStatusByID",           'i', "xp"      },
	// NOTE: LockLwMutex, UnlockLwMutex, and ReferLwMutexStatus are in Kernel_Library, see sceKernelInterrupt.cpp.
	// The below should not be called directly.
	//{0x71040D5C, nullptr,                                            "_sceKernelTryLockLwMutex",                  '?', ""        },
	//{0x7CFF8CF3, nullptr,                                            "_sceKernelLockLwMutex",                     '?', ""        },
	//{0x31327F19, nullptr,                                            "_sceKernelLockLwMutexCB",                   '?', ""        },
	//{0xBEED3A47, nullptr,                                            "_sceKernelUnlockLwMutex",                   '?', ""        },

	{0XF8170FBE, &WrapI_I<sceKernelDeleteMutex>,                     "sceKernelDeleteMutex",                      'i', "i"       },
	{0XB011B11F, &WrapI_IIU<sceKernelLockMutex>,                     "sceKernelLockMutex",                        'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X5BF4DD27, &WrapI_IIU<sceKernelLockMutexCB>,                   "sceKernelLockMutexCB",                      'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X6B30100F, &WrapI_II<sceKernelUnlockMutex>,                    "sceKernelUnlockMutex",                      'i', "ii"      },
	{0XB7D098C6, &WrapI_CUIU<sceKernelCreateMutex>,                  "sceKernelCreateMutex",                      'i', "sxip"    },
	{0X0DDCD2C9, &WrapI_II<sceKernelTryLockMutex>,                   "sceKernelTryLockMutex",                     'i', "ii"      },
	{0XA9C2CB9A, &WrapI_IU<sceKernelReferMutexStatus>,               "sceKernelReferMutexStatus",                 'i', "ip"      },
	{0X87D9223C, &WrapI_IIU<sceKernelCancelMutex>,                   "sceKernelCancelMutex",                      'i', "iix"     },

	{0XFCCFAD26, &WrapI_I<sceKernelCancelWakeupThread>,              "sceKernelCancelWakeupThread",               'i', "i"       },
	{0X1AF94D03, nullptr,                                            "sceKernelDonateWakeupThread",               '?', ""        },
	{0XEA748E31, &WrapI_UU<sceKernelChangeCurrentThreadAttr>,        "sceKernelChangeCurrentThreadAttr",          'i', "xx"      },
	{0X71BC9871, &WrapI_II<sceKernelChangeThreadPriority>,           "sceKernelChangeThreadPriority",             'i', "ii"      },
	{0X446D8DE6, &WrapI_CUUIUU<sceKernelCreateThread>,               "sceKernelCreateThread",                     'i', "sxxixx", HLE_NOT_IN_INTERRUPT },
	{0X9FA03CD3, &WrapI_I<sceKernelDeleteThread>,                    "sceKernelDeleteThread",                     'i', "i"       },
	{0XBD123D9E, &WrapI_U<sceKernelDelaySysClockThread>,             "sceKernelDelaySysClockThread",              'i', "P",      HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X1181E963, &WrapI_U<sceKernelDelaySysClockThreadCB>,           "sceKernelDelaySysClockThreadCB",            'i', "P",      HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XCEADEB47, &WrapI_U<sceKernelDelayThread>,                     "sceKernelDelayThread",                      'i', "x",      HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X68DA9E36, &WrapI_U<sceKernelDelayThreadCB>,                   "sceKernelDelayThreadCB",                    'i', "x",      HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XAA73C935, &WrapI_I<sceKernelExitThread>,                      "sceKernelExitThread",                       'i', "i"       },
	{0X809CE29B, &WrapI_I<sceKernelExitDeleteThread>,                "sceKernelExitDeleteThread",                 'i', "i"       },
	{0x94aa61ee, &WrapI_V<sceKernelGetThreadCurrentPriority>,        "sceKernelGetThreadCurrentPriority",         'i', ""        },
	{0X293B45B8, &WrapI_V<sceKernelGetThreadId>,                     "sceKernelGetThreadId",                      'i', "",       HLE_NOT_IN_INTERRUPT },
	{0X3B183E26, &WrapI_I<sceKernelGetThreadExitStatus>,             "sceKernelGetThreadExitStatus",              'i', "i"       },
	{0X52089CA1, &WrapI_I<sceKernelGetThreadStackFreeSize>,          "sceKernelGetThreadStackFreeSize",           'i', "i"       },
	{0XFFC36A14, &WrapU_UU<sceKernelReferThreadRunStatus>,           "sceKernelReferThreadRunStatus",             'x', "xx"      },
	{0X17C1684E, &WrapU_UU<sceKernelReferThreadStatus>,              "sceKernelReferThreadStatus",                'i', "xp"      },
	{0X2C34E053, &WrapI_I<sceKernelReleaseWaitThread>,               "sceKernelReleaseWaitThread",                'i', "i"       },
	{0X75156E8F, &WrapI_I<sceKernelResumeThread>,                    "sceKernelResumeThread",                     'i', "i"       },
	{0X3AD58B8C, &WrapU_V<sceKernelSuspendDispatchThread>,           "sceKernelSuspendDispatchThread",            'x', "",       HLE_NOT_IN_INTERRUPT },
	{0X27E22EC2, &WrapU_U<sceKernelResumeDispatchThread>,            "sceKernelResumeDispatchThread",             'x', "x",      HLE_NOT_IN_INTERRUPT },
	{0X912354A7, &WrapI_I<sceKernelRotateThreadReadyQueue>,          "sceKernelRotateThreadReadyQueue",           'i', "i"       },
	{0X9ACE131E, &WrapI_V<sceKernelSleepThread>,                     "sceKernelSleepThread",                      'i', "",       HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X82826F70, &WrapI_V<sceKernelSleepThreadCB>,                   "sceKernelSleepThreadCB",                    'i', "",       HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XF475845D, &WrapI_IIU<sceKernelStartThread>,                   "sceKernelStartThread",                      'i', "iix",    HLE_NOT_IN_INTERRUPT },
	{0X9944F31F, &WrapI_I<sceKernelSuspendThread>,                   "sceKernelSuspendThread",                    'i', "i"       },
	{0X616403BA, &WrapI_I<sceKernelTerminateThread>,                 "sceKernelTerminateThread",                  'i', "i"       },
	{0X383F7BCC, &WrapI_I<sceKernelTerminateDeleteThread>,           "sceKernelTerminateDeleteThread",            'i', "i"       },
	{0X840E8133, &WrapI_IU<sceKernelWaitThreadEndCB>,                "sceKernelWaitThreadEndCB",                  'i', "ix"      },
	{0XD13BDE95, &WrapI_V<sceKernelCheckThreadStack>,                "sceKernelCheckThreadStack",                 'i', ""        },

	{0X94416130, &WrapU_UUUU<sceKernelGetThreadmanIdList>,           "sceKernelGetThreadmanIdList",               'x', "xxxx"    },
	{0X57CF62DD, &WrapU_U<sceKernelGetThreadmanIdType>,              "sceKernelGetThreadmanIdType",               'x', "x"       },
	{0XBC80EC7C, &WrapU_UUU<sceKernelExtendThreadStack>,             "sceKernelExtendThreadStack",                'x', "xxx"     },
	// NOTE: Takes a UID from sceKernelMemory's AllocMemoryBlock and seems thread stack related.
	//{0x28BFD974, nullptr,                                           "ThreadManForUser_28BFD974",                  '?', ""        },

	{0X82BC5777, &WrapU64_V<sceKernelGetSystemTimeWide>,             "sceKernelGetSystemTimeWide",                'X', ""        },
	{0XDB738F35, &WrapI_U<sceKernelGetSystemTime>,                   "sceKernelGetSystemTime",                    'i', "x"       },
	{0X369ED59D, &WrapU_V<sceKernelGetSystemTimeLow>,                "sceKernelGetSystemTimeLow",                 'x', ""        },

	{0X8218B4DD, &WrapI_V<sceKernelReferGlobalProfiler>,             "sceKernelReferGlobalProfiler",              'i', ""       },
	{0X627E6F3A, &WrapI_U<sceKernelReferSystemStatus>,               "sceKernelReferSystemStatus",                'i', "x"       },
	{0X64D4540E, &WrapU_V<sceKernelReferThreadProfiler>,             "sceKernelReferThreadProfiler",              'x', ""       },

	//Fifa Street 2 uses alarms
	{0X6652B8CA, &WrapI_UUU<sceKernelSetAlarm>,                      "sceKernelSetAlarm",                         'i', "xxx"     },
	{0XB2C25152, &WrapI_UUU<sceKernelSetSysClockAlarm>,              "sceKernelSetSysClockAlarm",                 'i', "xxx"     },
	{0X7E65B999, &WrapI_I<sceKernelCancelAlarm>,                     "sceKernelCancelAlarm",                      'i', "i"       },
	{0XDAA3F564, &WrapI_IU<sceKernelReferAlarmStatus>,               "sceKernelReferAlarmStatus",                 'i', "ix"      },

	{0XBA6B92E2, &WrapI_UUU<sceKernelSysClock2USec>,                 "sceKernelSysClock2USec",                    'i', "xxx"     },
	{0X110DEC9A, &WrapI_UU<sceKernelUSec2SysClock>,                  "sceKernelUSec2SysClock",                    'i', "xx"      },
	{0XC8CD158C, &WrapU64_U<sceKernelUSec2SysClockWide>,             "sceKernelUSec2SysClockWide",                'X', "x"       },
	{0XE1619D7C, &WrapI_UUUU<sceKernelSysClock2USecWide>,            "sceKernelSysClock2USecWide",                'i', "xxxx"    },

	{0X278C0DF5, &WrapI_IU<sceKernelWaitThreadEnd>,                  "sceKernelWaitThreadEnd",                    'i', "ix"      },
	{0XD59EAD2F, &WrapI_I<sceKernelWakeupThread>,                    "sceKernelWakeupThread",                     'i', "i"       }, //AI Go, audio?

	{0x0C106E53, &WrapI_CIUUU<sceKernelRegisterThreadEventHandler>,  "sceKernelRegisterThreadEventHandler",       'i', "sixxx",  },
	{0x72F3C145, &WrapI_I<sceKernelReleaseThreadEventHandler>,       "sceKernelReleaseThreadEventHandler",        'i', "i"       },
	{0x369EEB6B, &WrapI_IU<sceKernelReferThreadEventHandlerStatus>,  "sceKernelReferThreadEventHandlerStatus",    'i', "ip"      },

	{0x349d6d6c, &sceKernelCheckCallback,                            "sceKernelCheckCallback",                    'i', ""        },
	{0XE81CAF8F, &WrapI_CUU<sceKernelCreateCallback>,                "sceKernelCreateCallback",                   'i', "sxx"     },
	{0XEDBA5844, &WrapI_I<sceKernelDeleteCallback>,                  "sceKernelDeleteCallback",                   'i', "i"       },
	{0XC11BA8C4, &WrapI_II<sceKernelNotifyCallback>,                 "sceKernelNotifyCallback",                   'i', "ii"      },
	{0XBA4051D6, &WrapI_I<sceKernelCancelCallback>,                  "sceKernelCancelCallback",                   'i', "i"       },
	{0X2A3D44FF, &WrapI_I<sceKernelGetCallbackCount>,                "sceKernelGetCallbackCount",                 'i', "i"       },
	{0X730ED8BC, &WrapI_IU<sceKernelReferCallbackStatus>,            "sceKernelReferCallbackStatus",              'i', "ip"      },

	{0X8125221D, &WrapI_CUU<sceKernelCreateMbx>,                     "sceKernelCreateMbx",                        'i', "sxx"     },
	{0X86255ADA, &WrapI_I<sceKernelDeleteMbx>,                       "sceKernelDeleteMbx",                        'i', "i"       },
	{0XE9B3061E, &WrapI_IU<sceKernelSendMbx>,                        "sceKernelSendMbx",                          'i', "ix"      },
	{0X18260574, &WrapI_IUU<sceKernelReceiveMbx>,                    "sceKernelReceiveMbx",                       'i', "ixx",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XF3986382, &WrapI_IUU<sceKernelReceiveMbxCB>,                  "sceKernelReceiveMbxCB",                     'i', "ixx",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X0D81716A, &WrapI_IU<sceKernelPollMbx>,                        "sceKernelPollMbx",                          'i', "ix"      },
	{0X87D4DD36, &WrapI_IU<sceKernelCancelReceiveMbx>,               "sceKernelCancelReceiveMbx",                 'i', "ix"      },
	{0XA8E8C846, &WrapI_IU<sceKernelReferMbxStatus>,                 "sceKernelReferMbxStatus",                   'i', "ip"      },

	{0X7C0DC2A0, &WrapI_CIUUU<sceKernelCreateMsgPipe>,               "sceKernelCreateMsgPipe",                    'i', "sixxp"   },
	{0XF0B7DA1C, &WrapI_I<sceKernelDeleteMsgPipe>,                   "sceKernelDeleteMsgPipe",                    'i', "i"       },
	{0X876DBFAD, &WrapI_IUUUUU<sceKernelSendMsgPipe>,                "sceKernelSendMsgPipe",                      'i', "ixxxxx"  },
	{0X7C41F2C2, &WrapI_IUUUUU<sceKernelSendMsgPipeCB>,              "sceKernelSendMsgPipeCB",                    'i', "ixxxxx"  },
	{0X884C9F90, &WrapI_IUUUU<sceKernelTrySendMsgPipe>,              "sceKernelTrySendMsgPipe",                   'i', "ixxxx"   },
	{0X74829B76, &WrapI_IUUUUU<sceKernelReceiveMsgPipe>,             "sceKernelReceiveMsgPipe",                   'i', "ixxxxx"  },
	{0XFBFA697D, &WrapI_IUUUUU<sceKernelReceiveMsgPipeCB>,           "sceKernelReceiveMsgPipeCB",                 'i', "ixxxxx"  },
	{0XDF52098F, &WrapI_IUUUU<sceKernelTryReceiveMsgPipe>,           "sceKernelTryReceiveMsgPipe",                'i', "ixxxx"   },
	{0X349B864D, &WrapI_IUU<sceKernelCancelMsgPipe>,                 "sceKernelCancelMsgPipe",                    'i', "ixx"     },
	{0X33BE4024, &WrapI_IU<sceKernelReferMsgPipeStatus>,             "sceKernelReferMsgPipeStatus",               'i', "ip"      },

	{0X56C039B5, &WrapI_CIUUU<sceKernelCreateVpl>,                   "sceKernelCreateVpl",                        'i', "sixxp"   },
	{0X89B3D48C, &WrapI_I<sceKernelDeleteVpl>,                       "sceKernelDeleteVpl",                        'i', "i"       },
	{0XBED27435, &WrapI_IUUU<sceKernelAllocateVpl>,                  "sceKernelAllocateVpl",                      'i', "ixxx",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XEC0A693F, &WrapI_IUUU<sceKernelAllocateVplCB>,                "sceKernelAllocateVplCB",                    'i', "ixxx",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XAF36D708, &WrapI_IUU<sceKernelTryAllocateVpl>,                "sceKernelTryAllocateVpl",                   'i', "ixx"     },
	{0XB736E9FF, &WrapI_IU<sceKernelFreeVpl>,                        "sceKernelFreeVpl",                          'i', "ix"      },
	{0X1D371B8A, &WrapI_IU<sceKernelCancelVpl>,                      "sceKernelCancelVpl",                        'i', "ix"      },
	{0X39810265, &WrapI_IU<sceKernelReferVplStatus>,                 "sceKernelReferVplStatus",                   'i', "ip"      },

	{0XC07BB470, &WrapI_CUUUUU<sceKernelCreateFpl>,                  "sceKernelCreateFpl",                        'i', "sixxxp"  },
	{0XED1410E0, &WrapI_I<sceKernelDeleteFpl>,                       "sceKernelDeleteFpl",                        'i', "i"       },
	{0XD979E9BF, &WrapI_IUU<sceKernelAllocateFpl>,                   "sceKernelAllocateFpl",                      'i', "ixx",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0XE7282CB6, &WrapI_IUU<sceKernelAllocateFplCB>,                 "sceKernelAllocateFplCB",                    'i', "ixx",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X623AE665, &WrapI_IU<sceKernelTryAllocateFpl>,                 "sceKernelTryAllocateFpl",                   'i', "ix"      },
	{0XF6414A71, &WrapI_IU<sceKernelFreeFpl>,                        "sceKernelFreeFpl",                          'i', "ix"      },
	{0XA8AA591F, &WrapI_IU<sceKernelCancelFpl>,                      "sceKernelCancelFpl",                        'i', "ix"      },
	{0XD8199E4C, &WrapI_IU<sceKernelReferFplStatus>,                 "sceKernelReferFplStatus",                   'i', "ip"      },

	{0X20FFF560, &WrapU_CU<sceKernelCreateVTimer>,                   "sceKernelCreateVTimer",                     'x', "sx",     HLE_NOT_IN_INTERRUPT },
	{0X328F9E52, &WrapU_I<sceKernelDeleteVTimer>,                    "sceKernelDeleteVTimer",                     'x', "i",      HLE_NOT_IN_INTERRUPT },
	{0XC68D9437, &WrapU_I<sceKernelStartVTimer>,                     "sceKernelStartVTimer",                      'x', "i"       },
	{0XD0AEEE87, &WrapU_I<sceKernelStopVTimer>,                      "sceKernelStopVTimer",                       'x', "i"       },
	{0XD2D615EF, &WrapU_I<sceKernelCancelVTimerHandler>,             "sceKernelCancelVTimerHandler",              'x', "i"       },
	{0XB3A59970, &WrapU_IU<sceKernelGetVTimerBase>,                  "sceKernelGetVTimerBase",                    'x', "ix"      },
	{0XB7C18B77, &WrapU64_I<sceKernelGetVTimerBaseWide>,             "sceKernelGetVTimerBaseWide",                'X', "i"       },
	{0X034A921F, &WrapU_IU<sceKernelGetVTimerTime>,                  "sceKernelGetVTimerTime",                    'x', "ix"      },
	{0XC0B3FFD2, &WrapU64_I<sceKernelGetVTimerTimeWide>,             "sceKernelGetVTimerTimeWide",                'X', "i"       },
	{0X5F32BEAA, &WrapU_IU<sceKernelReferVTimerStatus>,              "sceKernelReferVTimerStatus",                'x', "ix"      },
	{0X542AD630, &WrapU_IU<sceKernelSetVTimerTime>,                  "sceKernelSetVTimerTime",                    'x', "ix"      },
	{0XFB6425C3, &WrapU64_IU64<sceKernelSetVTimerTimeWide>,          "sceKernelSetVTimerTimeWide",                'X', "iX"      },
	{0XD8B299AE, &WrapU_IUUU<sceKernelSetVTimerHandler>,             "sceKernelSetVTimerHandler",                 'x', "ixxx"    },
	{0X53B00E9A, &WrapU_IU64UU<sceKernelSetVTimerHandlerWide>,       "sceKernelSetVTimerHandlerWide",             'x', "iXxx"    },

	{0X8DAFF657, &WrapI_CUUUUU<sceKernelCreateTlspl>,                "sceKernelCreateTlspl",                      'i', "sixxxp"  },
	{0X32BF938E, &WrapI_I<sceKernelDeleteTlspl>,                     "sceKernelDeleteTlspl",                      'i', "i"       },
	{0X721067F3, &WrapI_IU<sceKernelReferTlsplStatus>,               "sceKernelReferTlsplStatus",                 'i', "xp"      },
	// Not completely certain about args.
	{0X4A719FB2, &WrapI_I<sceKernelFreeTlspl>,                       "sceKernelFreeTlspl",                        'i', "i"       },
	// Internal.  Takes (uid, &addr) as parameters... probably.
	//{0x65F54FFB, nullptr,                                            "_sceKernelAllocateTlspl",                   'v', ""        },
	// NOTE: sceKernelGetTlsAddr is in Kernel_Library, see sceKernelInterrupt.cpp.

	// Not sure if these should be hooked up. See below.
	{0x0E927AED, &_sceKernelReturnFromTimerHandler,                  "_sceKernelReturnFromTimerHandler",          'v', ""        },
	{0X532A522E, &WrapV_I<_sceKernelExitThread>,                     "_sceKernelExitThread",                      'v', "i"       },

	// Shouldn't hook this up. No games should import this function manually and call it.
	// {0x6E9EA350, _sceKernelReturnFromCallback,"_sceKernelReturnFromCallback"},
	{0X71EC4271, &WrapU_UU<sceKernelLibcGettimeofday>,               "sceKernelLibcGettimeofday",               'x', "xx" },
	{0X79D1C3FA, &WrapI_V<sceKernelDcacheWritebackAll>,              "sceKernelDcacheWritebackAll",             'i', "" },
	{0X91E4F6A7, &WrapU_V<sceKernelLibcClock>,                       "sceKernelLibcClock",                      'x', "" },
	{0XB435DEC5, &WrapI_V<sceKernelDcacheWritebackInvalidateAll>,    "sceKernelDcacheWritebackInvalidateAll",   'i', "" },

};

const HLEFunction ThreadManForKernel[] =
{
	{0xCEADEB47, &WrapI_U<sceKernelDelayThread>,                     "sceKernelDelayThread",                      'i', "x",      HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED | HLE_KERNEL_SYSCALL },
	{0x446D8DE6, &WrapI_CUUIUU<sceKernelCreateThread>,               "sceKernelCreateThread",                     'i', "sxxixx", HLE_NOT_IN_INTERRUPT | HLE_KERNEL_SYSCALL },
	{0xF475845D, &WrapI_IIU<sceKernelStartThread>,                   "sceKernelStartThread",                      'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_KERNEL_SYSCALL },
	{0X9FA03CD3, &WrapI_I<sceKernelDeleteThread>,                    "sceKernelDeleteThread",                     'i', "i",      HLE_KERNEL_SYSCALL },
	{0XAA73C935, &WrapI_I<sceKernelExitThread>,                      "sceKernelExitThread",                       'i', "i",      HLE_KERNEL_SYSCALL },
	{0X809CE29B, &WrapI_I<sceKernelExitDeleteThread>,                "sceKernelExitDeleteThread",                 'i', "i",      HLE_KERNEL_SYSCALL },
	{0X9944F31F, &WrapI_I<sceKernelSuspendThread>,                   "sceKernelSuspendThread",                    'i', "i",      HLE_KERNEL_SYSCALL },
	{0X75156E8F, &WrapI_I<sceKernelResumeThread>,                    "sceKernelResumeThread",                     'i', "i",      HLE_KERNEL_SYSCALL },
	{0X94416130, &WrapU_UUUU<sceKernelGetThreadmanIdList>,           "sceKernelGetThreadmanIdList",               'x', "xxxx",   HLE_KERNEL_SYSCALL },
	{0x278c0df5, &WrapI_IU<sceKernelWaitThreadEnd>,                  "sceKernelWaitThreadEnd",                    'i', "ix",     HLE_KERNEL_SYSCALL },
	{0xd6da4ba1, &WrapI_CUIIU<sceKernelCreateSema>,                  "sceKernelCreateSema",                       'i', "sxiip",  HLE_KERNEL_SYSCALL },
	{0x28b6489c, &WrapI_I<sceKernelDeleteSema>,                      "sceKernelDeleteSema",                       'i', "i",      HLE_KERNEL_SYSCALL },
	{0x3f53e640, &WrapI_II<sceKernelSignalSema>,                     "sceKernelSignalSema",                       'i', "ii",     HLE_KERNEL_SYSCALL },
	{0x4e3a1105, &WrapI_IIU<sceKernelWaitSema>,                      "sceKernelWaitSema",                         'i', "iix",    HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED | HLE_KERNEL_SYSCALL},
	{0x58b1f937, &WrapI_II<sceKernelPollSema>,                       "sceKernelPollSema",                         'i', "ii",     HLE_KERNEL_SYSCALL },
	{0x55c20a00, &WrapI_CUUU<sceKernelCreateEventFlag>,              "sceKernelCreateEventFlag",                  'i', "sxxx",   HLE_KERNEL_SYSCALL },
	{0xef9e4c70, &WrapU_I<sceKernelDeleteEventFlag>,                 "sceKernelDeleteEventFlag",                  'x', "i",      HLE_KERNEL_SYSCALL },
	{0x1fb15a32, &WrapU_IU<sceKernelSetEventFlag>,                   "sceKernelSetEventFlag",                     'x', "ix",     HLE_KERNEL_SYSCALL },
	{0x812346e4, &WrapU_IU<sceKernelClearEventFlag>,                 "sceKernelClearEventFlag",                   'x', "ix",     HLE_KERNEL_SYSCALL },
	{0x402fcf22, &WrapI_IUUUU<sceKernelWaitEventFlag>,               "sceKernelWaitEventFlag",                    'i', "ixxpp",  HLE_NOT_IN_INTERRUPT | HLE_KERNEL_SYSCALL},
	{0xc07bb470, &WrapI_CUUUUU<sceKernelCreateFpl>,                  "sceKernelCreateFpl",                        'i', "sixxxp" ,HLE_KERNEL_SYSCALL },
	{0xed1410e0, &WrapI_I<sceKernelDeleteFpl>,                       "sceKernelDeleteFpl",                        'i', "i"      ,HLE_KERNEL_SYSCALL },
	{0x623ae665, &WrapI_IU<sceKernelTryAllocateFpl>,                 "sceKernelTryAllocateFpl",                   'i', "ix"     ,HLE_KERNEL_SYSCALL },
	{0x616403ba, &WrapI_I<sceKernelTerminateThread>,                 "sceKernelTerminateThread",                  'i', "i"      ,HLE_KERNEL_SYSCALL },
	{0x383f7bcc, &WrapI_I<sceKernelTerminateDeleteThread>,           "sceKernelTerminateDeleteThread",            'i', "i"      ,HLE_KERNEL_SYSCALL },
	{0x57cf62dd, &WrapU_U<sceKernelGetThreadmanIdType>,              "sceKernelGetThreadmanIdType",               'x', "x"      ,HLE_KERNEL_SYSCALL },
	{0x94aa61ee, &WrapI_V<sceKernelGetThreadCurrentPriority>,        "sceKernelGetThreadCurrentPriority",         'i', "",       HLE_KERNEL_SYSCALL },
	{0x293B45B8, &WrapI_V<sceKernelGetThreadId>,                     "sceKernelGetThreadId",                      'i', "",       HLE_KERNEL_SYSCALL | HLE_NOT_IN_INTERRUPT },
	{0x3B183E26, &WrapI_I<sceKernelGetThreadExitStatus>,             "sceKernelGetThreadExitStatus",              'i', "i",      HLE_KERNEL_SYSCALL },
	{0x82BC5777, &WrapU64_V<sceKernelGetSystemTimeWide>,             "sceKernelGetSystemTimeWide",                'X', "",       HLE_KERNEL_SYSCALL },
	{0xDB738F35, &WrapI_U<sceKernelGetSystemTime>,                   "sceKernelGetSystemTime",                    'i', "x",      HLE_KERNEL_SYSCALL },
	{0x369ED59D, &WrapU_V<sceKernelGetSystemTimeLow>,                "sceKernelGetSystemTimeLow",                 'x', "",       HLE_KERNEL_SYSCALL },
	{0x6652B8CA, &WrapI_UUU<sceKernelSetAlarm>,                      "sceKernelSetAlarm",                         'i', "xxx",    HLE_KERNEL_SYSCALL },
	{0xB2C25152, &WrapI_UUU<sceKernelSetSysClockAlarm>,              "sceKernelSetSysClockAlarm",                 'i', "xxx",    HLE_KERNEL_SYSCALL },
	{0x7E65B999, &WrapI_I<sceKernelCancelAlarm>,                     "sceKernelCancelAlarm",                      'i', "i",      HLE_KERNEL_SYSCALL },
	{0xDAA3F564, &WrapI_IU<sceKernelReferAlarmStatus>,               "sceKernelReferAlarmStatus",                 'i', "ix",     HLE_KERNEL_SYSCALL },
	{0x8125221D, &WrapI_CUU<sceKernelCreateMbx>,                     "sceKernelCreateMbx",                        'i', "sxx",    HLE_KERNEL_SYSCALL },
	{0x86255ADA, &WrapI_I<sceKernelDeleteMbx>,                       "sceKernelDeleteMbx",                        'i', "i",      HLE_KERNEL_SYSCALL },
	{0xE9B3061E, &WrapI_IU<sceKernelSendMbx>,                        "sceKernelSendMbx",                          'i', "ix",     HLE_KERNEL_SYSCALL },
	{0x18260574, &WrapI_IUU<sceKernelReceiveMbx>,                    "sceKernelReceiveMbx",                       'i', "ixx",    HLE_KERNEL_SYSCALL | HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0xF3986382, &WrapI_IUU<sceKernelReceiveMbxCB>,                  "sceKernelReceiveMbxCB",                     'i', "ixx",    HLE_KERNEL_SYSCALL | HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0x0D81716A, &WrapI_IU<sceKernelPollMbx>,                        "sceKernelPollMbx",                          'i', "ix",     HLE_KERNEL_SYSCALL },
	{0x87D4DD36, &WrapI_IU<sceKernelCancelReceiveMbx>,               "sceKernelCancelReceiveMbx",                 'i', "ix",     HLE_KERNEL_SYSCALL },
	{0xA8E8C846, &WrapI_IU<sceKernelReferMbxStatus>,                 "sceKernelReferMbxStatus",                   'i', "ip",     HLE_KERNEL_SYSCALL },
	{0x56C039B5, &WrapI_CIUUU<sceKernelCreateVpl>,                   "sceKernelCreateVpl",                        'i', "sixxp",  HLE_KERNEL_SYSCALL },
	{0x89B3D48C, &WrapI_I<sceKernelDeleteVpl>,                       "sceKernelDeleteVpl",                        'i', "i",      HLE_KERNEL_SYSCALL },
	{0xBED27435, &WrapI_IUUU<sceKernelAllocateVpl>,                  "sceKernelAllocateVpl",                      'i', "ixxx",   HLE_KERNEL_SYSCALL | HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0xEC0A693F, &WrapI_IUUU<sceKernelAllocateVplCB>,                "sceKernelAllocateVplCB",                    'i', "ixxx",   HLE_KERNEL_SYSCALL | HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0xAF36D708, &WrapI_IUU<sceKernelTryAllocateVpl>,                "sceKernelTryAllocateVpl",                   'i', "ixx",    HLE_KERNEL_SYSCALL },
	{0xB736E9FF, &WrapI_IU<sceKernelFreeVpl>,                        "sceKernelFreeVpl",                          'i', "ix",     HLE_KERNEL_SYSCALL },
	{0x1D371B8A, &WrapI_IU<sceKernelCancelVpl>,                      "sceKernelCancelVpl",                        'i', "ix",     HLE_KERNEL_SYSCALL },
	{0x39810265, &WrapI_IU<sceKernelReferVplStatus>,                 "sceKernelReferVplStatus",                   'i', "ip",     HLE_KERNEL_SYSCALL },
};

void Register_ThreadManForUser()
{
	RegisterHLEModule("ThreadManForUser", ARRAY_SIZE(ThreadManForUser), ThreadManForUser);
}


const HLEFunction LoadExecForUser[] =
{
	{0X05572A5F, &WrapV_V<sceKernelExitGame>,                        "sceKernelExitGame",                         'v', ""        },
	{0X4AC57943, &WrapI_I<sceKernelRegisterExitCallback>,            "sceKernelRegisterExitCallback",             'i', "i"       },
	{0XBD2F1094, &WrapI_CU<sceKernelLoadExec>,                       "sceKernelLoadExec",                         'i', "sx"      },
	{0X2AC9954B, &WrapV_V<sceKernelExitGameWithStatus>,              "sceKernelExitGameWithStatus",               'v', ""        },
	{0X362A956B, &WrapI_V<LoadExecForUser_362A956B>,                 "LoadExecForUser_362A956B",                  'i', ""        },
	{0X8ADA38D3, nullptr,                                            "LoadExecForUser_8ADA38D3",                  '?', ""        },
};

void Register_LoadExecForUser()
{
	RegisterHLEModule("LoadExecForUser", ARRAY_SIZE(LoadExecForUser), LoadExecForUser);
}
 
const HLEFunction LoadExecForKernel[] =
{
	{0x4AC57943, &WrapI_I<sceKernelRegisterExitCallback>,            "sceKernelRegisterExitCallback",             'i', "i",      HLE_KERNEL_SYSCALL },
	{0XA3D5E142, nullptr,                                            "sceKernelExitVSHVSH",                       '?', ""        },
	{0X28D0D249, &WrapI_CU<sceKernelLoadExec>,                       "sceKernelLoadExecVSHMs2",                   'i', "sx"      },
	{0x6D302D3D, &WrapV_V<sceKernelExitGame>,                        "sceKernelExitVSHKernel",                    'v', "x", HLE_KERNEL_SYSCALL },// when called in game mode it will have the same effect that sceKernelExitGame 	
};
 
void Register_LoadExecForKernel()
{
	RegisterHLEModule("LoadExecForKernel", ARRAY_SIZE(LoadExecForKernel), LoadExecForKernel);
}

const HLEFunction ExceptionManagerForKernel[] =
{
	{0X3FB264FC, nullptr,                                            "sceKernelRegisterExceptionHandler",         '?', ""        },
	{0X5A837AD4, nullptr,                                            "sceKernelRegisterPriorityExceptionHandler", '?', ""        },
	{0x565C0B0E, &WrapI_V<sceKernelRegisterDefaultExceptionHandler>, "sceKernelRegisterDefaultExceptionHandler",  'i', "",       HLE_KERNEL_SYSCALL },
	{0X1AA6CFFA, nullptr,                                            "sceKernelReleaseExceptionHandler",          '?', ""        },
	{0XDF83875E, nullptr,                                            "sceKernelGetActiveDefaultExceptionHandler", '?', ""        },
	{0X291FF031, nullptr,                                            "sceKernelReleaseDefaultExceptionHandler",   '?', ""        },
	{0X15ADC862, nullptr,                                            "sceKernelRegisterNmiHandler",               '?', ""        },
	{0XB15357C9, nullptr,                                            "sceKernelReleaseNmiHandler",                '?', ""        },
};

void Register_ExceptionManagerForKernel()
{
	RegisterHLEModule("ExceptionManagerForKernel", ARRAY_SIZE(ExceptionManagerForKernel), ExceptionManagerForKernel);
}

// Seen in some homebrew
const HLEFunction UtilsForKernel[] = {
	{0xC2DF770E, WrapI_UI<sceKernelIcacheInvalidateRange>,           "sceKernelIcacheInvalidateRange",            '?', "",       HLE_KERNEL_SYSCALL },
	{0X78934841, nullptr,                                            "sceKernelGzipDecompress",                   '?', ""        },
	{0XE8DB3CE6, nullptr,                                            "sceKernelDeflateDecompress",                '?', ""        },
	{0X840259F1, nullptr,                                            "sceKernelUtilsSha1Digest",                  '?', ""        },
	{0X9E5C5086, nullptr,                                            "sceKernelUtilsMd5BlockInit",                '?', ""        },
	{0X61E1E525, nullptr,                                            "sceKernelUtilsMd5BlockUpdate",              '?', ""        },
	{0XB8D24E78, nullptr,                                            "sceKernelUtilsMd5BlockResult",              '?', ""        },
	{0XC8186A58, nullptr,                                            "sceKernelUtilsMd5Digest",                   '?', ""        },
	{0X6C6887EE, nullptr,                                            "UtilsForKernel_6C6887EE",                   '?', ""        },
	{0X91E4F6A7, nullptr,                                            "sceKernelLibcClock",                        '?', ""        },
	{0X27CC57F0, nullptr,                                            "sceKernelLibcTime",                         '?', ""        },
	{0X79D1C3FA, &WrapI_V<sceKernelDcacheWritebackAll>,              "sceKernelDcacheWritebackAll",               '?', ""        },
	{0X3EE30821, &WrapI_UI<sceKernelDcacheWritebackRange>,           "sceKernelDcacheWritebackRange",             '?', ""        },
	{0X34B9FA9E, &WrapI_UI<sceKernelDcacheWritebackInvalidateRange>, "sceKernelDcacheWritebackInvalidateRange",   '?', ""        },
	{0XB435DEC5, &WrapI_V<sceKernelDcacheWritebackInvalidateAll>,    "sceKernelDcacheWritebackInvalidateAll",     '?', ""        },
	{0XBFA98062, &WrapI_UI<sceKernelDcacheInvalidateRange>,          "sceKernelDcacheInvalidateRange",            '?', ""        },
	{0X920F104A, &WrapU_V<sceKernelIcacheInvalidateAll>,             "sceKernelIcacheInvalidateAll",              '?', ""        },
	{0XE860E75E, nullptr,                                            "sceKernelUtilsMt19937Init",                 '?', ""        },
	{0X06FB8A63, nullptr,                                            "sceKernelUtilsMt19937UInt",                 '?', ""        },
};


void Register_UtilsForKernel()
{
	RegisterHLEModule("UtilsForKernel", ARRAY_SIZE(UtilsForKernel), UtilsForKernel);
}

void Register_ThreadManForKernel()
{
	RegisterHLEModule("ThreadManForKernel", ARRAY_SIZE(ThreadManForKernel), ThreadManForKernel);
}

const char *KernelErrorToString(u32 err) {
	switch (err) {
	case 0x00000000: return "ERROR_OK";
	case SCE_KERNEL_ERROR_BAD_ARGUMENT: "BAD_ARGUMENT";
	case 0x80000020: return "ALREADY";
	case 0x80000021: return "BUSY";
	case 0x80000022: return "OUT_OF_MEMORY";
	case 0x80000023: return "PRIV_REQUIRED";
	case 0x80000100: return "INVALID_ID";
	case 0x80000101: return "INVALID_NAME";
	case 0x80000102: return "INVALID_INDEX";
	case 0x80000103: return "INVALID_POINTER";
	case 0x80000104: return "INVALID_SIZE";
	case 0x80000105: return "INVALID_FLAG";
	case 0x80000106: return "INVALID_COMMAND";
	case 0x80000107: return "INVALID_MODE";
	case 0x80000108: return "INVALID_FORMAT";
	case 0x800001FE: return "INVALID_VALUE";
	case 0x800001FF: return "INVALID_ARGUMENT";
	case 0x80000209: return "BAD_FILE";
	case 0x8000020D: return "ACCESS_ERROR";
	case 0x80010002: return "ERRNO_FILE_NOT_FOUND";
	case 0x80010005: return "ERRNO_IO_ERROR";
	case 0x80010007: return "ERRNO_ARG_LIST_TOO_LONG";
	case 0x80010009: return "ERRNO_INVALID_FILE_DESCRIPTOR";
	case 0x8001000B: return "ERRNO_RESOURCE_UNAVAILABLE";
	case 0x8001000C: return "ERRNO_NO_MEMORY";
	case 0x8001000D: return "ERRNO_NO_PERM";
	case 0x8001000E: return "ERRNO_FILE_INVALID_ADDR";
	case 0x80010010: return "ERRNO_DEVICE_BUSY";
	case 0x80010011: return "ERRNO_FILE_ALREADY_EXISTS";
	case 0x80010012: return "ERRNO_CROSS_DEV_LINK";
	case 0x80010013: return "ERRNO_DEVICE_NOT_FOUND";
	case 0x80010014: return "ERRNO_NOT_A_DIRECTORY";
	case 0x80010015: return "ERRNO_IS_DIRECTORY";
	case 0x80010016: return "ERRNO_INVALID_ARGUMENT";
	case 0x80010018: return "ERRNO_TOO_MANY_OPEN_SYSTEM_FILES";
	case 0x8001001B: return "ERRNO_FILE_IS_TOO_BIG";
	case 0x8001001C: return "ERRNO_DEVICE_NO_FREE_SPACE";
	case 0x8001001E: return "ERRNO_READ_ONLY";
	case 0x80010020: return "ERRNO_CLOSED";
	case 0x80010024: return "ERRNO_FILE_PATH_TOO_LONG";
	case 0x80010047: return "ERRNO_FILE_PROTOCOL";
	case 0x8001005A: return "ERRNO_DIRECTORY_IS_NOT_EMPTY";
	case 0x8001005C: return "ERRNO_TOO_MANY_SYMBOLIC_LINKS";
	case 0x80010062: return "ERRNO_FILE_ADDR_IN_USE";
	case 0x80010067: return "ERRNO_CONNECTION_ABORTED";
	case 0x80010068: return "ERRNO_CONNECTION_RESET";
	case 0x80010069: return "ERRNO_NO_FREE_BUF_SPACE";
	case 0x8001006E: return "ERRNO_FILE_TIMEOUT";
	case 0x80010077: return "ERRNO_IN_PROGRESS";
	case 0x80010078: return "ERRNO_ALREADY";
	case 0x8001007B: return "ERRNO_NO_MEDIA";
	case 0x8001007C: return "ERRNO_INVALID_MEDIUM";
	case 0x8001007D: return "ERRNO_ADDRESS_NOT_AVAILABLE";
	case 0x8001007F: return "ERRNO_IS_ALREADY_CONNECTED";
	case 0x80010080: return "ERRNO_NOT_CONNECTED";
	case 0x80010084: return "ERRNO_FILE_QUOTA_EXCEEDED";
	case 0x8001B000: return "ERRNO_FUNCTION_NOT_SUPPORTED";
	case 0x8001B001: return "ERRNO_ADDR_OUT_OF_MAIN_MEM";
	case 0x8001B002: return "ERRNO_INVALID_UNIT_NUM";
	case 0x8001B003: return "ERRNO_INVALID_FILE_SIZE";
	case 0x8001B004: return "ERRNO_INVALID_FLAG";
	case 0x80020001: return "ERROR";
	case 0x80020002: return "NOTIMP";
	case 0x80020032: return "ILLEGAL_EXPCODE";
	case 0x80020033: return "EXPHANDLER_NOUSE";
	case 0x80020034: return "EXPHANDLER_USED";
	case 0x80020035: return "SYCALLTABLE_NOUSED";
	case 0x80020036: return "SYCALLTABLE_USED";
	case 0x80020037: return "ILLEGAL_SYSCALLTABLE";
	case 0x80020038: return "ILLEGAL_PRIMARY_SYSCALL_NUMBER";
	case 0x80020039: return "PRIMARY_SYSCALL_NUMBER_INUSE";
	case 0x80020064: return "ILLEGAL_CONTEXT";
	case 0x80020065: return "ILLEGAL_INTRCODE";
	case 0x80020066: return "CPUDI";
	case 0x80020067: return "FOUND_HANDLER";
	case 0x80020068: return "NOTFOUND_HANDLER";
	case 0x80020069: return "ILLEGAL_INTRLEVEL";
	case 0x8002006a: return "ILLEGAL_ADDRESS";
	case 0x8002006b: return "ILLEGAL_INTRPARAM";
	case 0x8002006c: return "ILLEGAL_STACK_ADDRESS";
	case 0x8002006d: return "ALREADY_STACK_SET";
	case 0x80020096: return "NO_TIMER";
	case 0x80020097: return "ILLEGAL_TIMERID";
	case 0x80020098: return "ILLEGAL_SOURCE";
	case 0x80020099: return "ILLEGAL_PRESCALE";
	case 0x8002009a: return "TIMER_BUSY";
	case 0x8002009b: return "TIMER_NOT_SETUP";
	case 0x8002009c: return "TIMER_NOT_INUSE";
	case 0x800200a0: return "UNIT_USED";
	case 0x800200a1: return "UNIT_NOUSE";
	case 0x800200a2: return "NO_ROMDIR";
	case 0x800200c8: return "IDTYPE_EXIST";
	case 0x800200c9: return "IDTYPE_NOT_EXIST";
	case 0x800200ca: return "IDTYPE_NOT_EMPTY";
	case 0x800200cb: return "UNKNOWN_UID";
	case 0x800200cc: return "UNMATCH_UID_TYPE";
	case 0x800200cd: return "ID_NOT_EXIST";
	case 0x800200ce: return "NOT_FOUND_UIDFUNC";
	case 0x800200cf: return "UID_ALREADY_HOLDER";
	case 0x800200d0: return "UID_NOT_HOLDER";
	case 0x800200d1: return "ILLEGAL_PERM";
	case 0x800200d2: return "ILLEGAL_ARGUMENT";
	case 0x800200d3: return "ILLEGAL_ADDR";
	case 0x800200d4: return "OUT_OF_RANGE";
	case 0x800200d5: return "MEM_RANGE_OVERLAP";
	case 0x800200d6: return "ILLEGAL_PARTITION";
	case 0x800200d7: return "PARTITION_INUSE";
	case 0x800200d8: return "ILLEGAL_MEMBLOCKTYPE";
	case 0x800200d9: return "MEMBLOCK_ALLOC_FAILED";
	case 0x800200da: return "MEMBLOCK_RESIZE_LOCKED";
	case 0x800200db: return "MEMBLOCK_RESIZE_FAILED";
	case 0x800200dc: return "HEAPBLOCK_ALLOC_FAILED";
	case 0x800200dd: return "HEAP_ALLOC_FAILED";
	case 0x800200de: return "ILLEGAL_CHUNK_ID";
	case 0x800200df: return "NOCHUNK";
	case 0x800200e0: return "NO_FREECHUNK";
	case 0x800200e1: return "MEMBLOCK_FRAGMENTED";
	case 0x800200e2: return "MEMBLOCK_CANNOT_JOINT";
	case 0x800200e3: return "MEMBLOCK_CANNOT_SEPARATE";
	case 0x800200e4: return "ILLEGAL_ALIGNMENT_SIZE";
	case 0x800200e5: return "ILLEGAL_DEVKIT_VER";
	case 0x8002012c: return "LINKERR";
	case 0x8002012d: return "ILLEGAL_OBJECT";
	case 0x8002012e: return "UNKNOWN_MODULE";
	case 0x8002012f: return "NOFILE";
	case 0x80020130: return "FILEERR";
	case 0x80020131: return "MEMINUSE";
	case 0x80020132: return "PARTITION_MISMATCH";
	case 0x80020133: return "ALREADY_STARTED";
	case 0x80020134: return "NOT_STARTED";
	case 0x80020135: return "ALREADY_STOPPED";
	case 0x80020136: return "CAN_NOT_STOP";
	case 0x80020137: return "NOT_STOPPED";
	case 0x80020138: return "NOT_REMOVABLE";
	case 0x80020139: return "EXCLUSIVE_LOAD";
	case 0x8002013a: return "LIBRARY_NOT_YET_LINKED";
	case 0x8002013b: return "LIBRARY_FOUND";
	case 0x8002013c: return "LIBRARY_NOTFOUND";
	case 0x8002013d: return "ILLEGAL_LIBRARY";
	case 0x8002013e: return "LIBRARY_INUSE";
	case 0x8002013f: return "ALREADY_STOPPING";
	case 0x80020140: return "ILLEGAL_OFFSET";
	case 0x80020141: return "ILLEGAL_POSITION";
	case 0x80020142: return "ILLEGAL_ACCESS";
	case 0x80020143: return "MODULE_MGR_BUSY";
	case 0x80020144: return "ILLEGAL_FLAG";
	case 0x80020145: return "CANNOT_GET_MODULELIST";
	case 0x80020146: return "PROHIBIT_LOADMODULE_DEVICE";
	case 0x80020147: return "PROHIBIT_LOADEXEC_DEVICE";
	case 0x80020148: return "UNSUPPORTED_PRX_TYPE";
	case 0x80020149: return "ILLEGAL_PERM_CALL";
	case 0x8002014a: return "CANNOT_GET_MODULE_INFORMATION";
	case 0x8002014b: return "ILLEGAL_LOADEXEC_BUFFER";
	case 0x8002014c: return "ILLEGAL_LOADEXEC_FILENAME";
	case 0x8002014d: return "NO_EXIT_CALLBACK";
	case 0x8002014e: return "MEDIA_CHANGED";
	case 0x8002014f: return "CANNOT_USE_BETA_VER_MODULE";
	case 0x80020190: return "NO_MEMORY";
	case 0x80020191: return "ILLEGAL_ATTR";
	case 0x80020192: return "ILLEGAL_ENTRY";
	case 0x80020193: return "ILLEGAL_PRIORITY";
	case 0x80020194: return "ILLEGAL_STACK_SIZE";
	case 0x80020195: return "ILLEGAL_MODE";
	case 0x80020196: return "ILLEGAL_MASK";
	case 0x80020197: return "ILLEGAL_THID";
	case 0x80020198: return "UNKNOWN_THID";
	case 0x80020199: return "UNKNOWN_SEMID";
	case 0x8002019a: return "UNKNOWN_EVFID";
	case 0x8002019b: return "UNKNOWN_MBXID";
	case 0x8002019c: return "UNKNOWN_VPLID";
	case 0x8002019d: return "UNKNOWN_FPLID";
	case 0x8002019e: return "UNKNOWN_MPPID";
	case 0x8002019f: return "UNKNOWN_ALMID";
	case 0x800201a0: return "UNKNOWN_TEID";
	case 0x800201a1: return "UNKNOWN_CBID";
	case 0x800201a2: return "DORMANT";
	case 0x800201a3: return "SUSPEND";
	case 0x800201a4: return "NOT_DORMANT";
	case 0x800201a5: return "NOT_SUSPEND";
	case 0x800201a6: return "NOT_WAIT";
	case 0x800201a7: return "CAN_NOT_WAIT";
	case 0x800201a8: return "WAIT_TIMEOUT";
	case 0x800201a9: return "WAIT_CANCEL";
	case 0x800201aa: return "RELEASE_WAIT";
	case 0x800201ab: return "NOTIFY_CALLBACK";
	case 0x800201ac: return "THREAD_TERMINATED";
	case 0x800201ad: return "SEMA_ZERO";
	case 0x800201ae: return "SEMA_OVF";
	case 0x800201af: return "EVF_COND";
	case 0x800201b0: return "EVF_MULTI";
	case 0x800201b1: return "EVF_ILPAT";
	case 0x800201b2: return "MBOX_NOMSG";
	case 0x800201b3: return "MPP_FULL";
	case 0x800201b4: return "MPP_EMPTY";
	case 0x800201b5: return "WAIT_DELETE";
	case 0x800201b6: return "ILLEGAL_MEMBLOCK";
	case 0x800201b7: return "ILLEGAL_MEMSIZE";
	case 0x800201b8: return "ILLEGAL_SPADADDR";
	case 0x800201b9: return "SPAD_INUSE";
	case 0x800201ba: return "SPAD_NOT_INUSE";
	case 0x800201bb: return "ILLEGAL_TYPE";
	case 0x800201bc: return "ILLEGAL_SIZE";
	case 0x800201bd: return "ILLEGAL_COUNT";
	case 0x800201be: return "UNKNOWN_VTID";
	case 0x800201bf: return "ILLEGAL_VTID";
	case 0x800201c0: return "ILLEGAL_KTLSID";
	case 0x800201c1: return "KTLS_FULL";
	case 0x800201c2: return "KTLS_BUSY";
	case 0x800201c9: return "MESSAGEBOX_DUPLICATE_MESSAGE";
	case SCE_KERNEL_ERROR_UNKNOWN_TLSPL_ID: return "UNKNOWN_TLSPL_ID";
	case SCE_KERNEL_ERROR_TOO_MANY_TLSPL: return "TOO_MANY_TLSPL";
	case SCE_KERNEL_ERROR_TLSPL_IN_USE: return "TLSPL_IN_USE";
	case 0x80020258: return "PM_INVALID_PRIORITY";
	case 0x80020259: return "PM_INVALID_DEVNAME";
	case 0x8002025a: return "PM_UNKNOWN_DEVNAME";
	case 0x8002025b: return "PM_PMINFO_REGISTERED";
	case 0x8002025c: return "PM_PMINFO_UNREGISTERED";
	case 0x8002025d: return "PM_INVALID_MAJOR_STATE";
	case 0x8002025e: return "PM_INVALID_REQUEST";
	case 0x8002025f: return "PM_UNKNOWN_REQUEST";
	case 0x80020260: return "PM_INVALID_UNIT";
	case 0x80020261: return "PM_CANNOT_CANCEL";
	case 0x80020262: return "PM_INVALID_PMINFO";
	case 0x80020263: return "PM_INVALID_ARGUMENT";
	case 0x80020264: return "PM_ALREADY_TARGET_PWRSTATE";
	case 0x80020265: return "PM_CHANGE_PWRSTATE_FAILED";
	case 0x80020266: return "PM_CANNOT_CHANGE_DEVPWR_STATE";
	case 0x80020267: return "PM_NO_SUPPORT_DEVPWR_STATE";
	case 0x800202bc: return "DMAC_REQUEST_FAILED";
	case 0x800202bd: return "DMAC_REQUEST_DENIED";
	case 0x800202be: return "DMAC_OP_QUEUED";
	case 0x800202bf: return "DMAC_OP_NOT_QUEUED";
	case 0x800202c0: return "DMAC_OP_RUNNING";
	case 0x800202c1: return "DMAC_OP_NOT_ASSIGNED";
	case 0x800202c2: return "DMAC_OP_TIMEOUT";
	case 0x800202c3: return "DMAC_OP_FREED";
	case 0x800202c4: return "DMAC_OP_USED";
	case 0x800202c5: return "DMAC_OP_EMPTY";
	case 0x800202c6: return "DMAC_OP_ABORTED";
	case 0x800202c7: return "DMAC_OP_ERROR";
	case 0x800202c8: return "DMAC_CHANNEL_RESERVED";
	case 0x800202c9: return "DMAC_CHANNEL_EXCLUDED";
	case 0x800202ca: return "DMAC_PRIVILEGE_ADDRESS";
	case 0x800202cb: return "DMAC_NO_ENOUGHSPACE";
	case 0x800202cc: return "DMAC_CHANNEL_NOT_ASSIGNED";
	case 0x800202cd: return "DMAC_CHILD_OPERATION";
	case 0x800202ce: return "DMAC_TOO_MUCH_SIZE";
	case 0x800202cf: return "DMAC_INVALID_ARGUMENT";
	case 0x80020320: return "MFILE";
	case 0x80020321: return "NODEV";
	case 0x80020322: return "XDEV";
	case 0x80020323: return "BADF";
	case 0x80020324: return "INVAL";
	case 0x80020325: return "UNSUP";
	case 0x80020326: return "ALIAS_USED";
	case 0x80020327: return "CANNOT_MOUNT";
	case 0x80020328: return "DRIVER_DELETED";
	case 0x80020329: return "ASYNC_BUSY";
	case 0x8002032a: return "NOASYNC";
	case 0x8002032b: return "REGDEV";
	case 0x8002032c: return "NOCWD";
	case 0x8002032d: return "NAMETOOLONG";
	case 0x800203e8: return "NXIO";
	case 0x800203e9: return "IO";
	case 0x800203ea: return "NOMEM";
	case 0x800203eb: return "STDIO_NOT_OPENED";
	case 0x8002044c: return "CACHE_ALIGNMENT";
	case 0x8002044d: return "ERRORMAX";

	// TODO: Change the above to use the enum.

	case SCE_KERNEL_ERROR_POWER_VMEM_IN_USE: return "POWER_VMEM_IN_USE";

	case SCE_ERROR_NETPARAM_BAD_NETCONF:  return "NETPARAM_BAD_NETCONF";
	case SCE_ERROR_NETPARAM_BAD_PARAM:    return "NETPARAM_BAD_PARAM";
	case SCE_ERROR_MODULE_BAD_ID:         return "MODULE_BAD_ID";
	case SCE_ERROR_MODULE_ALREADY_LOADED: return "MODULE_ALREADY_LOADED";
	case SCE_ERROR_MODULE_NOT_LOADED:     return "MODULE_NOT_LOADED";
	case SCE_ERROR_AV_MODULE_BAD_ID:      return "AV_MODULE_BAD_ID";

	case SCE_ERROR_UTILITY_INVALID_STATUS:     return "UTILITY_INVALID_STATUS";
	case SCE_ERROR_UTILITY_INVALID_PARAM_SIZE: return "UTILITY_INVALID_PARAM_SIZE";
	case SCE_ERROR_UTILITY_WRONG_TYPE:         return "UTILITY_WRONG_TYPE";
	case SCE_ERROR_UTILITY_INVALID_ADHOC_CHANNEL: return "UTILITY_INVALID_ADHOC_CHANNEL";
	case SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID: return "UTILITY_INVALID_SYSTEM_PARAM_ID";

	case SCE_ERROR_UTILITY_MSGDIALOG_BADOPTION: return "UTILITY_MSGDIALOG_BADOPTION";
	case SCE_ERROR_UTILITY_MSGDIALOG_ERRORCODEINVALID: return "UTILITY_MSGDIALOG_ERRORCODEINVALID";

	case SCE_UTILITY_SAVEDATA_ERROR_TYPE: return "SAVEDATA_TYPE";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_MS: return "SAVEDATA_LOAD_NO_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_EJECT_MS: return "SAVEDATA_LOAD_EJECT_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_ACCESS_ERROR: return "SAVEDATA_LOAD_ACCESS_ERROR";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN: return "SAVEDATA_LOAD_DATA_BROKEN";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA: return "SAVEDATA_LOAD_NO_DATA";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_PARAM: return "SAVEDATA_LOAD_PARAM";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_FILE_NOT_FOUND: return "SAVEDATA_LOAD_FILE_NOT_FOUND";
	case SCE_UTILITY_SAVEDATA_ERROR_LOAD_INTERNAL: return "SAVEDATA_LOAD_INTERNAL";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_NO_MEMSTICK: return "SAVEDATA_RW_NO_MEMSTICK";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL: return "SAVEDATA_RW_MEMSTICK_FULL";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN: return "SAVEDATA_RW_DATA_BROKEN";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA: return "SAVEDATA_RW_NO_DATA";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS: return "SAVEDATA_RW_BAD_PARAMS";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_FILE_NOT_FOUND: return "SAVEDATA_RW_FILE_NOT_FOUND";
	case SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_STATUS: return "SAVEDATA_RW_BAD_STATUS";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_MS: return "SAVEDATA_SAVE_NO_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_EJECT_MS: return "SAVEDATA_SAVE_EJECT_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE: return "SAVEDATA_SAVE_MS_NOSPACE";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_PROTECTED: return "SAVEDATA_SAVE_MS_PROTECTED";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_ACCESS_ERROR: return "SAVEDATA_SAVE_ACCESS_ERROR";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_PARAM: return "SAVEDATA_SAVE_PARAM";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_UMD: return "SAVEDATA_SAVE_NO_UMD";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_WRONG_UMD: return "SAVEDATA_SAVE_WRONG_UMD";
	case SCE_UTILITY_SAVEDATA_ERROR_SAVE_INTERNAL: return "SAVEDATA_SAVE_INTERNAL";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_MS: return "SAVEDATA_DELETE_NO_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_EJECT_MS: return "SAVEDATA_DELETE_EJECT_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_MS_PROTECTED: return "SAVEDATA_DELETE_MS_PROTECTED";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_ACCESS_ERROR: return "SAVEDATA_DELETE_ACCESS_ERROR";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA: return "SAVEDATA_DELETE_NO_DATA";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_PARAM: return "SAVEDATA_DELETE_PARAM";
	case SCE_UTILITY_SAVEDATA_ERROR_DELETE_INTERNAL: return "SAVEDATA_DELETE_INTERNAL";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_MS: return "SAVEDATA_SIZES_NO_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_EJECT_MS: return "SAVEDATA_SIZES_EJECT_MS";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_ACCESS_ERROR: return "SAVEDATA_SIZES_ACCESS_ERROR";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA: return "SAVEDATA_SIZES_NO_DATA";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_PARAM: return "SAVEDATA_SIZES_PARAM";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_UMD: return "SAVEDATA_SIZES_NO_UMD";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_WRONG_UMD: return "SAVEDATA_SIZES_WRONG_UMD";
	case SCE_UTILITY_SAVEDATA_ERROR_SIZES_INTERNAL: return "SAVEDATA_SIZES_INTERNAL";

	case SCE_ERROR_UTILITY_GAMEDATA_MEMSTICK_REMOVED: return "UTILITY_GAMEDATA_MEMSTICK_REMOVED";
	case SCE_ERROR_UTILITY_GAMEDATA_MEMSTICK_WRITE_PROTECTED: return "UTILITY_GAMEDATA_MEMSTICK_WRITE_PROTECTED";
	case SCE_ERROR_UTILITY_GAMEDATA_INVALID_MODE: return "UTILITY_GAMEDATA_INVALID_MODE";

	case SCE_ERROR_AUDIO_CHANNEL_NOT_INIT: return "AUDIO_CHANNEL_NOT_INIT";
	case SCE_ERROR_AUDIO_CHANNEL_BUSY: return "AUDIO_CHANNEL_BUSY";
	case SCE_ERROR_AUDIO_INVALID_CHANNEL: return "AUDIO_INVALID_CHANNEL";
	case SCE_ERROR_AUDIO_PRIV_REQUIRED: return "AUDIO_PRIV_REQUIRED";
	case SCE_ERROR_AUDIO_NO_CHANNELS_AVAILABLE: return "AUDIO_NO_CHANNELS_AVAILABLE";
	case SCE_ERROR_AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED: return "AUDIO_OUTPUT_SAMPLE_DATA_SIZE_NOT_ALIGNED";
	case SCE_ERROR_AUDIO_INVALID_FORMAT: return "AUDIO_INVALID_FORMAT";
	case SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED: return "AUDIO_CHANNEL_NOT_RESERVED";
	case SCE_ERROR_AUDIO_NOT_OUTPUT: return "AUDIO_NOT_OUTPUT";
	case SCE_ERROR_AUDIO_INVALID_FREQUENCY: return "AUDIO_INVALID_FREQUENCY";
	case SCE_ERROR_AUDIO_INVALID_VOLUME: return "AUDIO_INVALID_VOLUME";
	case SCE_ERROR_AUDIO_CHANNEL_ALREADY_RESERVED: return "AUDIO_CHANNEL_ALREADY_RESERVED";

	case SCE_ERROR_USBMIC_INVALID_MAX_SAMPLES: return "USBMIC_INVALID_MAX_SAMPLES";
	case SCE_ERROR_USBMIC_INVALID_SAMPLERATE: return "USBMIC_INVALID_SAMPLERATE";
	case SCE_ERROR_USB_WAIT_TIMEOUT: return "USB_WAIT_TIMEOUT";
	case SCE_ERROR_UMD_NOT_READY: return "UMD_NOT_READY";

	case SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS: return "MEMSTICK_DEVCTL_BAD_PARAMS";
	case SCE_ERROR_MEMSTICK_DEVCTL_TOO_MANY_CALLBACKS: return "MEMSTICK_DEVCTL_TOO_MANY_CALLBACKS";

	case SCE_ERROR_PGD_INVALID_HEADER: return "PGD_INVALID_HEADER";

	case SCE_ERROR_ATRAC_API_FAIL: return "SCE_ERROR_ATRAC_API_FAIL";
	case SCE_ERROR_ATRAC_NO_ATRACID: return "SCE_ERROR_ATRAC_NO_ATRACID";
	case SCE_ERROR_ATRAC_INVALID_CODECTYPE: return "SCE_ERROR_ATRAC_INVALID_CODECTYPE";
	case SCE_ERROR_ATRAC_BAD_ATRACID: return "SCE_ERROR_ATRAC_BAD_ATRACID";
	case SCE_ERROR_ATRAC_UNKNOWN_FORMAT: return "SCE_ERROR_ATRAC_UNKNOWN_FORMAT";
	case SCE_ERROR_ATRAC_WRONG_CODECTYPE: return "SCE_ERROR_ATRAC_WRONG_CODECTYPE";
	case SCE_ERROR_ATRAC_BAD_CODEC_PARAMS: return "SCE_ERROR_ATRAC_BAD_CODEC_PARAMS";
	case SCE_ERROR_ATRAC_ALL_DATA_LOADED: return "SCE_ERROR_ATRAC_ALL_DATA_LOADED";
	case SCE_ERROR_ATRAC_NO_DATA: return "SCE_ERROR_ATRAC_NO_DATA";
	case SCE_ERROR_ATRAC_SIZE_TOO_SMALL: return "SCE_ERROR_ATRAC_SIZE_TOO_SMALL";
	case SCE_ERROR_ATRAC_SECOND_BUFFER_NEEDED: return "SCE_ERROR_ATRAC_SECOND_BUFFER_NEEDED";
	case SCE_ERROR_ATRAC_INCORRECT_READ_SIZE: return "SCE_ERROR_ATRAC_INCORRECT_READ_SIZE";
	case SCE_ERROR_ATRAC_BAD_SAMPLE: return "SCE_ERROR_ATRAC_BAD_SAMPLE";
	case SCE_ERROR_ATRAC_BAD_FIRST_RESET_SIZE: return "SCE_ERROR_ATRAC_BAD_FIRST_RESET_SIZE";
	case SCE_ERROR_ATRAC_BAD_SECOND_RESET_SIZE: return "SCE_ERROR_ATRAC_BAD_SECOND_RESET_SIZE";
	case SCE_ERROR_ATRAC_ADD_DATA_IS_TOO_BIG: return "SCE_ERROR_ATRAC_ADD_DATA_IS_TOO_BIG";
	case SCE_ERROR_ATRAC_NOT_MONO: return "SCE_ERROR_ATRAC_NOT_MONO";
	case SCE_ERROR_ATRAC_NO_LOOP_INFORMATION: return "SCE_ERROR_ATRAC_NO_LOOP_INFORMATION";
	case SCE_ERROR_ATRAC_SECOND_BUFFER_NOT_NEEDED: return "SCE_ERROR_ATRAC_SECOND_BUFFER_NOT_NEEDED";
	case SCE_ERROR_ATRAC_BUFFER_IS_EMPTY: return "SCE_ERROR_ATRAC_BUFFER_IS_EMPTY";
	case SCE_ERROR_ATRAC_ALL_DATA_DECODED: return "SCE_ERROR_ATRAC_ALL_DATA_DECODED";
	case SCE_ERROR_ATRAC_IS_LOW_LEVEL: return "SCE_ERROR_ATRAC_IS_LOW_LEVEL";
	case SCE_ERROR_ATRAC_IS_FOR_SCESAS: return "SCE_ERROR_ATRAC_IS_FOR_SCESAS";
	case SCE_ERROR_ATRAC_AA3_INVALID_DATA: return "SCE_ERROR_ATRAC_AA3_INVALID_DATA";
	case SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL: return "SCE_ERROR_ATRAC_AA3_SIZE_TOO_SMALL";
	case SCE_ERROR_ATRAC_BAD_ALIGNMENT: return "SCE_ERROR_ATRAC_BAD_ALIGNMENT";

	case SCE_MPEG_ERROR_BAD_VERSION: return "SCE_MPEG_ERROR_BAD_VERSION";
	case SCE_MPEG_ERROR_NO_MEMORY: return "SCE_MPEG_ERROR_NO_MEMORY";
	case SCE_MPEG_ERROR_INVALID_ADDR: return "SCE_MPEG_ERROR_INVALID_ADDR";
	case SCE_MPEG_ERROR_INVALID_VALUE: return "SCE_MPEG_ERROR_INVALID_VALUE";
	case SCE_MPEG_ERROR_NO_DATA: return "SCE_MPEG_ERROR_NO_DATA";
	case SCE_MPEG_ERROR_ALREADY_INIT: return "SCE_MPEG_ERROR_ALREADY_INIT";
	case SCE_MPEG_ERROR_NOT_YET_INIT: return "SCE_MPEG_ERROR_NOT_YET_INIT";
	case SCE_MPEG_ERROR_AVC_INVALID_VALUE: return "SCE_MPEG_ERROR_AVC_INVALID_VALUE";
	case SCE_MPEG_ERROR_AVC_DECODE_FATAL: return "SCE_MPEG_ERROR_AVC_DECODE_FATAL";

	case SCE_PSMF_ERROR_NOT_INITIALIZED: return "SCE_PSMF_ERROR_NOT_INITIALIZED";
	case SCE_PSMF_ERROR_BAD_VERSION: return "SCE_PSMF_ERROR_BAD_VERSION";
	case SCE_PSMF_ERROR_NOT_FOUND: return "SCE_PSMF_ERROR_NOT_FOUND";
	case SCE_PSMF_ERROR_INVALID_ID: return "SCE_PSMF_ERROR_INVALID_ID";
	case SCE_PSMF_ERROR_INVALID_VALUE: return "SCE_PSMF_ERROR_INVALID_VALUE";
	case SCE_PSMF_ERROR_INVALID_TIMESTAMP: return "SCE_PSMF_ERROR_INVALID_TIMESTAMP";
	case SCE_PSMF_ERROR_INVALID_PSMF: return "SCE_PSMF_ERROR_INVALID_PSMF";

	case SCE_PSMFPLAYER_ERROR_INVALID_STATUS: return "SCE_PSMFPLAYER_ERROR_INVALID_STATUS";
	case SCE_PSMFPLAYER_ERROR_INVALID_STREAM: return "SCE_PSMFPLAYER_ERROR_INVALID_STREAM";
	case SCE_PSMFPLAYER_ERROR_BUFFER_SIZE: return "SCE_PSMFPLAYER_ERROR_BUFFER_SIZE";
	case SCE_PSMFPLAYER_ERROR_INVALID_CONFIG: return "SCE_PSMFPLAYER_ERROR_INVALID_CONFIG";
	case SCE_PSMFPLAYER_ERROR_INVALID_PARAM: return "SCE_PSMFPLAYER_ERROR_INVALID_PARAM";
	case SCE_PSMFPLAYER_ERROR_NO_MORE_DATA: return "SCE_PSMFPLAYER_ERROR_NO_MORE_DATA";

	case SCE_FONT_ERROR_OUT_OF_MEMORY: return "SCE_FONT_ERROR_OUT_OF_MEMORY";
	case SCE_FONT_ERROR_INVALID_LIBID: return "SCE_FONT_ERROR_INVALID_LIBID";
	case SCE_FONT_ERROR_INVALID_PARAMETER: return "SCE_FONT_ERROR_INVALID_PARAMETER";
	case SCE_FONT_ERROR_HANDLER_OPEN_FAILED: return "SCE_FONT_ERROR_HANDLER_OPEN_FAILED";
	case SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS: return "SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS";
	case SCE_FONT_ERROR_INVALID_FONT_DATA: return "SCE_FONT_ERROR_INVALID_FONT_DATA";

	case SCE_MUTEX_ERROR_NO_SUCH_MUTEX: return "SCE_MUTEX_ERROR_NO_SUCH_MUTEX";
	case SCE_MUTEX_ERROR_TRYLOCK_FAILED: return "SCE_MUTEX_ERROR_TRYLOCK_FAILED";
	case SCE_MUTEX_ERROR_NOT_LOCKED: return "SCE_MUTEX_ERROR_NOT_LOCKED";
	case SCE_MUTEX_ERROR_LOCK_OVERFLOW: return "SCE_MUTEX_ERROR_LOCK_OVERFLOW";
	case SCE_MUTEX_ERROR_UNLOCK_UNDERFLOW: return "SCE_MUTEX_ERROR_UNLOCK_UNDERFLOW";
	case SCE_MUTEX_ERROR_ALREADY_LOCKED: return "SCE_MUTEX_ERROR_ALREADY_LOCKED";

	case SCE_LWMUTEX_ERROR_NO_SUCH_LWMUTEX: return "SCE_LWMUTEX_ERROR_NO_SUCH_LWMUTEX";
	case SCE_LWMUTEX_ERROR_TRYLOCK_FAILED: return "SCE_LWMUTEX_ERROR_TRYLOCK_FAILED";
	case SCE_LWMUTEX_ERROR_NOT_LOCKED: return "SCE_LWMUTEX_ERROR_NOT_LOCKED";
	case SCE_LWMUTEX_ERROR_LOCK_OVERFLOW: return "SCE_LWMUTEX_ERROR_LOCK_OVERFLOW";
	case SCE_LWMUTEX_ERROR_UNLOCK_UNDERFLOW: return "SCE_LWMUTEX_ERROR_UNLOCK_UNDERFLOW";
	case SCE_LWMUTEX_ERROR_ALREADY_LOCKED: return "SCE_LWMUTEX_ERROR_ALREADY_LOCKED";

	case SCE_SSL_ERROR_NOT_INIT: return "SCE_SSL_ERROR_NOT_INIT";
	case SCE_SSL_ERROR_ALREADY_INIT: return "SCE_SSL_ERROR_ALREADY_INIT";
	case SCE_SSL_ERROR_OUT_OF_MEMORY: return "SCE_SSL_ERROR_OUT_OF_MEMORY";
	case SCE_SSL_ERROR_INVALID_PARAMETER: return "SCE_SSL_ERROR_INVALID_PARAMETER";

	case SCE_SAS_ERROR_INVALID_GRAIN: return "SCE_SAS_ERROR_INVALID_GRAIN";
	case SCE_SAS_ERROR_INVALID_MAX_VOICES: return "SCE_SAS_ERROR_INVALID_MAX_VOICES";
	case SCE_SAS_ERROR_INVALID_OUTPUT_MODE: return "SCE_SAS_ERROR_INVALID_OUTPUT_MODE";
	case SCE_SAS_ERROR_INVALID_SAMPLE_RATE: return "SCE_SAS_ERROR_INVALID_SAMPLE_RATE";
	case SCE_SAS_ERROR_BAD_ADDRESS: return "SCE_SAS_ERROR_BAD_ADDRESS";
	case SCE_SAS_ERROR_INVALID_VOICE: return "SCE_SAS_ERROR_INVALID_VOICE";
	case SCE_SAS_ERROR_INVALID_NOISE_FREQ: return "SCE_SAS_ERROR_INVALID_NOISE_FREQ";
	case SCE_SAS_ERROR_INVALID_PITCH: return "SCE_SAS_ERROR_INVALID_PITCH";
	case SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE: return "SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE";
	case SCE_SAS_ERROR_INVALID_PARAMETER: return "SCE_SAS_ERROR_INVALID_PARAMETER";
	case SCE_SAS_ERROR_INVALID_LOOP_POS: return "SCE_SAS_ERROR_INVALID_LOOP_POS";
	case SCE_SAS_ERROR_VOICE_PAUSED: return "SCE_SAS_ERROR_VOICE_PAUSED";
	case SCE_SAS_ERROR_INVALID_VOLUME: return "SCE_SAS_ERROR_INVALID_VOLUME";
	case SCE_SAS_ERROR_INVALID_ADSR_RATE: return "SCE_SAS_ERROR_INVALID_ADSR_RATE";
	case SCE_SAS_ERROR_INVALID_PCM_SIZE: return "SCE_SAS_ERROR_INVALID_PCM_SIZE";
	case SCE_SAS_ERROR_REV_INVALID_TYPE: return "SCE_SAS_ERROR_REV_INVALID_TYPE";
	case SCE_SAS_ERROR_REV_INVALID_FEEDBACK: return "SCE_SAS_ERROR_REV_INVALID_FEEDBACK";
	case SCE_SAS_ERROR_REV_INVALID_DELAY: return "SCE_SAS_ERROR_REV_INVALID_DELAY";
	case SCE_SAS_ERROR_REV_INVALID_VOLUME: return "SCE_SAS_ERROR_REV_INVALID_VOLUME";
	case SCE_SAS_ERROR_BUSY: return "SCE_SAS_ERROR_BUSY";
	case SCE_SAS_ERROR_ATRAC3_ALREADY_SET: return "SCE_SAS_ERROR_ATRAC3_ALREADY_SET";
	case SCE_SAS_ERROR_ATRAC3_NOT_SET: return "SCE_SAS_ERROR_ATRAC3_NOT_SET";
	case SCE_SAS_ERROR_NOT_INIT: return "SCE_SAS_ERROR_NOT_INIT";

	case SCE_AVCODEC_ERROR_INVALID_DATA: return "SCE_AVCODEC_ERROR_INVALID_DATA";

	case SCE_REG_ERROR_MALLOC_FAILURE: return "SCE_REG_ERROR_MALLOC_FAILURE";
	case SCE_REG_ERROR_CATEGORY_NOT_FOUND: return "SCE_REG_ERROR_CATEGORY_NOT_FOUND";
	case SCE_REG_ERROR_REGISTRY_NOT_FOUND: return "SCE_REG_ERROR_REGISTRY_NOT_FOUND";
	case SCE_REG_ERROR_INVALID_PATH: return "SCE_REG_ERROR_INVALID_PATH";
	case SCE_REG_ERROR_INVALID_NAME: return "SCE_REG_ERROR_INVALID_NAME";
	case SCE_REG_ERROR_PERMISSION_FAILURE: return "SCE_REG_ERROR_PERMISSION_FAILURE";

	case SCE_MP3_ERROR_INVALID_HANDLE: return "SCE_MP3_ERROR_INVALID_HANDLE";
	case SCE_MP3_ERROR_UNRESERVED_HANDLE: return "SCE_MP3_ERROR_UNRESERVED_HANDLE";
	case SCE_MP3_ERROR_NOT_YET_INIT_HANDLE: return "SCE_MP3_ERROR_NOT_YET_INIT_HANDLE";
	case SCE_MP3_ERROR_NO_RESOURCE_AVAIL: return "SCE_MP3_ERROR_NO_RESOURCE_AVAIL";
	case SCE_MP3_ERROR_BAD_SAMPLE_RATE: return "SCE_MP3_ERROR_BAD_SAMPLE_RATE";
	case SCE_MP3_ERROR_BAD_RESET_FRAME: return "SCE_MP3_ERROR_BAD_RESET_FRAME";
	case SCE_MP3_ERROR_BAD_ADDR: return "SCE_MP3_ERROR_BAD_ADDR";
	case SCE_MP3_ERROR_BAD_SIZE: return "SCE_MP3_ERROR_BAD_SIZE";

	case SCE_NET_RESOLVER_ERROR_NOT_TERMINATED: return "SCE_NET_RESOLVER_ERROR_NOT_TERMINATED";
	case SCE_NET_RESOLVER_ERROR_NO_DNS_SERVER: return "SCE_NET_RESOLVER_ERROR_NO_DNS_SERVER";
	case SCE_NET_RESOLVER_ERROR_INVALID_PTR: return "SCE_NET_RESOLVER_ERROR_INVALID_PTR";
	case SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN: return "SCE_NET_RESOLVER_ERROR_INVALID_BUFLEN";
	case SCE_NET_RESOLVER_ERROR_INVALID_ID: return "SCE_NET_RESOLVER_ERROR_INVALID_ID";
	case SCE_NET_RESOLVER_ERROR_ID_MAX: return "SCE_NET_RESOLVER_ERROR_ID_MAX";
	case SCE_NET_RESOLVER_ERROR_NO_MEM: return "SCE_NET_RESOLVER_ERROR_NO_MEM";
	case SCE_NET_RESOLVER_ERROR_BAD_ID: return "SCE_NET_RESOLVER_ERROR_BAD_ID";
	case SCE_NET_RESOLVER_ERROR_CTX_BUSY: return "SCE_NET_RESOLVER_ERROR_CTX_BUSY";
	case SCE_NET_RESOLVER_ERROR_ALREADY_STOPPED: return "SCE_NET_RESOLVER_ERROR_ALREADY_STOPPED";
	case SCE_NET_RESOLVER_ERROR_NOT_SUPPORTED: return "SCE_NET_RESOLVER_ERROR_NOT_SUPPORTED";
	case SCE_NET_RESOLVER_ERROR_BUF_NO_SPACE: return "SCE_NET_RESOLVER_ERROR_BUF_NO_SPACE";
	case SCE_NET_RESOLVER_ERROR_INVALID_PACKET: return "SCE_NET_RESOLVER_ERROR_INVALID_PACKET";
	case SCE_NET_RESOLVER_ERROR_STOPPED: return "SCE_NET_RESOLVER_ERROR_STOPPED";
	case SCE_NET_RESOLVER_ERROR_SOCKET: return "SCE_NET_RESOLVER_ERROR_SOCKET";
	case SCE_NET_RESOLVER_ERROR_TIMEOUT: return "SCE_NET_RESOLVER_ERROR_TIMEOUT";
	case SCE_NET_RESOLVER_ERROR_NO_RECORD: return "SCE_NET_RESOLVER_ERROR_NO_RECORD";
	case SCE_NET_RESOLVER_ERROR_RES_PACKET_FORMAT: return "SCE_NET_RESOLVER_ERROR_RES_PACKET_FORMAT";
	case SCE_NET_RESOLVER_ERROR_RES_SERVER_FAILURE: return "SCE_NET_RESOLVER_ERROR_RES_SERVER_FAILURE";
	case SCE_NET_RESOLVER_ERROR_INVALID_HOST: return "SCE_NET_RESOLVER_ERROR_INVALID_HOST";
	case SCE_NET_RESOLVER_ERROR_RES_NOT_IMPLEMENTED: return "SCE_NET_RESOLVER_ERROR_RES_NOT_IMPLEMENTED";
	case SCE_NET_RESOLVER_ERROR_RES_SERVER_REFUSED: return "SCE_NET_RESOLVER_ERROR_RES_SERVER_REFUSED";
	case SCE_NET_RESOLVER_ERROR_INTERNAL: return "SCE_NET_RESOLVER_ERROR_INTERNAL";

	case SCE_NET_APCTL_ERROR_ALREADY_INITIALIZED: return "SCE_NET_APCTL_ERROR_ALREADY_INITIALIZED";
	case SCE_NET_APCTL_ERROR_INVALID_CODE: return "SCE_NET_APCTL_ERROR_INVALID_CODE";
	case SCE_NET_APCTL_ERROR_INVALID_IP: return "SCE_NET_APCTL_ERROR_INVALID_IP";
	case SCE_NET_APCTL_ERROR_NOT_DISCONNECTED: return "SCE_NET_APCTL_ERROR_NOT_DISCONNECTED";
	case SCE_NET_APCTL_ERROR_NOT_IN_BSS: return "SCE_NET_APCTL_ERROR_NOT_IN_BSS";
	case SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF: return "SCE_NET_APCTL_ERROR_WLAN_SWITCH_OFF";
	case SCE_NET_APCTL_ERROR_WLAN_BEACON_LOST: return "SCE_NET_APCTL_ERROR_WLAN_BEACON_LOST";
	case SCE_NET_APCTL_ERROR_WLAN_DISASSOCIATION: return "SCE_NET_APCTL_ERROR_WLAN_DISASSOCIATION";
	case SCE_NET_APCTL_ERROR_INVALID_ID: return "SCE_NET_APCTL_ERROR_INVALID_ID";
	case SCE_NET_APCTL_ERROR_WLAN_SUSPENDED: return "SCE_NET_APCTL_ERROR_WLAN_SUSPENDED";
	case SCE_NET_APCTL_ERROR_TIMEOUT: return "SCE_NET_APCTL_ERROR_TIMEOUT";
	default:
		return nullptr;
	}
}
