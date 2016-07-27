/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2015-2016 Intel Corp.
 * (Written by Alexandru Gagniuc <alexandrux.gagniuc@intel.com> for Intel Corp.)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <arch/early_variables.h>
#include <arch/io.h>
#include <cbmem.h>
#include <commonlib/helpers.h>
#include <console/console.h>
#include <fsp/util.h>
#include <inttypes.h>
#include <lib.h>
#include <string.h>

#define HOB_HEADER_LEN		8

enum resource_type {
	EFI_RESOURCE_SYSTEM_MEMORY		= 0,
	EFI_RESOURCE_MEMORY_MAPPED_IO		= 1,
	EFI_RESOURCE_IO				= 2,
	EFI_RESOURCE_FIRMWARE_DEVICE		= 3,
	EFI_RESOURCE_MEMORY_MAPPED_IO_PORT	= 4,
	EFI_RESOURCE_MEMORY_RESERVED		= 5,
	EFI_RESOURCE_IO_RESERVED		= 6,
	EFI_RESOURCE_MAX_MEMORY_TYPE		= 7,
};

/* GUIDs in little-endian, so they can be used with memcmp() */
const uint8_t fsp_reserved_memory_guid[16] = {
	0x59, 0x97, 0xa7, 0x69, 0x73, 0x13, 0x67, 0x43,
	0xa6, 0xc4, 0xc7, 0xf5, 0x9e, 0xfd, 0x98, 0x6e,
};

const uint8_t fsp_nv_storage_guid[16] = {
       0x02, 0xcf, 0x1a, 0x72, 0x77, 0x4d, 0x2a, 0x4c,
       0xb3, 0xdc, 0x27, 0x0b, 0x7b, 0xa9, 0xe4, 0xb0
};

/*
 * Utilities for walking HOBs
 */

bool fsp_guid_compare(const uint8_t guid1[16], const uint8_t guid2[16])
{
	return !memcmp(guid1, guid2, 16);
}

const struct hob_header *fsp_next_hob(const struct hob_header *parent)
{
	union {
		const struct hob_header *hob;
		uintptr_t addr;
	} hob_walker;

	hob_walker.hob = parent;
	hob_walker.addr += parent->length;
	return hob_walker.hob;
}

static const void *hob_header_to_struct(const struct hob_header *hob)
{
	union {
		const struct hob_header *hob_hdr;
		const void *hob_descr;
		uintptr_t addr;
	} hob_walker;

	hob_walker.hob_hdr = hob;
	hob_walker.addr += HOB_HEADER_LEN;
	return hob_walker.hob_descr;
}

static const void *hob_header_to_extension_hob(const struct hob_header *hob)
{
	union {
		const struct hob_header *hob_hdr;
		const void *hob_descr;
		uintptr_t addr;
	} hob_walker;

	hob_walker.hob_hdr = hob;
	hob_walker.addr += HOB_HEADER_LEN + 16; /* header and 16-byte GUID */
	return hob_walker.hob_descr;
}

const
struct hob_resource *fsp_hob_header_to_resource(const struct hob_header *hob)
{
	return hob_header_to_struct(hob);
}

/*
 * Utilities for locating and identifying HOBs
 */

static void *fsp_hob_list_ptr CAR_GLOBAL;

static void save_hob_list(int is_recovery)
{
	uint32_t *cbmem_loc;
	cbmem_loc = cbmem_add(CBMEM_ID_FSP_RUNTIME, sizeof(*cbmem_loc));
	*cbmem_loc = (uintptr_t)fsp_get_hob_list();
}

ROMSTAGE_CBMEM_INIT_HOOK(save_hob_list);

const void *fsp_get_hob_list(void)
{
	uint32_t *list_loc;

	if (ENV_ROMSTAGE)
		return (void *)car_get_var(fsp_hob_list_ptr);
	list_loc = cbmem_find(CBMEM_ID_FSP_RUNTIME);
	return (list_loc) ? (void *)(uintptr_t)(*list_loc) : NULL;
}

void *fsp_get_hob_list_ptr(void)
{
	return car_get_var_ptr(&fsp_hob_list_ptr);
}

static const
struct hob_resource *find_resource_hob_by_guid(const struct hob_header *hob,
					       const uint8_t guid[16])
{
	const struct hob_resource *res;

	for ( ; hob->type != HOB_TYPE_END_OF_HOB_LIST;
		hob = fsp_next_hob(hob)) {

		if (hob->type != HOB_TYPE_RESOURCE_DESCRIPTOR)
			continue;

		res = fsp_hob_header_to_resource(hob);
		if (fsp_guid_compare(res->owner_guid, guid))
			return res;
	}
	return NULL;
}

void fsp_find_reserved_memory(struct range_entry *re)
{
	const struct hob_resource *fsp_mem;
	const void *hob_list = fsp_get_hob_list();

	range_entry_init(re, 0, 0, 0);

	fsp_mem = find_resource_hob_by_guid(hob_list, fsp_reserved_memory_guid);

	if (!fsp_mem) {
		return;
	}

	range_entry_init(re, fsp_mem->addr, fsp_mem->addr + fsp_mem->length, 0);
}

const void *fsp_find_extension_hob_by_guid(const uint8_t *guid, size_t *size)
{
	const uint8_t *hob_guid;
	const struct hob_header *hob = fsp_get_hob_list();

	if (!hob)
		return NULL;

	for ( ; hob->type != HOB_TYPE_END_OF_HOB_LIST;
		hob = fsp_next_hob(hob)) {

		if (hob->type != HOB_TYPE_GUID_EXTENSION)
			continue;

		hob_guid = hob_header_to_struct(hob);
		if (fsp_guid_compare(hob_guid, guid)) {
			*size = hob->length - (HOB_HEADER_LEN + 16);
			return hob_header_to_extension_hob(hob);
		}
	}

	return NULL;
}

const void *fsp_find_nv_storage_data(size_t *size)
{
	return fsp_find_extension_hob_by_guid(fsp_nv_storage_guid, size);
}
