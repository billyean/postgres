/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_race_protection.c
 *	  Test code for shared plan cache race protection (Patch 0010).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_race_protection/test_shared_plan_cache_race_protection.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/prepare.h"
#include "fmgr.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/plancache.h"
#include "utils/shared_plancache.h"
#include "varatt.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_spc_entry_refcount);
PG_FUNCTION_INFO_V1(test_spc_set_entry_refcount);
PG_FUNCTION_INFO_V1(test_spc_l2_stale_after_deser_count);
PG_FUNCTION_INFO_V1(test_spc_last_l2_status);
PG_FUNCTION_INFO_V1(test_spc_has_pin);
PG_FUNCTION_INFO_V1(test_spc_force_nested_pin);
PG_FUNCTION_INFO_V1(test_spc_clear_nested_pin);
PG_FUNCTION_INFO_V1(test_spc_release_pin);
PG_FUNCTION_INFO_V1(test_spc_arm_race_is_valid);
PG_FUNCTION_INFO_V1(test_spc_arm_race_generation);
PG_FUNCTION_INFO_V1(test_spc_arm_race_epoch);
PG_FUNCTION_INFO_V1(test_spc_arm_race_payload);
PG_FUNCTION_INFO_V1(test_spc_entry_refcount_by_key);
PG_FUNCTION_INFO_V1(test_spc_race_l2_hit_count);
PG_FUNCTION_INFO_V1(test_spc_race_l2_miss_count);
PG_FUNCTION_INFO_V1(test_spc_race_l2_store_count);
PG_FUNCTION_INFO_V1(test_spc_race_current_entries);
PG_FUNCTION_INFO_V1(test_spc_race_current_generation);
PG_FUNCTION_INFO_V1(test_spc_race_current_store_epoch);
PG_FUNCTION_INFO_V1(test_spc_race_entry_is_valid_for_prep);
PG_FUNCTION_INFO_V1(test_spc_race_force_relcache_invalidation);
PG_FUNCTION_INFO_V1(test_spc_hook_fired_count);
PG_FUNCTION_INFO_V1(test_spc_arm_deser_error);
PG_FUNCTION_INFO_V1(test_spc_arm_deser_elog_error);
PG_FUNCTION_INFO_V1(test_spc_force_real_pin);
PG_FUNCTION_INFO_V1(test_spc_capture_key);
PG_FUNCTION_INFO_V1(test_spc_set_entry_refcount_by_key);
PG_FUNCTION_INFO_V1(test_spc_force_same_key_overwrite);

/* State for race simulation hooks */
static SharedPlanKey armed_key;
static bool key_armed = false;
static int armed_inval_type = 0;

static bool
compute_key_for_prep(const char *stmt_name, SharedPlanKey *key)
{
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	SharedPlanRejectReason reject;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	if (plansource->query_list == NIL)
		return false;

	return ComputeSharedPlanKeyForLookup(plansource, key, &reject);
}

static void
race_simulation_hook(void)
{
	dshash_table *hash = SharedPlanCacheGetHash();
	SharedPlanCacheControl *ctl = SharedPlanCacheGetControl();
	SharedPlanEntry *entry;

	if (!key_armed || hash == NULL || ctl == NULL)
		return;

	switch (armed_inval_type)
	{
		case 1:
			/* T2/T4: mark entry is_valid=0 */
			entry = dshash_find(hash, &armed_key, false);
			if (entry == NULL)
				goto fail_closed;
			pg_atomic_write_u32(&entry->is_valid, 0);
			dshash_release_lock(hash, entry);
			break;

		case 2:
			/* T3: bump global generation */
			pg_atomic_fetch_add_u64(&ctl->generation, 1);
			break;

		case 3:
			/*
			 * T13: specific-relid invalidation + same-key overwrite.
			 * Mark is_valid=0, bump epoch, then re-set is_valid=1 with
			 * same generation (simulates Backend C re-store).
			 */
			entry = dshash_find(hash, &armed_key, false);
			if (entry == NULL)
				goto fail_closed;
			pg_atomic_write_u32(&entry->is_valid, 0);
			dshash_release_lock(hash, entry);
			pg_atomic_fetch_add_u64(&ctl->relcache_store_epoch, 1);
			entry = dshash_find(hash, &armed_key, false);
			if (entry == NULL)
				goto fail_closed;
			pg_atomic_write_u32(&entry->is_valid, 1);
			dshash_release_lock(hash, entry);
			break;

		case 4:
			/*
			 * T14: force a payload identity mismatch during second validation
			 * without corrupting the real entry.  Uses a test-only flag that
			 * causes second validation to observe a mismatch.
			 */
			SharedPlanCacheTestArmPayloadMismatch();
			break;

		default:
			break;
	}

	key_armed = false;
	armed_inval_type = 0;
	shared_plan_cache_after_pin_hook = NULL;
	return;

fail_closed:
	key_armed = false;
	armed_inval_type = 0;
	shared_plan_cache_after_pin_hook = NULL;
}

static bool
arm_race_hook(const char *stmt_name, int inval_type)
{
	if (!compute_key_for_prep(stmt_name, &armed_key))
		return false;

	armed_inval_type = inval_type;
	key_armed = true;
	shared_plan_cache_after_pin_hook = race_simulation_hook;
	return true;
}

/* ---- SQL-callable functions ---- */

Datum
test_spc_entry_refcount(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanEntry *entry;
	dshash_table *hash;
	int32		result = -1;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_INT32(-1);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_INT32(-1);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_INT32(-1);

	result = (int32) pg_atomic_read_u32(&entry->refcount);
	dshash_release_lock(hash, entry);

	PG_RETURN_INT32(result);
}

Datum
test_spc_set_entry_refcount(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	uint32		value = (uint32) PG_GETARG_INT64(1);
	SharedPlanKey key;
	SharedPlanEntry *entry;
	dshash_table *hash;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_BOOL(false);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	pg_atomic_write_u32(&entry->refcount, value);
	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(true);
}

Datum
test_spc_l2_stale_after_deser_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2StaleAfterDeserCount());
}

Datum
test_spc_last_l2_status(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(SharedPlanCacheLastL2StatusName()));
}

Datum
test_spc_has_pin(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheHasPin());
}

/*
 * test_spc_force_nested_pin - simulate a held pin for nested-pin testing.
 *
 * Sets current_refcount_held=true with the key of the named prepared stmt,
 * WITHOUT incrementing the shared entry's refcount.  The next L2 lookup
 * must detect the nested pin and return MISS.
 */
Datum
test_spc_force_nested_pin(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_BOOL(false);

	SharedPlanCacheTestSetPinHeld(&key);
	PG_RETURN_BOOL(true);
}

/*
 * test_spc_clear_nested_pin - clear a simulated pin without refcount decrement.
 */
Datum
test_spc_clear_nested_pin(PG_FUNCTION_ARGS)
{
	SharedPlanCacheTestClearPinHeld();
	PG_RETURN_VOID();
}

Datum
test_spc_release_pin(PG_FUNCTION_ARGS)
{
	SharedPlanCacheReleasePin();
	PG_RETURN_BOOL(!SharedPlanCacheHasPin());
}

/* Race simulation arm functions */

Datum
test_spc_arm_race_is_valid(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(arm_race_hook(stmt_name, 1));
}

Datum
test_spc_arm_race_generation(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(arm_race_hook(stmt_name, 2));
}

Datum
test_spc_arm_race_epoch(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(arm_race_hook(stmt_name, 3));
}

Datum
test_spc_arm_race_payload(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(arm_race_hook(stmt_name, 4));
}

Datum
test_spc_entry_refcount_by_key(PG_FUNCTION_ARGS)
{
	bytea	   *key_bytes = PG_GETARG_BYTEA_PP(0);
	SharedPlanKey key;
	SharedPlanEntry *entry;
	dshash_table *hash;
	int32		result = -1;

	if (VARSIZE_ANY_EXHDR(key_bytes) != sizeof(SharedPlanKey))
		ereport(ERROR, (errmsg("invalid key size")));

	memcpy(&key, VARDATA_ANY(key_bytes), sizeof(SharedPlanKey));

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_INT32(-1);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_INT32(-1);

	result = (int32) pg_atomic_read_u32(&entry->refcount);
	dshash_release_lock(hash, entry);

	PG_RETURN_INT32(result);
}

Datum
test_spc_hook_fired_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheTestHookFiredCount());
}

Datum
test_spc_arm_deser_error(PG_FUNCTION_ARGS)
{
	SharedPlanCacheTestArmDeserError();
	PG_RETURN_BOOL(true);
}

Datum
test_spc_arm_deser_elog_error(PG_FUNCTION_ARGS)
{
	SharedPlanCacheTestArmDeserElogError();
	PG_RETURN_BOOL(true);
}

/*
 * test_spc_force_real_pin - create a real pin (refcount incremented + tracking set).
 * Takes a prepared statement name, computes its key, then calls the helper.
 */
Datum
test_spc_force_real_pin(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(SharedPlanCacheTestForceRealPin(&key));
}

/* Counter/accessor duplicates for self-contained test module */

Datum
test_spc_race_l2_hit_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2HitCount());
}

Datum
test_spc_race_l2_miss_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2MissCount());
}

Datum
test_spc_race_l2_store_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2StoreCount());
}

Datum
test_spc_race_current_entries(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SharedPlanCacheCurrentEntries());
}

Datum
test_spc_race_current_generation(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) SharedPlanCacheGeneration());
}

Datum
test_spc_race_current_store_epoch(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) SharedPlanCacheRelcacheStoreEpoch());
}

Datum
test_spc_race_entry_is_valid_for_prep(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanEntry *entry;
	bool		result = false;
	dshash_table *hash;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_BOOL(false);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	result = (pg_atomic_read_u32(&entry->is_valid) == 1 &&
			  entry->generation == SharedPlanCacheGeneration());
	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(result);
}

Datum
test_spc_race_force_relcache_invalidation(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	if (relid == InvalidOid)
		CacheInvalidateRelcacheAll();
	else
		CacheInvalidateRelcacheByRelid(relid);

	PG_RETURN_VOID();
}

/*
 * test_spc_capture_key - capture the SharedPlanKey for a prepared stmt as bytea.
 */
Datum
test_spc_capture_key(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	bytea	   *result;

	if (!compute_key_for_prep(stmt_name, &key))
		PG_RETURN_NULL();

	result = (bytea *) palloc(VARHDRSZ + sizeof(SharedPlanKey));
	SET_VARSIZE(result, VARHDRSZ + sizeof(SharedPlanKey));
	memcpy(VARDATA(result), &key, sizeof(SharedPlanKey));

	PG_RETURN_BYTEA_P(result);
}

/*
 * test_spc_set_entry_refcount_by_key - set refcount on an entry identified by captured key.
 */
Datum
test_spc_set_entry_refcount_by_key(PG_FUNCTION_ARGS)
{
	bytea	   *key_bytes = PG_GETARG_BYTEA_PP(0);
	uint32		value = (uint32) PG_GETARG_INT64(1);
	SharedPlanKey key;
	SharedPlanEntry *entry;
	dshash_table *hash;

	if (VARSIZE_ANY_EXHDR(key_bytes) != sizeof(SharedPlanKey))
		ereport(ERROR, (errmsg("invalid key size")));

	memcpy(&key, VARDATA_ANY(key_bytes), sizeof(SharedPlanKey));

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	pg_atomic_write_u32(&entry->refcount, value);
	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(true);
}

/*
 * test_spc_force_same_key_overwrite - controlled same-entry refcount
 * preservation helper.
 *
 * Verifies that same-entry stale overwrite must not reset refcount.
 * It does not test full payload replacement.  Production refcount
 * preservation is enforced by code review and by ensuring refcount
 * initialization occurs only for new entries (see SharedPlanPublishEntry).
 *
 * Marks the entry stale (is_valid=0), updates generation, then re-publishes
 * (is_valid=1).  Does NOT touch refcount.  Never creates entries.
 *
 * Returns true if the entry was found and touched, false otherwise.
 */
Datum
test_spc_force_same_key_overwrite(PG_FUNCTION_ARGS)
{
	bytea	   *key_bytes = PG_GETARG_BYTEA_PP(0);
	SharedPlanKey key;
	SharedPlanEntry *entry;
	dshash_table *hash;
	SharedPlanCacheControl *ctl;

	if (VARSIZE_ANY_EXHDR(key_bytes) != sizeof(SharedPlanKey))
		ereport(ERROR, (errmsg("invalid key size")));

	memcpy(&key, VARDATA_ANY(key_bytes), sizeof(SharedPlanKey));

	hash = SharedPlanCacheGetHash();
	ctl = SharedPlanCacheGetControl();
	if (hash == NULL || ctl == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * Mark stale and re-publish with current generation.
	 * Refcount is deliberately NOT touched — this is the invariant under test.
	 */
	pg_atomic_write_u32(&entry->is_valid, 0);
	entry->generation = pg_atomic_read_u64(&ctl->generation);
	pg_atomic_write_u32(&entry->is_valid, 1);

	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(true);
}
