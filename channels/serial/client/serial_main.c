/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Serial Port Device Service Virtual Channel
 *
 * Copyright 2011 O.S. Systems Software Ltda.
 * Copyright 2011 Eduardo Fiss Beloni <beloni@ossystems.com.br>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SYS_MODEM_H
#include <sys/modem.h>
#endif
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#ifdef HAVE_SYS_STRTIO_H
#include <sys/strtio.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "serial_tty.h"
#include "serial_constants.h"

#include <winpr/collections.h>
#include <winpr/comm.h>
#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>

#include <freerdp/freerdp.h>
#include <freerdp/utils/list.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/utils/svc_plugin.h>

typedef struct _SERIAL_DEVICE SERIAL_DEVICE;

struct _SERIAL_DEVICE
{
	DEVICE device;
	HANDLE* hComm;

	// TMP: TBR
	char* path;
	SERIAL_TTY* tty;
	//

	wLog* log;
	HANDLE thread;
	wMessageQueue* IrpQueue;
};

static void serial_process_irp_create(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 FileId;
	
	DWORD DesiredAccess;
	DWORD SharedAccess;
	DWORD CreateDisposition;
	UINT32 PathLength;
	
	Stream_Read_UINT32(irp->input, DesiredAccess);		/* DesiredAccess (4 bytes) */
	Stream_Seek_UINT64(irp->input);				/* AllocationSize (8 bytes) */
	Stream_Seek_UINT32(irp->input);				/* FileAttributes (4 bytes) */
	Stream_Read_UINT32(irp->input, SharedAccess);		/* SharedAccess (4 bytes) */
	Stream_Read_UINT32(irp->input, CreateDisposition);	/* CreateDisposition (4 bytes) */
	Stream_Seek_UINT32(irp->input); 			/* CreateOptions (4 bytes) */
	Stream_Read_UINT32(irp->input, PathLength);		/* PathLength (4 bytes) */
	Stream_Seek(irp->input, PathLength);			/* Path (variable) */

	assert(PathLength == 0); /* MS-RDPESP 2.2.2.2 */

	/* CommCreateFileA current implementation expects nothing else than that */
	assert(DesiredAccess == (GENERIC_READ | GENERIC_WRITE));
	assert(SharedAccess == 0);
	assert(CreateDisposition == OPEN_EXISTING);

	serial->hComm = CreateFile(serial->device.name,
				   DesiredAccess,	/* GENERIC_READ | GENERIC_WRITE */
				   SharedAccess,	/* 0 */
				   NULL,		/* SecurityAttributes */
				   CreateDisposition,	/* OPEN_EXISTING */
				   0,			/* FlagsAndAttributes */
				   NULL);		/* TemplateFile */

	if (!serial->hComm || (serial->hComm == INVALID_HANDLE_VALUE))
	{
		DEBUG_WARN("CreateFile failure: %s last-error: Ox%x\n", serial->device.name, GetLastError());

		irp->IoStatus = STATUS_UNSUCCESSFUL;
		FileId = 0;
		goto error_handle;
	}

	FileId = irp->devman->id_sequence++; // TMP: !?
	irp->IoStatus = STATUS_SUCCESS;

	DEBUG_SVC("%s %s (%d) created.", serial->device.name, serial->path, FileId);

  error_handle:
	Stream_Write_UINT32(irp->output, FileId);	/* FileId (4 bytes) */
	Stream_Write_UINT8(irp->output, 0);		/* Information (1 byte) */

	irp->Complete(irp);
}

static void serial_process_irp_close(SERIAL_DEVICE* serial, IRP* irp)
{
	Stream_Seek(irp->input, 32); /* Padding (32 bytes) */

	if (!CloseHandle(serial->hComm))
	{
		DEBUG_WARN("CloseHandle failure: %s %s (%d) closed.", serial->device.name, serial->path, irp->device->id);
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	DEBUG_SVC("%s %s (%d) closed.", serial->device.name, serial->path, irp->device->id);

	serial->hComm = NULL;
	irp->IoStatus = STATUS_SUCCESS;

  error_handle:
	Stream_Zero(irp->output, 5); /* Padding (5 bytes) */

	irp->Complete(irp);
}

static void serial_process_irp_read(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 Length;
	UINT64 Offset;
	BYTE* buffer = NULL;
	SERIAL_TTY* tty = serial->tty;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */

	if (!tty)
	{
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		Length = 0;

		DEBUG_WARN("tty not valid.");
	}
	else
	{
		buffer = (BYTE*) malloc(Length);

		if (!serial_tty_read(tty, buffer, &Length))
		{
			irp->IoStatus = STATUS_UNSUCCESSFUL;
			free(buffer);
			buffer = NULL;
			Length = 0;

			DEBUG_WARN("read %s(%d) failed.", serial->path, tty->id);
		}
		else
		{
			DEBUG_SVC("read %llu-%llu from %d", Offset, Offset + Length, tty->id);
		}
	}

	Stream_Write_UINT32(irp->output, Length); /* Length (4 bytes) */

	if (Length > 0)
	{
		Stream_EnsureRemainingCapacity(irp->output, Length);
		Stream_Write(irp->output, buffer, Length);
	}

	free(buffer);

	irp->Complete(irp);
}

static void serial_process_irp_write(SERIAL_DEVICE* serial, IRP* irp)
{
	int status;
	UINT32 Length;
	UINT64 Offset;
	SERIAL_TTY* tty = serial->tty;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */

	if (!tty)
	{
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		Length = 0;

		DEBUG_WARN("tty not valid.");

		Stream_Write_UINT32(irp->output, Length); /* Length (4 bytes) */
		Stream_Write_UINT8(irp->output, 0); /* Padding (1 byte) */
		irp->Complete(irp);
		return;
	}

	status = serial_tty_write(tty, Stream_Pointer(irp->input), Length);

	if (status < 0)
	{
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		Length = 0;

		printf("serial_tty_write failure: status: %d, errno: %d\n", status, errno);

		Stream_Write_UINT32(irp->output, Length); /* Length (4 bytes) */
		Stream_Write_UINT8(irp->output, 0); /* Padding (1 byte) */
		irp->Complete(irp);
		return;
	}

	Stream_Write_UINT32(irp->output, Length); /* Length (4 bytes) */
	Stream_Write_UINT8(irp->output, 0); /* Padding (1 byte) */
	irp->Complete(irp);
}

static void serial_process_irp_device_control(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 IoControlCode;
	UINT32 InputBufferLength;
	BYTE*  InputBuffer = NULL;
	UINT32 OutputBufferLength;
	BYTE*  OutputBuffer = NULL;
	DWORD  BytesReturned = 0;
	SERIAL_TTY* tty = serial->tty;

	Stream_Read_UINT32(irp->input, OutputBufferLength); /* OutputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, InputBufferLength); /* InputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, IoControlCode); /* IoControlCode (4 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */

	if (!tty)
	{
		DEBUG_WARN("tty not valid.");
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	OutputBuffer = (BYTE*)calloc(OutputBufferLength, sizeof(BYTE));
	if (OutputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	InputBuffer = (BYTE*)calloc(InputBufferLength, sizeof(BYTE));
	if (InputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}
		
	/* FIXME: to be replaced by DeviceIoControl() */
	if (CommDeviceIoControl(serial->hComm, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength, &BytesReturned, NULL))
	{
		Stream_Write(irp->output, OutputBuffer, BytesReturned);
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		DEBUG_SVC("CommDeviceIoControl failure: IoControlCode 0x%x last-error: 0x%x", IoControlCode, GetLastError());

		switch(GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_NOT_SUPPORTED:
				irp->IoStatus = STATUS_INVALID_PARAMETER;
				break;

			case ERROR_INSUFFICIENT_BUFFER:
				irp->IoStatus = STATUS_BUFFER_TOO_SMALL; /* TMP: better have STATUS_BUFFER_SIZE_TOO_SMALL? http://msdn.microsoft.com/en-us/library/windows/hardware/ff547466%28v=vs.85%29.aspx#generic_status_values_for_serial_device_control_requests */
				break;

			default:
				DEBUG_SVC("unexpected last-error: 0x%x", GetLastError());
				irp->IoStatus = STATUS_UNSUCCESSFUL;
				break;
		}
	}

  error_handle:
	if (InputBuffer != NULL)
		free(InputBuffer);

	if (OutputBuffer != NULL)
		free(OutputBuffer);

	// TMP: double check the completion, Information field
	irp->Complete(irp);
}

static void serial_process_irp(SERIAL_DEVICE* serial, IRP* irp)
{
	WLog_Print(serial->log, WLOG_DEBUG, "IRP MajorFunction: 0x%04X MinorFunction: 0x%04X\n",
			irp->MajorFunction, irp->MinorFunction);

	switch (irp->MajorFunction)
	{
		case IRP_MJ_CREATE:
			serial_process_irp_create(serial, irp);
			break;

		case IRP_MJ_CLOSE:
			serial_process_irp_close(serial, irp);
			break;

		case IRP_MJ_READ:
			serial_process_irp_read(serial, irp);
			break;

		case IRP_MJ_WRITE:
			serial_process_irp_write(serial, irp);
			break;

		case IRP_MJ_DEVICE_CONTROL:
			serial_process_irp_device_control(serial, irp);
			break;

		default:
			DEBUG_WARN("MajorFunction 0x%X not supported", irp->MajorFunction);
			irp->IoStatus = STATUS_NOT_SUPPORTED;
			irp->Complete(irp);
			break;
	}
}

static void* serial_thread_func(void* arg)
{
	IRP* irp;
	wMessage message;
	SERIAL_DEVICE* drive = (SERIAL_DEVICE*) arg;

	while (1)
	{
		if (!MessageQueue_Wait(drive->IrpQueue))
			break;

		if (!MessageQueue_Peek(drive->IrpQueue, &message, TRUE))
			break;

		if (message.id == WMQ_QUIT)
			break;

		irp = (IRP*) message.wParam;

		if (irp)
			serial_process_irp(drive, irp);
	}

	ExitThread(0);
	return NULL;
}

static void serial_irp_request(DEVICE* device, IRP* irp)
{
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*) device;
	MessageQueue_Post(serial->IrpQueue, NULL, 0, (void*) irp, NULL);
}

static void serial_free(DEVICE* device)
{
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*) device;

	WLog_Print(serial->log, WLOG_DEBUG, "freeing");

	MessageQueue_PostQuit(serial->IrpQueue, 0);
	WaitForSingleObject(serial->thread, INFINITE);
	CloseHandle(serial->thread);

	serial_tty_free(serial->tty);

	/* Clean up resources */
	Stream_Free(serial->device.data, TRUE);
	MessageQueue_Free(serial->IrpQueue);

	free(serial);
}

#ifdef STATIC_CHANNELS
#define DeviceServiceEntry	serial_DeviceServiceEntry
#endif

int DeviceServiceEntry(PDEVICE_SERVICE_ENTRY_POINTS pEntryPoints)
{
	int i, len;
	char* name;
	char* path;
	RDPDR_SERIAL* device;
	SERIAL_DEVICE* serial;

	device = (RDPDR_SERIAL*) pEntryPoints->device;
	name = device->Name;
	path = device->Path;

	if (!name || (name[0] == '*'))
	{
		/* TODO: implement auto detection of parallel ports */
		return 0;
	}

	if ((name && name[0]) && (path && path[0]))
	{
		if (!DefineCommDevice(name /* eg: COM1 */, path /* eg: /dev/ttyS0 */))
		{
			DEBUG_SVC("Could not registered: %s as %s", path, name);
			return -1;
		}

		serial = (SERIAL_DEVICE*) calloc(1, sizeof(SERIAL_DEVICE));
		if (!serial)
			return -1;

		serial->device.type = RDPDR_DTYP_SERIAL;
		serial->device.name = name;
		serial->device.IRPRequest = serial_irp_request;
		serial->device.Free = serial_free;

		len = strlen(name);
		serial->device.data = Stream_New(NULL, len + 1);

		for (i = 0; i <= len; i++)
			Stream_Write_UINT8(serial->device.data, name[i] < 0 ? '_' : name[i]);

		serial->path = path;
		serial->IrpQueue = MessageQueue_New(NULL);

		WLog_Init();
		serial->log = WLog_Get("com.freerdp.channel.serial.client");
		WLog_Print(serial->log, WLOG_DEBUG, "initializing");

		pEntryPoints->RegisterDevice(pEntryPoints->devman, (DEVICE*) serial);

		serial->thread = CreateThread(NULL, 0, 
				(LPTHREAD_START_ROUTINE) serial_thread_func, (void*) serial, 0, NULL);
	}

	return 0;
}
