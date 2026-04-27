--
-- Tests for buffered-insert adoption in INSERT INTO ... SELECT (Patch 0005).
-- Restricted first step: non-partitioned heap target, no ON CONFLICT,
-- no RETURNING, no BEFORE ROW triggers.
--

-- ============================================================
-- T1: Basic bulk insert (exercises multiple auto-flush cycles)
-- ============================================================
CREATE TABLE bi_target_basic (id int, val text);

INSERT INTO bi_target_basic
SELECT g, 'row-' || g FROM generate_series(1, 2000) g;

SELECT count(*) FROM bi_target_basic;
SELECT min(id), max(id) FROM bi_target_basic;

DROP TABLE bi_target_basic;

-- ============================================================
-- T2: Indexed target
-- ============================================================
CREATE TABLE bi_target_idx (id int, val text);
CREATE INDEX bi_target_idx_id ON bi_target_idx (id);

INSERT INTO bi_target_idx
SELECT g, 'row-' || g FROM generate_series(1, 500) g;

SELECT count(*) FROM bi_target_idx;

-- Verify index is usable and correct
SET enable_seqscan = off;
SELECT count(*) FROM bi_target_idx WHERE id BETWEEN 1 AND 500;
RESET enable_seqscan;

DROP TABLE bi_target_idx;

-- ============================================================
-- T3: AFTER ROW trigger
-- ============================================================
CREATE TABLE bi_target_trig (id int, val text);
CREATE TABLE bi_audit (id int, val text, logged_at timestamp DEFAULT now());

CREATE FUNCTION bi_audit_fn() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO bi_audit (id, val) VALUES (NEW.id, NEW.val);
    RETURN NEW;
END;
$$;

CREATE TRIGGER bi_target_trig_after
    AFTER INSERT ON bi_target_trig
    FOR EACH ROW EXECUTE FUNCTION bi_audit_fn();

INSERT INTO bi_target_trig
SELECT g, 'row-' || g FROM generate_series(1, 50) g;

SELECT count(*) FROM bi_target_trig;
SELECT count(*) FROM bi_audit;

-- Verify insertion order is preserved
SELECT bool_and(t.id = a.id) AS order_preserved
FROM (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM bi_target_trig) t
JOIN (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM bi_audit) a
ON t.rn = a.rn;

DROP TABLE bi_target_trig CASCADE;
DROP TABLE bi_audit;
DROP FUNCTION bi_audit_fn;

-- ============================================================
-- T4: Index + AFTER ROW trigger combined
-- ============================================================
CREATE TABLE bi_target_combo (id int, val text);
CREATE INDEX bi_target_combo_id ON bi_target_combo (id);
CREATE TABLE bi_audit_combo (id int, val text);

CREATE FUNCTION bi_audit_combo_fn() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO bi_audit_combo (id, val) VALUES (NEW.id, NEW.val);
    RETURN NEW;
END;
$$;

CREATE TRIGGER bi_target_combo_after
    AFTER INSERT ON bi_target_combo
    FOR EACH ROW EXECUTE FUNCTION bi_audit_combo_fn();

INSERT INTO bi_target_combo
SELECT g, 'row-' || g FROM generate_series(1, 100) g;

SELECT count(*) FROM bi_target_combo;
SELECT count(*) FROM bi_audit_combo;

-- Verify index correctness
SET enable_seqscan = off;
SELECT count(*) FROM bi_target_combo WHERE id BETWEEN 1 AND 100;
RESET enable_seqscan;

DROP TABLE bi_target_combo CASCADE;
DROP TABLE bi_audit_combo;
DROP FUNCTION bi_audit_combo_fn;

-- ============================================================
-- T5: ON CONFLICT fallback (uses non-buffered path)
-- ============================================================
CREATE TABLE bi_target_conflict (id int PRIMARY KEY, val text);

INSERT INTO bi_target_conflict VALUES (1, 'existing');

INSERT INTO bi_target_conflict
SELECT g, 'row-' || g FROM generate_series(1, 10) g
ON CONFLICT (id) DO NOTHING;

SELECT count(*) FROM bi_target_conflict;
SELECT val FROM bi_target_conflict WHERE id = 1;

DROP TABLE bi_target_conflict;

-- ============================================================
-- T6: RETURNING fallback (uses non-buffered path)
-- ============================================================
CREATE TABLE bi_target_ret (id int, val text);

INSERT INTO bi_target_ret
SELECT g, 'row-' || g FROM generate_series(1, 3) g
RETURNING id, val;

SELECT count(*) FROM bi_target_ret;

DROP TABLE bi_target_ret;

-- ============================================================
-- T7: BEFORE ROW trigger fallback (uses non-buffered path)
-- ============================================================
CREATE TABLE bi_target_br (id int, val text);

CREATE FUNCTION bi_br_fn() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    NEW.val := NEW.val || '-modified';
    RETURN NEW;
END;
$$;

CREATE TRIGGER bi_target_br_before
    BEFORE INSERT ON bi_target_br
    FOR EACH ROW EXECUTE FUNCTION bi_br_fn();

INSERT INTO bi_target_br
SELECT g, 'row-' || g FROM generate_series(1, 5) g;

SELECT count(*) FROM bi_target_br;
SELECT val FROM bi_target_br WHERE id = 1;

DROP TABLE bi_target_br;
DROP FUNCTION bi_br_fn;

-- ============================================================
-- T8: Partitioned target fallback (uses non-buffered path)
-- ============================================================
CREATE TABLE bi_target_part (id int, val text) PARTITION BY RANGE (id);
CREATE TABLE bi_target_part_1 PARTITION OF bi_target_part FOR VALUES FROM (1) TO (501);
CREATE TABLE bi_target_part_2 PARTITION OF bi_target_part FOR VALUES FROM (501) TO (1001);

INSERT INTO bi_target_part
SELECT g, 'row-' || g FROM generate_series(1, 1000) g;

SELECT count(*) FROM bi_target_part;
SELECT count(*) FROM bi_target_part_1;
SELECT count(*) FROM bi_target_part_2;

DROP TABLE bi_target_part;

-- ============================================================
-- T9: Volatile target-default fallback
-- Expected to fall back to non-buffered path under E8.
-- Test validates correctness; path selection is not observable
-- from SQL output.
-- ============================================================
CREATE TABLE bi_target_volatile (
    id int,
    val text,
    rand_val double precision DEFAULT random()
);

INSERT INTO bi_target_volatile (id, val)
SELECT g, 'row-' || g FROM generate_series(1, 5) g;

SELECT count(*) FROM bi_target_volatile;
-- Verify the volatile default was evaluated (all values should be distinct)
SELECT count(DISTINCT rand_val) = count(*) AS all_distinct
FROM bi_target_volatile;

DROP TABLE bi_target_volatile;

-- ============================================================
-- T10: Zero-row insert
-- ============================================================
CREATE TABLE bi_target_zero (id int, val text);

INSERT INTO bi_target_zero
SELECT g, 'row-' || g FROM generate_series(1, 100) g WHERE false;

SELECT count(*) FROM bi_target_zero;

DROP TABLE bi_target_zero;
