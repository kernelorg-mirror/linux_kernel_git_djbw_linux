// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.
#include <linux/module.h>
#include <linux/compiler.h>
#include <linux/jump_label.h>
#include <linux/moduleparam.h>

enum {
	F_IFENCE,
};

#ifdef CONFIG_SPECTRE1_IFENCE
static unsigned long nospec_flag = 1 << F_IFENCE;
DEFINE_STATIC_KEY_TRUE(nospec_key);
#else
static unsigned long nospec_flag;
DEFINE_STATIC_KEY_FALSE(nospec_key);
#endif

EXPORT_SYMBOL(nospec_key);

static int param_set_nospec(const char *val, const struct kernel_param *kp)
{
	unsigned long *flags = kp->arg;

	if (strcmp(val, "ifence") == 0 || strcmp(val, "ifence\n") == 0) {
		if (!test_and_set_bit(F_IFENCE, flags))
			static_key_enable(&nospec_key.key);
		return 0;
	} else if (strcmp(val, "mask") == 0 || strcmp(val, "mask\n") == 0) {
		if (test_and_clear_bit(F_IFENCE, flags))
			static_key_disable(&nospec_key.key);
		return 0;
	}
	return -EINVAL;
}

static int param_get_nospec(char *buffer, const struct kernel_param *kp)
{
	unsigned long *flags = kp->arg;

	return sprintf(buffer, "%s\n", test_bit(F_IFENCE, flags)
			? "ifence" : "mask");
}

static struct kernel_param_ops nospec_param_ops = {
	.set = param_set_nospec,
	.get = param_get_nospec,
};

core_param_cb(spectre_v1, &nospec_param_ops, &nospec_flag, 0600);
MODULE_PARM_DESC(spectre_v1, "Spectre-v1 mitigation: 'mask' (default) vs 'ifence'");
