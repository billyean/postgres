CREATE EXTENSION test_shared_plan_cache_serialization;
SET plan_cache_mode = force_generic_plan;

-- ===== Round-trip tests =====

-- T1: Simple SELECT
PREPARE rt1 AS SELECT 1;
SELECT test_shared_plan_roundtrip('rt1') AS roundtrip_select_1;
DEALLOCATE rt1;

-- T2: Permanent table SELECT
CREATE TABLE ser_test_t (id int, val text);
PREPARE rt2 AS SELECT * FROM ser_test_t;
SELECT test_shared_plan_roundtrip('rt2') AS roundtrip_table;

-- T3: Parameterized SELECT
PREPARE rt3(int) AS SELECT $1;
SELECT test_shared_plan_roundtrip('rt3') AS roundtrip_param;
DEALLOCATE rt3;

-- T4: Join query
CREATE TABLE ser_test_t2 (id int, data text);
PREPARE rt4 AS SELECT * FROM ser_test_t t1 JOIN ser_test_t2 t2 ON t1.id = t2.id;
SELECT test_shared_plan_roundtrip('rt4') AS roundtrip_join;
DEALLOCATE rt4;

-- T5: Aggregate query
PREPARE rt5 AS SELECT count(*), sum(id) FROM ser_test_t GROUP BY val;
SELECT test_shared_plan_roundtrip('rt5') AS roundtrip_agg;
DEALLOCATE rt5;

-- T6: Subquery
PREPARE rt6 AS SELECT * FROM ser_test_t WHERE id IN (SELECT id FROM ser_test_t2);
SELECT test_shared_plan_roundtrip('rt6') AS roundtrip_subquery;
DEALLOCATE rt6;

-- ===== DSA round-trip =====

-- T6b: DSA round-trip
PREPARE dsa1 AS SELECT * FROM ser_test_t t1 JOIN ser_test_t2 t2 ON t1.id = t2.id;
SELECT test_shared_plan_roundtrip_dsa('dsa1') AS roundtrip_dsa;
DEALLOCATE dsa1;

-- ===== Size test =====

-- T7: Serialized size is positive
PREPARE sz1 AS SELECT * FROM ser_test_t;
SELECT test_shared_plan_serialized_size('sz1') > 0 AS size_positive;
DEALLOCATE sz1;

-- T8: Oversize rejected with artificial 1kB limit
PREPARE sz2 AS SELECT * FROM ser_test_t t1 JOIN ser_test_t2 t2 ON t1.id = t2.id;
SELECT test_shared_plan_serialize_oversize('sz2', 1) AS oversize_result;
DEALLOCATE sz2;

DEALLOCATE rt2;
DROP TABLE ser_test_t2;
DROP TABLE ser_test_t;

-- ===== Shareability tests (with exact rejection reason) =====

-- T9: Temp table => NOT_SHAREABLE:TEMP_OBJECT
CREATE TEMP TABLE ser_tmp (x int);
PREPARE sh1 AS SELECT * FROM ser_tmp;
SELECT test_shared_plan_serialize_cached_plan('sh1') AS temp_table_result;
DEALLOCATE sh1;
DROP TABLE ser_tmp;

-- T10: RLS table => NOT_SHAREABLE:DEPENDS_ON_RLS
CREATE TABLE ser_rls (x int);
ALTER TABLE ser_rls ENABLE ROW LEVEL SECURITY;
ALTER TABLE ser_rls FORCE ROW LEVEL SECURITY;
CREATE POLICY p ON ser_rls USING (true);
CREATE ROLE regress_ser_rls_user;
GRANT SELECT ON ser_rls TO regress_ser_rls_user;
SET ROLE regress_ser_rls_user;
PREPARE sh2 AS SELECT * FROM ser_rls;
SELECT test_shared_plan_serialize_cached_plan('sh2') AS rls_result;
DEALLOCATE sh2;
RESET ROLE;
REVOKE SELECT ON ser_rls FROM regress_ser_rls_user;
DROP ROLE regress_ser_rls_user;
DROP POLICY p ON ser_rls;
ALTER TABLE ser_rls DISABLE ROW LEVEL SECURITY;
ALTER TABLE ser_rls NO FORCE ROW LEVEL SECURITY;
DROP TABLE ser_rls;

-- ===== Corrupt payload tests =====

-- T11: Too-short buffer
SELECT test_shared_plan_deserialize_corrupt('too_short') AS corrupt_too_short;

-- T12: Wrong magic
SELECT test_shared_plan_deserialize_corrupt('wrong_magic') AS corrupt_wrong_magic;

-- T13: Wrong version
SELECT test_shared_plan_deserialize_corrupt('wrong_version') AS corrupt_wrong_version;

-- T13b: Wrong format
SELECT test_shared_plan_deserialize_corrupt('wrong_format') AS corrupt_wrong_format;

-- T14: Payload length mismatch
SELECT test_shared_plan_deserialize_corrupt('payload_len_mismatch') AS corrupt_payload_len;

-- T15: num_stmts mismatch
SELECT test_shared_plan_deserialize_corrupt('num_stmts_mismatch') AS corrupt_num_stmts;

-- T15b: Non-list payload
SELECT test_shared_plan_deserialize_corrupt('non_list_payload') AS corrupt_non_list;

-- T15c: List element not PlannedStmt
SELECT test_shared_plan_deserialize_corrupt('list_non_plannedstmt') AS corrupt_non_pstmt;

-- T15d: Unparseable text (caught ERROR => PARSE_ERROR)
SELECT test_shared_plan_deserialize_corrupt('unparseable_text') AS corrupt_unparseable;

-- ===== Timing (informational) =====

-- T16: Timing report
PREPARE tm1 AS SELECT 1;
SELECT serialized_bytes > 0 AS timing_ok
FROM test_shared_plan_serialize_timing('tm1', 10);
DEALLOCATE tm1;

RESET plan_cache_mode;
