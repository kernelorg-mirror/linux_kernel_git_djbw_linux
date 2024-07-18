// SPDX-License-Identifier: GPL-2.0
/*
 * TEE Security Manager for the TEE Device Interface Security Protocol
 * (TDISP, PCIe r6.1 sec 11)
 *
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#define dev_fmt(fmt) "TSM: " fmt

#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/sysfs.h>
#include <linux/xarray.h>
#include <linux/pci-tsm.h>
#include <linux/bitfield.h>
#include "pci.h"

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a tsm instance
 */
static DECLARE_RWSEM(pci_tsm_rwsem);
static const struct pci_tsm_ops *tsm_ops;

/* supplemental attributes to surface when pci_tsm_attr_group is active */
static const struct attribute_group *pci_tsm_owner_attr_group;

static int pci_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held_read(&pci_tsm_rwsem);
	scoped_cond_guard(mutex_intr, return -EINTR, &pci_tsm->exec_lock) {
		int rc;

		if (pci_tsm->state < PCI_TSM_CONNECT)
			return 0;

		rc = tsm_ops->exec(pdev, TSM_EXEC_DISCONNECT);
		if (rc)
			return rc;
		pci_tsm->state = PCI_TSM_INIT;
	}
	return 0;
}

static int pci_tsm_connect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held_read(&pci_tsm_rwsem);
	scoped_cond_guard(mutex_intr, return -EINTR, &pci_tsm->exec_lock) {
		int rc;

		if (pci_tsm->state >= PCI_TSM_CONNECT)
			return 0;

		rc = tsm_ops->exec(pdev, TSM_EXEC_CONNECT);
		if (rc)
			return rc;
		pci_tsm->state = PCI_TSM_CONNECT;
	}
	return 0;
}

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	int rc;
	bool connect;
	struct pci_dev *pdev = to_pci_dev(dev);

	rc = kstrtobool(buf, &connect);
	if (rc)
		return rc;

	if (connect)
		rc = pci_tsm_connect(pdev);
	else
		rc = pci_tsm_disconnect(pdev);
	if (rc)
		return rc;
	return len;
}

static ssize_t connect_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return sysfs_emit(buf, "%d\n", pdev->tsm->state >= PCI_TSM_CONNECT);
}
static DEVICE_ATTR_RW(connect);

static bool pci_tsm_group_visible(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->tsm)
		return true;
	return false;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_tsm);

static struct attribute *pci_tsm_attrs[] = {
	&dev_attr_connect.attr,
	NULL,
};

const struct attribute_group pci_tsm_attr_group = {
	.name = "tsm",
	.attrs = pci_tsm_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),
};

static ssize_t authenticated_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	/*
	 * When device authentication is TSM owned, 'authenticated' is
	 * identical to the connect state.
	 */
	return connect_show(dev, attr, buf);
}
static DEVICE_ATTR_RO(authenticated);

static struct attribute *pci_tsm_auth_attrs[] = {
	&dev_attr_authenticated.attr,
	NULL,
};

const struct attribute_group pci_tsm_auth_attr_group = {
	.attrs = pci_tsm_auth_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),
};

static void __pci_tsm_init(struct pci_dev *pdev)
{
	bool tee_cap;
	u16 ide_cap;
	bool tsm_attach = false;

	if (pdev->is_virtfn)
		return;

	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	tee_cap = pdev->devcap & PCI_EXP_DEVCAP_TEE;

	if (!(ide_cap || tee_cap))
		return;

	lockdep_assert_held_write(&pci_tsm_rwsem);
	if (!tsm_ops)
		return;

	/*
	 * If a physical device has any security capabilities it may be
	 * a candidate to connect with the platform TSM
	 */
	if (ide_cap && tee_cap) {
		/* Common case all TSM implementations are expected to support */
		tsm_attach = true;
	}

	if (tsm_ops->probe) {
		/*
		 * Optional TSM support for IDE without TDISP (standalone IDE),
		 * or TDISP without IDE (TSM trusted interconnect), or TSM
		 * policy to filter @pdev.
		 */
		tsm_attach = tsm_ops->probe(pdev) == 0;
	}

	pci_dbg(pdev, "Device security capabilities detected (%s%s ), TSM %s\n",
		ide_cap ? " ide" : "", tee_cap ? " tee" : "",
		tsm_attach ? "attach" : "skip");

	if (!tsm_attach)
		return;

	struct pci_tsm *pci_tsm __free(kfree) = kzalloc(sizeof(*pci_tsm), GFP_KERNEL);
	if (!pci_tsm)
		return;

	pci_tsm->ide_cap = ide_cap;
	mutex_init(&pci_tsm->exec_lock);
	pci_tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					       PCI_DOE_PROTO_CMA);
	if (!pci_tsm->doe_mb) {
		pci_warn(pdev, "TSM init failure, no CMA mailbox\n");
		return;
	}

	pdev->tsm = no_free_ptr(pci_tsm);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	if (pci_tsm_owner_attr_group)
		sysfs_merge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
}

void pci_tsm_init(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_init(pdev);
}

int pci_tsm_register(const struct pci_tsm_ops *ops, const struct attribute_group *grp)
{
	struct pci_dev *pdev;

	if (!ops)
		return 0;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (tsm_ops)
		return -EBUSY;
	tsm_ops = ops;
	pci_tsm_owner_attr_group = grp;
	for_each_pci_dev(pdev)
		__pci_tsm_init(pdev);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_register);

static void __pci_tsm_destroy(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held_write(&pci_tsm_rwsem);
	pdev->tsm = NULL;
	if (pci_tsm_owner_attr_group)
		sysfs_unmerge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	kfree(pci_tsm);
}

void pci_tsm_destroy(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_destroy(pdev);
}

void pci_tsm_unregister(const struct pci_tsm_ops *ops)
{
	struct pci_dev *pdev;

	if (!ops)
		return;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (ops != tsm_ops)
		return;
	for_each_pci_dev(pdev)
		__pci_tsm_destroy(pdev);
	tsm_ops = NULL;
}
EXPORT_SYMBOL_GPL(pci_tsm_unregister);

int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz)
{
	if (!pdev->tsm || !pdev->tsm->doe_mb)
		return -ENXIO;

	return pci_doe(pdev->tsm->doe_mb, PCI_VENDOR_ID_PCI_SIG, type, req,
		       req_sz, resp, resp_sz);
}
EXPORT_SYMBOL_GPL(pci_tsm_doe_transfer);
