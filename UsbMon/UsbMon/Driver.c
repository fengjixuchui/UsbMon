 
#include <fltKernel.h>

#include "UsbUtil.h" 
#include "UsbHid.h"
#pragma warning (disable : 4100)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////	Types
////

/*
typedef struct PENDINGIRP
{
	RT_LIST_ENTRY entry;
	PIRP irp; 
}PENDINGIRP, *PPENDINGIRP;
*/

typedef struct HIJACK_CONTEXT
{
	PDEVICE_OBJECT		  DeviceObject;
	PIRP				  Irp;
	PVOID				  Context;
	IO_COMPLETION_ROUTINE* routine;
	PURB				  urb;
//	PENDINGIRP*			  pending_irp;
}HIJACK_CONTEXT, *PHIJACK_CONTEXT;    
  
typedef enum _DEVICE_PNP_STATE {
	NotStarted = 0,         // Not started
	Started,                // After handling of START_DEVICE IRP
	StopPending,            // After handling of QUERY_STOP IRP
	Stopped,                // After handling of STOP_DEVICE IRP
	RemovePending,          // After handling of QUERY_REMOVE IRP
	SurpriseRemovePending,  // After handling of SURPRISE_REMOVE IRP
	Deleted,                // After handling of REMOVE_DEVICE IRP
	UnKnown                 // Unknown state
} DEVICE_PNP_STATE;
 
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////	Global Variable
////

//PENDINGIRP	  g_head;
DRIVER_DISPATCH*  g_pDispatchInternalDeviceControlForCcgp = NULL;
DRIVER_DISPATCH*  g_pDispatchInternalDeviceControl = NULL;
DRIVER_DISPATCH*  g_pDispatchPnP  = NULL;
PHID_DEVICE_NODE* g_pHidWhiteList = NULL;
volatile LONG	  g_irp_count	  = 0;
volatile LONG	  g_current_index = 0;
PDRIVER_OBJECT    g_pDriverObj	  = NULL;


//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////	Marco
////  
#define ARRAY_SIZE      100
 
 
 
//-----------------------------------------------------------------------------------------
/*
 VOID RemoveAllPendingIrpFromList()
 { 
	 RT_LIST_ENTRY* pEntry = head.entry.Flink;
	 while (pEntry != &head.entry && !pEntry)
	 {
		 PENDINGIRP* pStrct;
		 pStrct = CONTAINING_RECORD(pEntry, PENDINGIRP, entry);
		 if (pStrct)
		 {
			 if (pStrct->irp)
			 { 
				 IoCancelIrp(pStrct->irp);
			 }
			 if (&(pStrct->entry))
			 {
				 RTRemoveEntryList(&(pStrct->entry));
			 } 
			 ExFreePool(pStrct);
		 }
		 pEntry = pEntry->Flink;
	 } 
 } 
*/
//-----------------------------------------------------------------------------------------
VOID DriverUnload(
	_In_ struct _DRIVER_OBJECT *DriverObject
)
{
	UNREFERENCED_PARAMETER(DriverObject);
	 
	//RemoveAllPendingIrpFromList();

	PDRIVER_OBJECT pDriverObj = NULL; 

	if (NT_SUCCESS(GetUsbHub(USB2, &pDriverObj)))
	{
		if (pDriverObj && g_pDispatchInternalDeviceControl)
		{
			InterlockedExchange64((LONG64 volatile *)&pDriverObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL], (LONG64)g_pDispatchInternalDeviceControl);
			g_pDispatchInternalDeviceControl = NULL;
		}
	}  
	if (NT_SUCCESS(GetUsbHub(USB_COMPOSITE, &pDriverObj)))
	{
		if (pDriverObj && g_pDispatchInternalDeviceControlForCcgp)
		{
			InterlockedExchange64((LONG64 volatile *)&pDriverObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL], (LONG64)g_pDispatchInternalDeviceControlForCcgp);
			g_pDispatchInternalDeviceControlForCcgp = NULL;
		}
	}
	while (g_irp_count)
	{
		KeSleep(0);
	}
	
	STACK_TRACE_DEBUG_INFO("IRP finished All \r\n"); 

	while (g_current_index > 0)
	{
		if (g_pHidWhiteList[g_current_index])
		{
			ExFreePool(g_pHidWhiteList[g_current_index]);
		}
		InterlockedDecrement(&g_current_index);
	}

	return;
}
//----------------------------------------------------------------------------------------//
NTSTATUS  MyCompletionCallback(
	_In_     PDEVICE_OBJECT DeviceObject,			//Class Driver-Created Device Object by MiniDriver DriverObject
	_In_     PIRP           Irp,
	_In_opt_ PVOID          Context
)
{ 
	HIJACK_CONTEXT* pContext		= (HIJACK_CONTEXT*)Context;
	PVOID context					= pContext->Context;
	IO_COMPLETION_ROUTINE* callback = pContext->routine;
	WCHAR DeviceName[256]			= { 0 };

 	//DumpUrb(pContext->urb);
	/*
	if (&pContext->pending_irp->entry)
	{
		ExFreePool(&pContext->pending_irp->entry);
	}
	*/
	// Extract HIJACK_CONTEXT  

	GetDeviceName(DeviceObject, DeviceName);

	STACK_TRACE_DEBUG_INFO("Come here is mouse deviceName: %ws DriverName: %ws \r\n", DeviceObject->DriverObject->DriverName.Buffer, DeviceName);

	ExFreePool(Context);

	InterlockedDecrement(&g_irp_count);

	return callback(DeviceObject, Irp, context);
}
 
/*---------------------------------------------------------------------
NTSTATUS DispatchPnP(
	_Inout_ struct _DEVICE_OBJECT *DeviceObject,
	_Inout_ struct _IRP           *Irp
)
{
	NTSTATUS Status = Irp->IoStatus.Status;
	PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
	switch (IoStack->MinorFunction)
	{
	case IRP_MN_QUERY_INTERFACE:

	}
	return g_pDispatchPnP(DeviceObject, Irp);
}
-----------------------------------------------------------------------*/

NTSTATUS CheckPipeHandle(USBD_PIPE_HANDLE pipe_handle_from_urb, USBD_PIPE_INFORMATION* pipe_handle_from_whitelist, ULONG NumberOfPipes)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	do {
		ULONG i;
		for (i = 0; i < NumberOfPipes; i++)
		{
			STACK_TRACE_DEBUG_INFO("pipe_handle_from_urb: %I64x pipe_handle_from_whitelist[]: %I64x \r\n", pipe_handle_from_urb, pipe_handle_from_whitelist[i].PipeHandle);
			if (pipe_handle_from_urb == pipe_handle_from_whitelist[i].PipeHandle)
			{
				status = STATUS_SUCCESS;
				break;
			}
		}
		 
	} while (FALSE);

	return status; 
}
//----------------------------------------------------------------------------------------// 

BOOLEAN CheckIfDeviceObjectExist(PDEVICE_OBJECT hid_device_object, USBD_PIPE_HANDLE handle)
{
	BOOLEAN exist = FALSE;
	int i = 0;
	while (g_pHidWhiteList[i])
	{
		STACK_TRACE_DEBUG_INFO("hid_device_object: %I64x \r\n", hid_device_object);

 		if (g_pHidWhiteList[i]->device_object == hid_device_object)
		{
			NTSTATUS status = CheckPipeHandle(handle,
					g_pHidWhiteList[i]->InterfaceDesc->Pipes,
					g_pHidWhiteList[i]->InterfaceDesc->NumberOfPipes
			);

			if (NT_SUCCESS(status))
			{
				STACK_TRACE_DEBUG_INFO("Handle Matched: %I64x \r\n", handle);
				STACK_TRACE_DEBUG_INFO("Matched DeviceObject in Array: %I64X Input Device: %I64X\r\n", g_pHidWhiteList[i]->device_object, hid_device_object);
				exist = TRUE;
			}
			else
			{
				STACK_TRACE_DEBUG_INFO("Handle not Matched: %I64x \r\n", handle);
			}
		}

		i++;
	}

	return exist;
}

//----------------------------------------------------------------------------------------//  
PDEVICE_OBJECT TraceHidDeviceObject(PDEVICE_OBJECT device_object)
{
	PDEVICE_OBJECT ret_device_obj = device_object;

	if (!g_pDriverObj)
	{
		GetDriverObjectByName(L"\\Driver\\hidusb", &g_pDriverObj);
	}

	if (!g_pDriverObj)
	{
		return NULL;
	}

 	while (ret_device_obj)
	{
		WCHAR device_name[256] = { 0 };
		GetDeviceName(ret_device_obj, device_name);
	//	STACK_TRACE_DEBUG_INFO("DriverObj: %I64X DriverName: %ws DeviceObj: %I64X DeviceName: %ws \r\n",
		//	ret_device_obj->DriverObject, ret_device_obj->DriverObject->DriverName.Buffer, ret_device_obj, device_name);

		if (ret_device_obj->DriverObject == g_pDriverObj)
		{
			//STACK_TRACE_DEBUG_INFO("LastDevice DriverObj: %ws \r\n device_obj:%I64X \r\n", device_obj->DriverObject->DriverName.Buffer, device_obj);
			break;
		}
		ret_device_obj = ret_device_obj->AttachedDevice;
	}

	return ret_device_obj;
}
//----------------------------------------------------------------------------------------// 
NTSTATUS DispatchInternalDeviceControl(
	_Inout_ struct _DEVICE_OBJECT *DeviceObject,
	_Inout_ struct _IRP           *Irp
)
{
	do
	{ 
		HIJACK_CONTEXT* hijack		= NULL;
		PIO_STACK_LOCATION irpStack = NULL;
		PDEVICE_OBJECT	device_obj  = NULL;
		PURB urb					= NULL;

		//PENDINGIRP*	new_entry  = (PENDINGIRP*)ExAllocatePoolWithTag(NonPagedPool, sizeof(PENDINGIRP), 'kcaj');		

		irpStack = IoGetCurrentIrpStackLocation(Irp);
		if (irpStack->Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
		{
			break;
		}
		
		urb = (PURB)irpStack->Parameters.Others.Argument1;
		if (urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
		{
			break;
		}

		hijack = (HIJACK_CONTEXT*)ExAllocatePoolWithTag(NonPagedPool, sizeof(HIJACK_CONTEXT), 'kcaj');
		
		if (!hijack)
		{
			break;
		}
		
		RtlZeroMemory(hijack, sizeof(HIJACK_CONTEXT));

		device_obj = TraceHidDeviceObject(DeviceObject);
		if (!device_obj)
		{
			//STACK_TRACE_DEBUG_INFO("cannot find device_obj \r\n");
			break;
		}
		
		if (CheckIfDeviceObjectExist(device_obj,urb->UrbBulkOrInterruptTransfer.PipeHandle))
		{
			// TODO: Insert if keyboard / mouse device
			// new_entry->irp = Irp;
			// RTInsertTailList(&head.entry, &new_entry->entry);
			hijack->routine = irpStack->CompletionRoutine;
			hijack->Context = irpStack->Context;
			hijack->DeviceObject = DeviceObject;
			hijack->Irp = Irp;
			hijack->urb = urb;
			//hijack->pending_irp = new_entry; 

			irpStack->CompletionRoutine = MyCompletionCallback;
			irpStack->Context = hijack;

			STACK_TRACE_DEBUG_INFO("Targeted \r\n");
			InterlockedIncrement(&g_irp_count);
		}

	} while (0);

	return g_pDispatchInternalDeviceControl(DeviceObject, Irp);
}

//----------------------------------------------------------------------------------------// 
NTSTATUS DispatchInternalDeviceControlForCcgp(
	_Inout_ struct _DEVICE_OBJECT *DeviceObject,
	_Inout_ struct _IRP           *Irp
)
{
	do
	{
		HIJACK_CONTEXT* hijack = NULL;
		PIO_STACK_LOCATION irpStack = NULL;
		PDEVICE_OBJECT	device_obj = NULL;
		PURB urb = NULL;

		//PENDINGIRP*	new_entry  = (PENDINGIRP*)ExAllocatePoolWithTag(NonPagedPool, sizeof(PENDINGIRP), 'kcaj');		

		irpStack = IoGetCurrentIrpStackLocation(Irp);
		if (irpStack->Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
		{
			break;
		}

		urb = (PURB)irpStack->Parameters.Others.Argument1;
		if (urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
		{
			break;
		}

		hijack = (HIJACK_CONTEXT*)ExAllocatePoolWithTag(NonPagedPool, sizeof(HIJACK_CONTEXT), 'kcaj');

		if (!hijack)
		{
			break;
		}

		RtlZeroMemory(hijack, sizeof(HIJACK_CONTEXT));

		device_obj = TraceHidDeviceObject(DeviceObject);
		if (!device_obj)
		{
			STACK_TRACE_DEBUG_INFO("cannot find device_obj \r\n");
			break;
		}

		if (CheckIfDeviceObjectExist(device_obj, urb->UrbBulkOrInterruptTransfer.PipeHandle))
		{
			// TODO: Insert if keyboard / mouse device
			// new_entry->irp = Irp;
			// RTInsertTailList(&head.entry, &new_entry->entry);
			hijack->routine = irpStack->CompletionRoutine;
			hijack->Context = irpStack->Context;
			hijack->DeviceObject = DeviceObject;
			hijack->Irp = Irp;
			hijack->urb = urb;
			//hijack->pending_irp = new_entry; 

			irpStack->CompletionRoutine = MyCompletionCallback;
			irpStack->Context = hijack;

			STACK_TRACE_DEBUG_INFO("Targeted \r\n");
			InterlockedIncrement(&g_irp_count);
		}

	} while (0);

	return g_pDispatchInternalDeviceControlForCcgp(DeviceObject, Irp);
}
//----------------------------------------------------------------------------------------//
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING String)
{

	PDRIVER_OBJECT pDriverObj;

	//RTInitializeListHead(&head.entry);
  
	WCHAR* Usbhub = GetUsbHubDriverNameByVersion(USB);  
	STACK_TRACE_DEBUG_INFO("Usb Hun: %ws \r\n", Usbhub);

	WCHAR* Usbhub2 = GetUsbHubDriverNameByVersion(USB2); 
	STACK_TRACE_DEBUG_INFO("Usb Hun2: %ws \r\n", Usbhub2);

	WCHAR* Usbhub3 = GetUsbHubDriverNameByVersion(USB3); 
	STACK_TRACE_DEBUG_INFO("Usb Hun3: %ws \r\n", Usbhub3);

	PHID_DEVICE_NODE* device_object_list = NULL;
	ULONG size = 0;

	pDriverObj = NULL;
	GetDriverObjectByName(L"\\Driver\\hidusb", &g_pDriverObj);
	if (!g_pDriverObj)
	{
		return STATUS_UNSUCCESSFUL;
	}

	if (NT_SUCCESS(SearchAllHidRelation(&device_object_list, &size)))
	{
		STACK_TRACE_DEBUG_INFO("device_object_list: %I64X size: %x \r\n", device_object_list, size);

		//if (NT_SUCCESS(SearchAllUsbCcgpRelation(&device_object_list, &size)))
	//	{
			STACK_TRACE_DEBUG_INFO("device_object_list_usbccgp: %I64X size: %x \r\n", device_object_list, size);
			if (device_object_list && size)
			{
				g_pHidWhiteList = device_object_list;
				g_current_index = size;

				pDriverObj = NULL;
				/*GetUsbHub(USB3, &pDriverObj);

				if (!pDriverObj)
				{
					GetUsbHub(USB3_NEW, &pDriverObj);
				}
				*/
				if (!pDriverObj)
				{
					GetUsbHub(USB2, &pDriverObj);
				}

				if (!pDriverObj)
				{
					return 0;
				}

				g_pDispatchInternalDeviceControl = (DRIVER_DISPATCH*)InterlockedExchange64(
					(LONG64 volatile *)&pDriverObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL],
					(LONG64)DispatchInternalDeviceControl
				);

				GetUsbHub(USB_COMPOSITE, &pDriverObj);
				g_pDispatchInternalDeviceControlForCcgp = (DRIVER_DISPATCH*)InterlockedExchange64(
					(LONG64 volatile *)&pDriverObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL],
					(LONG64)DispatchInternalDeviceControlForCcgp
				);

			}
		//}
	}
	object->DriverUnload = DriverUnload;

	return 0;
}
 