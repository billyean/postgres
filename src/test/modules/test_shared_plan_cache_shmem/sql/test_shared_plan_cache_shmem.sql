CREATE EXTENSION test_shared_plan_cache_shmem;

-- T1/T2: Default startup: backend attached and active
SELECT test_shared_plan_cache_attached() AS attached;
SELECT test_shared_plan_cache_active() AS active;

-- T3: dshash handles are valid
SELECT test_shared_plan_cache_handles_valid() AS handles_valid;

-- T9: No entries yet
SELECT test_shared_plan_cache_current_entries() AS current_entries;

-- T7: Repeated attach is idempotent
SELECT test_shared_plan_cache_force_attach() AS still_attached;
SELECT test_shared_plan_cache_active() AS still_active;
