/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd. All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#include <linux/kobject.h>
#include <linux/types.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#include <linux/mm.h>
#endif /* < KERNEL_VERSION(4, 12, 0) */

#include "include/defex_debug.h"
#include "include/defex_internal.h"
#include "include/defex_rules.h"
#include "include/defex_sign.h"

#define DEFEX_RULES_FILE "/dpolicy"

/*
 * Variant 1: Platform build, use static packed rules array
 */
#include "defex_packed_rules.inc"

#ifdef DEFEX_RAMDISK_ENABLE
/*
 * Variant 2: Platform build, load rules from kernel ramdisk or system partition
 */
#ifdef DEFEX_SIGN_ENABLE
#include "include/defex_sign.h"
#endif
#if (DEFEX_RULES_ARRAY_SIZE < 8)
#undef DEFEX_RULES_ARRAY_SIZE
#define DEFEX_RULES_ARRAY_SIZE	sizeof(struct rule_item_struct)
#endif
#ifdef DEFEX_KERNEL_ONLY
#undef DEFEX_RULES_ARRAY_SIZE
#define DEFEX_RULES_ARRAY_SIZE	(256 * 1024)
static unsigned char defex_packed_rules[DEFEX_RULES_ARRAY_SIZE] = {0};
#else
static unsigned char defex_packed_rules[DEFEX_RULES_ARRAY_SIZE] __ro_after_init = {0};
#endif /* DEFEX_KERNEL_ONLY */
#endif /* DEFEX_RAMDISK_ENABLE */


#ifdef DEFEX_INTEGRITY_ENABLE

#include <linux/fs.h>
#include <crypto/hash.h>
#include <crypto/public_key.h>
#include <crypto/internal/rsa.h>
#include "../../integrity/integrity.h"
#define SHA256_DIGEST_SIZE 32
#endif /* DEFEX_INTEGRITY_ENABLE */

static int is_recovery;

__visible_for_testing int __init bootmode_setup(char *str)
{
	if (str && *str == '2') {
		is_recovery = 1;
		pr_alert("[DEFEX] recovery mode setup\n");
	}
	return 0;
}
__setup("bootmode=", bootmode_setup);

int check_rules_ready(void)
{
	struct rule_item_struct *base = (struct rule_item_struct *)defex_packed_rules;

	return (!base || !base->data_size)?0:1;
}

__visible_for_testing int check_system_mount(void)
{
	static int mount_system_root = -1;
	struct file *fp;

	if (mount_system_root < 0) {
		fp = local_fopen("/sbin/recovery", O_RDONLY, 0);
		if (IS_ERR(fp))
			fp = local_fopen("/system/bin/recovery", O_RDONLY, 0);

		if (!IS_ERR(fp)) {
			pr_alert("[DEFEX] recovery mode\n");
			filp_close(fp, NULL);
			is_recovery = 1;
		} else {
			pr_alert("[DEFEX] normal mode\n");
		}

		mount_system_root = 0;
		fp = local_fopen("/system_root", O_DIRECTORY | O_PATH, 0);
		if (!IS_ERR(fp)) {
			filp_close(fp, NULL);
			mount_system_root = 1;
			pr_alert("[DEFEX] system_root=TRUE\n");
		} else {
			pr_alert("[DEFEX] system_root=FALSE\n");
		}
	}
	return (mount_system_root > 0);
}

#ifdef DEFEX_INTEGRITY_ENABLE
__visible_for_testing int defex_check_integrity(struct file *f, unsigned char *hash)
{
	struct crypto_shash *handle = NULL;
	struct shash_desc *shash = NULL;
	static const unsigned char buff_zero[SHA256_DIGEST_SIZE] = {0};
	unsigned char hash_sha256[SHA256_DIGEST_SIZE];
	unsigned char *buff = NULL;
	size_t buff_size = PAGE_SIZE;
	loff_t file_size = 0;
	int ret = 0, err = 0, read_size = 0;

	// A saved hash is zero, skip integrity check
	if (!memcmp(buff_zero, hash, SHA256_DIGEST_SIZE))
		return ret;

	if (IS_ERR(f))
		goto hash_error;

	handle = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		pr_err("[DEFEX] Can't alloc sha256, error : %d", err);
		return -1;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	shash = (struct shash_desc *)kvzalloc(sizeof(struct shash_desc) + crypto_shash_descsize(handle), GFP_KERNEL);
#else
	shash = kzalloc(sizeof(struct shash_desc) + crypto_shash_descsize(handle), GFP_KERNEL);
#endif /* < KERNEL_VERSION(4, 12, 0) */

	if (shash == NULL)
		goto hash_error;

	shash->tfm = handle;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	buff = kvmalloc(buff_size, GFP_KERNEL);
#else
	buff = kmalloc(buff_size, GFP_KERNEL);
#endif /* < KERNEL_VERSION(4, 12, 0) */

	if (buff == NULL)
		goto hash_error;

	err = crypto_shash_init(shash);
	if (err < 0)
		goto hash_error;


	while (1) {
		read_size = local_fread(f, file_size, (char *)buff, buff_size);
		if (read_size < 0)
			goto hash_error;
		if (read_size == 0)
			break;
		file_size += read_size;
		err = crypto_shash_update(shash, buff, read_size);
		if (err < 0)
			goto hash_error;
	}

	err = crypto_shash_final(shash, hash_sha256);
	if (err < 0)
		goto hash_error;

	ret = memcmp(hash_sha256, hash, SHA256_DIGEST_SIZE);

	goto hash_exit;

hash_error:
	ret = -1;
hash_exit:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	kvfree(buff);
	kvfree(shash);
#else
	kfree(buff);
	kfree(shash);
#endif /* < KERNEL_VERSION(4, 12, 0) */

	if (handle)
		crypto_free_shash(handle);
	return ret;

}

__visible_for_testing int defex_integrity_default(const char *file_path)
{
	static const char integrity_default[] = "/system/bin/install-recovery.sh";

	return strncmp(integrity_default, file_path, sizeof(integrity_default));
}

#endif /* DEFEX_INTEGRITY_ENABLE */

#if defined(DEFEX_RAMDISK_ENABLE)

#ifdef DEFEX_KERNEL_ONLY
int load_rules_late(void)
{
	struct file *f;
	int data_size, rules_size, res = 0;
	unsigned char *data_buff = NULL;
	static unsigned long start_time;
	static unsigned long last_time;
	unsigned long cur_time = get_seconds();
	static DEFINE_SPINLOCK(load_lock);
	static atomic_t in_progress = ATOMIC_INIT(0);

	if (!spin_trylock(&load_lock))
		return res;

	if (atomic_read(&in_progress) != 0) {
		spin_unlock(&load_lock);
		return res;
	}

	atomic_set(&in_progress, 1);
	spin_unlock(&load_lock);

	/* The first try to load, initialize time values */
	if (!start_time)
		start_time = get_seconds();
	/* Skip this try, wait for next second */
	if (cur_time == last_time)
		goto do_exit;
	/* Load has been attempted for 30 seconds, give up. */
	if ((cur_time - start_time) > 30) {
		res = -1;
		goto do_exit;
	}
	last_time = cur_time;

	f = local_fopen(DEFEX_RULES_FILE, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("[DEFEX] Failed to open rules file (%ld)\n", (long)PTR_ERR(f));
		goto do_exit;
	}
	data_size = i_size_read(file_inode(f));
	if (data_size <= 0 || data_size > (sizeof(defex_packed_rules) << 1))
		goto do_clean;
	data_buff = vmalloc(data_size);
	if (!data_buff)
		goto do_clean;

	rules_size = local_fread(f, 0, data_buff, data_size);
	if (rules_size <= 0) {
		pr_err("[DEFEX] Failed to read rules file (%d)\n", rules_size);
		goto do_clean;
	}
	pr_info("[DEFEX] Late load rules file: %s.\n", DEFEX_RULES_FILE);
	pr_info("[DEFEX] Read %d bytes.\n", rules_size);
	if (rules_size > sizeof(defex_packed_rules))
		rules_size = sizeof(defex_packed_rules);
	memcpy(defex_packed_rules, data_buff, rules_size);
	res = (rules_size > 0);
do_clean:
	filp_close(f, NULL);
	vfree(data_buff);
do_exit:
	atomic_set(&in_progress, 0);
	return res;
}
#endif /* DEFEX_KERNEL_ONLY */

__visible_for_testing int __init do_load_rules(void)
{
	struct file *f;
	int res = -1, data_size, rules_size;
	unsigned char *data_buff = NULL;

	memset(defex_packed_rules, 0, sizeof(defex_packed_rules));
	pr_info("[DEFEX] Load rules file: %s.\n", DEFEX_RULES_FILE);
	f = local_fopen(DEFEX_RULES_FILE, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("[DEFEX] Failed to open rules file (%ld)\n", (long)PTR_ERR(f));
#ifdef DEFEX_KERNEL_ONLY
		if (is_recovery)
			res = 0;
#endif /* DEFEX_KERNEL_ONLY */
		return res;
	}
	data_size = i_size_read(file_inode(f));
	if (data_size <= 0 || data_size > (sizeof(defex_packed_rules) << 1))
		goto do_clean;
	data_buff = vmalloc(data_size);
	if (!data_buff)
		goto do_clean;

	rules_size = local_fread(f, 0, data_buff, data_size);
	if (rules_size <= 0) {
		pr_err("[DEFEX] Failed to read rules file (%d)\n", rules_size);
		goto do_clean;
	}
	pr_info("[DEFEX] Read %d bytes.\n", rules_size);

#ifdef DEFEX_SIGN_ENABLE
	res = defex_rules_signature_check(data_buff, rules_size, &rules_size);
	if (!res && rules_size > sizeof(defex_packed_rules))
		res = -1;

	if (!res)
		pr_info("[DEFEX] Rules signature verified successfully.\n");
	else
		pr_err("[DEFEX] Rules signature incorrect!!!\n");
#else
	res = 0;
#endif

	if (!res)
		memcpy(defex_packed_rules, data_buff, rules_size);
do_clean:
	filp_close(f, NULL);
	vfree(data_buff);

#ifdef DEFEX_KERNEL_ONLY
	if (is_recovery && res != 0) {
		res = 0;
		pr_info("[DEFEX] Kernel Only & recovery mode, rules loading is passed.\n");
	}
#endif

	return res;
}

#endif /* DEFEX_RAMDISK_ENABLE */

__visible_for_testing struct rule_item_struct *lookup_dir(struct rule_item_struct *base,
								const char *name, int l, int for_recovery)
{
	struct rule_item_struct *item = NULL;
	unsigned int offset;

	if (!base || !base->next_level)
		return item;
	item = GET_ITEM_PTR(base->next_level);
	do {
		if ((!(item->feature_type & feature_is_file)
			 || (!!(item->feature_type & feature_for_recovery)) == for_recovery)
				&& item->size == l && !memcmp(name, item->name, l))
			return item;
		offset = item->next_file;
		item = GET_ITEM_PTR(offset);
	} while (offset);
	return NULL;
}

__visible_for_testing int lookup_tree(const char *file_path, int attribute, struct file *f)
{
	const char *ptr, *next_separator;
	struct rule_item_struct *base, *cur_item = NULL;
	int l;

	if (!file_path || *file_path != '/')
		return 0;

#ifdef DEFEX_KERNEL_ONLY
try_to_load:
#endif
	base = (struct rule_item_struct *)defex_packed_rules;
	if (!base || !base->data_size) {
#ifdef DEFEX_KERNEL_ONLY
		/* allow all requests if rules were not loaded for Recovery mode */
		l = load_rules_late();
		if (l > 0)
			goto try_to_load;
		if (!l || is_recovery)
			return (attribute == feature_ped_exception || attribute == feature_safeplace_path)?1:0;
#endif /* DEFEX_KERNEL_ONLY */
		/* block all requests if rules were not loaded instead */
		return 0;
	}

	ptr = file_path + 1;
	do {
		next_separator = strchr(ptr, '/');
		if (!next_separator)
			l = strlen(ptr);
		else
			l = next_separator - ptr;
		if (!l)
			return 0;
		cur_item = lookup_dir(base, ptr, l, is_recovery);
		if (!cur_item)
			cur_item = lookup_dir(base, ptr, l, !is_recovery);
		if (!cur_item)
			break;
		if (cur_item->feature_type & attribute) {
#ifdef DEFEX_INTEGRITY_ENABLE
			/* Integrity acceptable only for files */
			if ((cur_item->feature_type & feature_is_file) && f) {
				if (defex_integrity_default(file_path)
					&& defex_check_integrity(f, cur_item->integrity))
					return DEFEX_INTEGRITY_FAIL;
			}
#endif /* DEFEX_INTEGRITY_ENABLE */
			if (attribute & (feature_immutable_path_open | feature_immutable_path_write)
				&& !(cur_item->feature_type & feature_is_file)) {
				/* Allow open the folder by default */
				if (!next_separator || *(ptr + l + 1) == 0)
					return 0;
			}
			return 1;
		}
		base = cur_item;
		ptr += l;
		if (next_separator)
			ptr++;
	} while (*ptr);
	return 0;
}

int rules_lookup(const char *target_file, int attribute, struct file *f)
{
	int ret = 0;
#if (defined(DEFEX_SAFEPLACE_ENABLE) || defined(DEFEX_IMMUTABLE_ENABLE) || defined(DEFEX_PED_ENABLE))
	static const char system_root_txt[] = "/system_root";

	if (check_system_mount() &&
		!strncmp(target_file, system_root_txt, sizeof(system_root_txt) - 1))
		target_file += (sizeof(system_root_txt) - 1);

	ret = lookup_tree(target_file, attribute, f);
#endif
	return ret;
}

void __init defex_load_rules(void)
{
#if defined(DEFEX_RAMDISK_ENABLE)
	if (!boot_state_unlocked && do_load_rules() != 0) {
#if !(defined(DEFEX_DEBUG_ENABLE) || defined(DEFEX_KERNEL_ONLY))
		panic("[DEFEX] Signature mismatch.\n");
#endif
	}
#endif /* DEFEX_RAMDISK_ENABLE */
}
