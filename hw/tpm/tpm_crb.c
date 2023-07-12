/*
 * tpm_crb.c - QEMU's TPM CRB interface emulator
 *
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * Authors:
 *   Marc-André Lureau <marcandre.lureau@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * tpm_crb is a device for TPM 2.0 Command Response Buffer (CRB) Interface
 * as defined in TCG PC Client Platform TPM Profile (PTP) Specification
 * Family “2.0” Level 00 Revision 01.03 v22
 */

#include "qemu/osdep.h"

#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/acpi/acpi_aml_interface.h"
#include "hw/acpi/tpm.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci_ids.h"
#include "hw/acpi/tpm.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "sysemu/tpm_backend.h"
#include "sysemu/tpm_util.h"
#include "sysemu/reset.h"
#include "sysemu/xen.h"
#include "tpm_prop.h"
#include "tpm_ppi.h"
#include "trace.h"
#include "qom/object.h"
#include "tpm_crb.h"

struct CRBState {
    ISADevice parent_obj;

    TPMCRBState state;
};
typedef struct CRBState CRBState;

DECLARE_INSTANCE_CHECKER(CRBState, CRB,
                         TYPE_TPM_CRB)

static void tpm_crb_isa_request_completed(TPMIf *ti, int ret)
{
    CRBState *s = CRB(ti);

    tpm_crb_request_completed(&s->state, ret);
}

static enum TPMVersion tpm_crb_isa_get_version(TPMIf *ti)
{
    CRBState *s = CRB(ti);

    return tpm_crb_get_version(&s->state);
}

static int tpm_crb_isa_pre_save(void *opaque)
{
    CRBState *s = opaque;

    return tpm_crb_pre_save(&s->state);
}

static const VMStateDescription vmstate_tpm_crb_isa = {
    .name = "tpm-crb",
    .pre_save = tpm_crb_isa_pre_save,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST(),
    }
};

static Property tpm_crb_isa_properties[] = {
    DEFINE_PROP_TPMBE("tpmdev", CRBState, state.tpmbe),
    DEFINE_PROP_BOOL("ppi", CRBState, state.ppi_enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void tpm_crb_isa_reset(void *dev)
{
    CRBState *s = CRB(dev);

    return tpm_crb_reset(&s->state, TPM_CRB_ADDR_BASE);
}

static void tpm_crb_isa_realize(DeviceState *dev, Error **errp)
{
    CRBState *s = CRB(dev);

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }
    if (!s->state.tpmbe) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }

    tpm_crb_init_memory(OBJECT(s), &s->state, errp);

    memory_region_add_subregion(isa_address_space(ISA_DEVICE(dev)),
        TPM_CRB_ADDR_BASE, &s->state.mmio);

    if (s->state.ppi_enabled) {
        memory_region_add_subregion(isa_address_space(ISA_DEVICE(dev)),
            TPM_PPI_ADDR_BASE, &s->state.ppi.ram);
    }

    if (xen_enabled()) {
        tpm_crb_isa_reset(dev);
    } else {
        qemu_register_reset(tpm_crb_isa_reset, dev);
    }
}

static void build_tpm_crb_isa_aml(AcpiDevAmlIf *adev, Aml *scope)
{
    Aml *dev, *crs;
    CRBState *s = CRB(adev);
    TPMIf *ti = TPM_IF(s);

    dev = aml_device("TPM");
    if (tpm_crb_isa_get_version(ti) == TPM_VERSION_2_0) {
        aml_append(dev, aml_name_decl("_HID", aml_string("MSFT0101")));
        aml_append(dev, aml_name_decl("_STR", aml_string("TPM 2.0 Device")));
    } else {
        aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0C31")));
    }
    aml_append(dev, aml_name_decl("_UID", aml_int(1)));
    aml_append(dev, aml_name_decl("_STA", aml_int(0xF)));
    crs = aml_resource_template();
    aml_append(crs, aml_memory32_fixed(TPM_CRB_ADDR_BASE, TPM_CRB_ADDR_SIZE,
                                      AML_READ_WRITE));
    aml_append(dev, aml_name_decl("_CRS", crs));
    tpm_build_ppi_acpi(ti, dev);
    aml_append(scope, dev);
}

static void tpm_crb_isa_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    TPMIfClass *tc = TPM_IF_CLASS(klass);
    AcpiDevAmlIfClass *adevc = ACPI_DEV_AML_IF_CLASS(klass);

    dc->realize = tpm_crb_isa_realize;
    device_class_set_props(dc, tpm_crb_isa_properties);
    dc->vmsd  = &vmstate_tpm_crb_isa;
    dc->user_creatable = true;
    tc->model = TPM_MODEL_TPM_CRB;
    tc->get_version = tpm_crb_isa_get_version;
    tc->request_completed = tpm_crb_isa_request_completed;
    adevc->build_dev_aml = build_tpm_crb_isa_aml;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo tpm_crb_isa_info = {
    .name = TYPE_TPM_CRB,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(CRBState),
    .class_init  = tpm_crb_isa_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_TPM_IF },
        { TYPE_ACPI_DEV_AML_IF },
        { }
    }
};

static void tpm_crb_isa_register(void)
{
    type_register_static(&tpm_crb_isa_info);
}

type_init(tpm_crb_isa_register)
