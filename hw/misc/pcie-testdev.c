/*
 * QEMU PCIe test device
 *
 * Copyright (c) 2012 Red Hat Inc.
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pcie.h"
#include "qemu/event_notifier.h"
#include "qemu/module.h"
#include "system/kvm.h"
#include "qom/object.h"
#include "hw/pci/msix.h"

typedef struct PCIeTestDevHdr {
    uint8_t test;
    uint8_t width;
    uint8_t pad0[2];
    uint32_t offset;
    uint8_t data;
    uint8_t pad1[3];
    uint32_t count;
    uint8_t name[];
} PCIeTestDevHdr;

typedef struct IOTest {
    MemoryRegion *mr;
    EventNotifier notifier;
    bool hasnotifier;
    unsigned size;
    bool match_data;
    PCIeTestDevHdr *hdr;
    unsigned bufsize;
} IOTest;

#define IOTEST_DATAMATCH 0xFA
#define IOTEST_NOMATCH   0xCE

#define IOTEST_MEMSIZE 2048

static const char *iotest_test[] = {
    "no-eventfd",
    "wildcard-eventfd",
    "datamatch-eventfd"
};

static const char *iotest_type[] = {
    "mmio",
};

#define IOTEST_TEST(i) (iotest_test[((i) % ARRAY_SIZE(iotest_test))])
#define IOTEST_TYPE(i) (iotest_type[((i) / ARRAY_SIZE(iotest_test))])
#define IOTEST_MAX_TEST (ARRAY_SIZE(iotest_test))
#define IOTEST_MAX_TYPE (ARRAY_SIZE(iotest_type))
#define IOTEST_MAX (IOTEST_MAX_TEST * IOTEST_MAX_TYPE)

enum {
    IOTEST_ACCESS_NAME,
    IOTEST_ACCESS_DATA,
    IOTEST_ACCESS_MAX,
};

#define IOTEST_ACCESS_TYPE uint8_t
#define IOTEST_ACCESS_WIDTH (sizeof(uint8_t))

struct PCIeTestDevState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion mmio;
    IOTest *tests;
    int current;

    uint64_t membar_size;
    bool membar_backed;
    MemoryRegion membar;

    PCIDevice *pcidev;
    int msix_enabled;
    MemoryRegion msix;
};

#define TYPE_PCIE_TEST_DEV "pcie-testdev"

OBJECT_DECLARE_SIMPLE_TYPE(PCIeTestDevState, PCIE_TEST_DEV)

#define IOTEST_REGION(d) (&(d)->mmio)
#define IOTEST_SIZE(i)  (IOTEST_MEMSIZE)
#define IOTEST_PCI_BAR(i) (PCI_BASE_ADDRESS_SPACE_MEMORY)

static int pcie_testdev_start(IOTest *test)
{
    test->hdr->count = 0;
    if (!test->hasnotifier) {
        return 0;
    }
    event_notifier_test_and_clear(&test->notifier);
    memory_region_add_eventfd(test->mr,
                              le32_to_cpu(test->hdr->offset),
                              test->size,
                              test->match_data,
                              test->hdr->data,
                              &test->notifier);
    return 0;
}

static void pcie_testdev_stop(IOTest *test)
{
    if (!test->hasnotifier) {
        return;
    }
    memory_region_del_eventfd(test->mr,
                              le32_to_cpu(test->hdr->offset),
                              test->size,
                              test->match_data,
                              test->hdr->data,
                              &test->notifier);
}

static void
pcie_testdev_reset(PCIeTestDevState *d)
{
    if (d->current == -1) {
        return;
    }
    pcie_testdev_stop(&d->tests[d->current]);
    d->current = -1;
}

static void pcie_testdev_inc(IOTest *test, unsigned inc)
{
    uint32_t c = le32_to_cpu(test->hdr->count);
    test->hdr->count = cpu_to_le32(c + inc);
}

static void
pcie_testdev_write(void *opaque, hwaddr addr, uint64_t val,
                  unsigned size, int type)
{
    PCIeTestDevState *d = opaque;
    IOTest *test;
    int t, r;

    if (addr == offsetof(PCIeTestDevHdr, test)) {
        pcie_testdev_reset(d);
        if (val >= IOTEST_MAX_TEST) {
            return;
        }
        t = type * IOTEST_MAX_TEST + val;
        r = pcie_testdev_start(&d->tests[t]);
        if (r < 0) {
            return;
        }
        d->current = t;
        return;
    }
    if (d->current < 0) {
        return;
    }
    test = &d->tests[d->current];
    if (addr != le32_to_cpu(test->hdr->offset)) {
        return;
    }
    if (test->match_data && test->size != size) {
        return;
    }
    if (test->match_data && val != test->hdr->data) {
        return;
    }
    pcie_testdev_inc(test, 1);
}

static uint64_t
pcie_testdev_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIeTestDevState *d = opaque;
    const char *buf;
    IOTest *test;
    if (d->current < 0) {
        return 0;
    }
    test = &d->tests[d->current];
    buf = (const char *)test->hdr;
    if (addr + size >= test->bufsize) {
        return 0;
    }
    if (test->hasnotifier) {
        event_notifier_test_and_clear(&test->notifier);
    }
    return buf[addr];
}

static void
pcie_testdev_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    pcie_testdev_write(opaque, addr, val, size, 0);
}

static const MemoryRegionOps pcie_testdev_mmio_ops = {
    .read = pcie_testdev_read,
    .write = pcie_testdev_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void pcie_testdev_realize(PCIDevice *pci_dev, Error **errp)
{
    PCIeTestDevState *d = PCIE_TEST_DEV(pci_dev);
    Error *err = NULL;
    uint8_t *pci_conf;
    char *name;
    int r, i;

    pci_conf = pci_dev->config;

    d->pcidev = pci_dev;

    pci_conf[PCI_INTERRUPT_PIN] = 0; /* no interrupt pin */
    pci_conf[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL;

    int ret = pcie_cap_init(pci_dev, 0x40, PCI_EXP_TYPE_ENDPOINT, 0, &err);
    if (ret < 0 && err) {
        error_reportf_err(*errp, "Failed to initialize PCIe capabilities: %s",
                     error_get_pretty(err));
        return;
    }

    memory_region_init_io(&d->mmio, OBJECT(d), &pcie_testdev_mmio_ops, d,
                          "pci-testdev-mmio", IOTEST_MEMSIZE * 2);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

    if (d->membar_backed)
        memory_region_init_ram(&d->membar, OBJECT(d),
                               "pci-testdev-membar-backed",
                               d->membar_size, NULL);
    else
        memory_region_init(&d->membar, OBJECT(d),
                           "pci-testdev-membar",
                           d->membar_size);
    pci_register_bar(pci_dev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &d->membar);

    msix_init_exclusive_bar(pci_dev, d->msix_enabled, 1, errp);

    d->current = -1;
    d->tests = g_malloc0(IOTEST_MAX * sizeof *d->tests);
    for (i = 0; i < IOTEST_MAX; ++i) {
        IOTest *test = &d->tests[i];
        name = g_strdup_printf("%s-%s", IOTEST_TYPE(i), IOTEST_TEST(i));
        test->bufsize = sizeof(PCIeTestDevHdr) + strlen(name) + 1;
        test->hdr = g_malloc0(test->bufsize);
        memcpy(test->hdr->name, name, strlen(name) + 1);
        g_free(name);
        test->hdr->offset = cpu_to_le32(IOTEST_SIZE(i) + i * IOTEST_ACCESS_WIDTH);
        test->match_data = strcmp(IOTEST_TEST(i), "wildcard-eventfd");
        if (!test->match_data) {
            test->size = 0;
        } else {
            test->size = IOTEST_ACCESS_WIDTH;
        }
        test->hdr->test = i;
        test->hdr->data = test->match_data ? IOTEST_DATAMATCH : IOTEST_NOMATCH;
        test->hdr->width = IOTEST_ACCESS_WIDTH;
        test->mr = IOTEST_REGION(d);
        if (!strcmp(IOTEST_TEST(i), "no-eventfd")) {
            test->hasnotifier = false;
            continue;
        }
        r = event_notifier_init(&test->notifier, 0);
        assert(r >= 0);
        test->hasnotifier = true;
    }
}

static void
pcie_testdev_uninit(PCIDevice *dev)
{
    PCIeTestDevState *d = PCIE_TEST_DEV(dev);
    int i;

    pcie_testdev_reset(d);
    for (i = 0; i < IOTEST_MAX; ++i) {
        if (d->tests[i].hasnotifier) {
            event_notifier_cleanup(&d->tests[i].notifier);
        }
        g_free(d->tests[i].hdr);
    }
    g_free(d->tests);
}

static void qdev_pcie_testdev_reset(DeviceState *dev)
{
    PCIeTestDevState *d = PCIE_TEST_DEV(dev);
/*    msix_reset(d->pcidev); */
    pcie_testdev_reset(d);
}

static const Property pcie_testdev_properties[] = {
    DEFINE_PROP_SIZE("membar", PCIeTestDevState, membar_size, 0),
    DEFINE_PROP_BOOL("membar-backed", PCIeTestDevState, membar_backed, false),
    DEFINE_PROP_INT32("msix-vectors", PCIeTestDevState, msix_enabled, 0),
};

static void pcie_testdev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pcie_testdev_realize;
    k->exit = pcie_testdev_uninit;
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = 0xA000;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_DRM_ACCEL;
    dc->desc = "PCIe Test Device";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_legacy_reset(dc, qdev_pcie_testdev_reset);
    device_class_set_props(dc, pcie_testdev_properties);
}

static const TypeInfo pcie_testdev_info = {
    .name          = TYPE_PCIE_TEST_DEV,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIeTestDevState),
    .class_init    = pcie_testdev_class_init,
    .interfaces = (const InterfaceInfo[]) {
        {  INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void pcie_testdev_register_types(void)
{
    type_register_static(&pcie_testdev_info);
}

type_init(pcie_testdev_register_types)
