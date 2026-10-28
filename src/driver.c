#include "hv.h"
#include <wdmsec.h>

#define DEVICE_NAME L"\\Device\\Ophion"
#define SYMLINK_NAME L"\\DosDevices\\Ophion"
static const GUID OphionDeviceClass =
    {0x17a34ba8, 0xa06c, 0x4c93, {0xaf, 0xa4, 0x21, 0xdb, 0xf7, 0x39, 0xe1, 0x67}};

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR bytes)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS dispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    OPH_PACKET *p;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(device);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return complete(irp, STATUS_INVALID_DEVICE_STATE, 0);
    switch (s->MajorFunction) {
    case IRP_MJ_CREATE:
        if (s->FileObject->FileName.Length) return complete(irp, STATUS_OBJECT_NAME_INVALID, 0);
        return complete(irp, lab_open(s->FileObject), 0);
    case IRP_MJ_CLEANUP:
    case IRP_MJ_CLOSE:
        lab_close(s->FileObject);
        return complete(irp, STATUS_SUCCESS, 0);
    case IRP_MJ_DEVICE_CONTROL:
        if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_OPH_EXCHANGE)
            return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (irp->RequestorMode != UserMode || IoIs32bitProcess(irp) ||
            IoGetRequestorProcess(irp) != PsGetCurrentProcess())
            return complete(irp, STATUS_ACCESS_DENIED, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength != sizeof(OPH_PACKET) ||
            s->Parameters.DeviceIoControl.OutputBufferLength != sizeof(OPH_PACKET))
            return complete(irp, STATUS_INFO_LENGTH_MISMATCH, 0);
        p = irp->AssociatedIrp.SystemBuffer;
        if (!p) return complete(irp, STATUS_INVALID_USER_BUFFER, 0);
        if (p->version != OPH_VERSION) return complete(irp, STATUS_REVISION_MISMATCH, 0);
        p->root_sequence = p->root_tsc = 0;
        status = lab_ioctl(s->FileObject, p);
        return complete(irp, status, NT_SUCCESS(status) ? sizeof(*p) : 0);
    default: return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

VOID DriverUnload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING name;
    broadcast_terminate_all();
    vmx_terminate();
    RtlInitUnicodeString(&name, SYMLINK_NAME);
    IoDeleteSymbolicLink(&name);
    IoDeleteDevice(driver->DeviceObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    PDEVICE_OBJECT device = NULL;
    UNICODE_STRING name, link, sddl;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(registry);
    lab_initialize();
    RtlInitUnicodeString(&name, DEVICE_NAME);
    RtlInitUnicodeString(&link, SYMLINK_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = IoCreateDeviceSecure(driver, 0, &name, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, TRUE, &sddl, &OphionDeviceClass, &device);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = dispatch;
    driver->DriverUnload = DriverUnload;
    if (!vmx_init()) {
        vmx_terminate();
        IoDeleteDevice(device);
        return STATUS_HV_OPERATION_FAILED;
    }
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) {
        broadcast_terminate_all(); vmx_terminate(); IoDeleteDevice(device);
        return status;
    }
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
