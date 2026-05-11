# Test pg_pipeline_graph diagnostic PlanState classifier.
#
# This TAP test verifies that the extension loads, the GUC works,
# and representative queries produce expected classification patterns
# in DEBUG1 output.
#
# v1 is a one-node-per-fragment diagnostic classifier, not a true
# multi-node pipeline builder.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Helper: run query with classifier enabled, return combined output.
# Uses LOAD to ensure the library is loaded in the session.
# GUC is PGC_SUSET so we use the superuser connection (default).
sub run_classified
{
	my ($sql) = @_;
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		"LOAD 'pg_pipeline_graph';\n"
			. "SET pg_pipeline_graph.enabled = on;\n"
			. "SET client_min_messages = debug1;\n"
			. $sql);
	is($ret, 0, "query succeeded: $sql");
	my $combined = ($stdout // '') . "\n" . ($stderr // '');
	return $combined;
}

# Test 1: Extension loads and GUC is recognized
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"LOAD 'pg_pipeline_graph'; SHOW pg_pipeline_graph.enabled;");
is($ret, 0, 'LOAD succeeds');
like($stdout, qr/off/, 'GUC pg_pipeline_graph.enabled defaults to off');

# Test 2: Simple SeqScan classification
$node->safe_psql('postgres',
	"CREATE TABLE pgpg_t (id int, val text); "
		. "INSERT INTO pgpg_t SELECT g, 'r' || g FROM generate_series(1,5) g; "
		. "ANALYZE pgpg_t;");

my $out = run_classified("SELECT * FROM pgpg_t WHERE id > 2;");
like($out, qr/PIPELINE CLASSIFIER/, 'produces PIPELINE CLASSIFIER header');
like($out, qr/SeqScan/, 'classifies SeqScan');
like($out, qr/source=BASE_SCAN/, 'SeqScan is BASE_SCAN source');
like($out, qr/qual=Y/, 'SeqScan has qual annotation');

# Test 3: Constant Result (synthetic source)
$out = run_classified("SELECT 1 AS x;");
like($out, qr/PIPELINE CLASSIFIER/, 'Result: PIPELINE CLASSIFIER header');
like($out, qr/Result/, 'classifies Result node');
like($out, qr/source=SYNTHETIC/, 'childless Result is SYNTHETIC source');

# Test 4: ValuesScan
$out = run_classified(
	"SELECT * FROM (VALUES (1,'a'),(2,'b')) AS t(id,name);");
like($out, qr/PIPELINE CLASSIFIER/, 'ValuesScan: PIPELINE CLASSIFIER header');
like($out, qr/ValuesScan/, 'classifies ValuesScan');
like($out, qr/source=VALUES/, 'ValuesScan is VALUES source');

# Test 5: Sort blocking boundary
$out = run_classified(
	"SET enable_sort = on; SELECT * FROM pgpg_t ORDER BY val;");
like($out, qr/PIPELINE CLASSIFIER/, 'Sort: PIPELINE CLASSIFIER header');
like($out, qr/Sort/, 'classifies Sort node');
like($out, qr/boundary=BLOCKING/, 'Sort is BLOCKING boundary');
like($out, qr/source=BREAKER_OUTPUT/, 'Sort has BREAKER_OUTPUT source');

# Test 6: HashJoin / Hash
$node->safe_psql('postgres',
	"CREATE TABLE pgpg_t2 (id int, ref int); "
		. "INSERT INTO pgpg_t2 SELECT g, g FROM generate_series(1,5) g; "
		. "ANALYZE pgpg_t2;");

$out = run_classified(
	"SET enable_nestloop=off; SET enable_mergejoin=off; "
		. "SELECT * FROM pgpg_t t1 JOIN pgpg_t2 t2 ON t1.id = t2.ref;");
like($out, qr/PIPELINE CLASSIFIER/, 'HashJoin: PIPELINE CLASSIFIER header');
like($out, qr/Hash\b/, 'classifies Hash node');
like($out, qr/HashJoin/, 'classifies HashJoin node');
like($out, qr/boundary=FAN_IN/, 'HashJoin is FAN_IN boundary');

# Test 7: Append (UNION ALL)
$out = run_classified(
	"SELECT * FROM pgpg_t WHERE id < 2 "
		. "UNION ALL SELECT * FROM pgpg_t WHERE id > 4;");
like($out, qr/PIPELINE CLASSIFIER/, 'Append: PIPELINE CLASSIFIER header');
like($out, qr/Append/, 'classifies Append node');
like($out, qr/FAN_IN/, 'Append is FAN_IN boundary');

# Test 8: CTE materialized
$out = run_classified(
	"WITH cte AS MATERIALIZED (SELECT * FROM pgpg_t WHERE id <= 3) "
		. "SELECT * FROM cte;");
like($out, qr/PIPELINE CLASSIFIER/, 'CTE: PIPELINE CLASSIFIER header');
like($out, qr/CteScan/, 'classifies CteScan');
like($out, qr/source=CTE_TUPLESTORE/, 'CteScan is CTE_TUPLESTORE source');

# Test 9: ProjectSet / SRF
$out = run_classified("SELECT generate_series(1,3);");
like($out, qr/PIPELINE CLASSIFIER/, 'ProjectSet: PIPELINE CLASSIFIER header');
like($out, qr/ProjectSet/, 'classifies ProjectSet');
like($out, qr/boundary=CONSERVATIVE/, 'ProjectSet is CONSERVATIVE boundary');

# Test 10: Aggregate (hash)
$out = run_classified(
	"SET enable_hashagg=on; SET enable_sort=off; "
		. "SELECT id, count(*) FROM pgpg_t GROUP BY id;");
like($out, qr/PIPELINE CLASSIFIER/, 'Agg: PIPELINE CLASSIFIER header');
like($out, qr/Agg/, 'classifies Agg node');
like($out, qr/boundary=BLOCKING/, 'HashAgg is BLOCKING boundary');

# Cleanup
$node->safe_psql('postgres', "DROP TABLE pgpg_t2; DROP TABLE pgpg_t;");

$node->stop;
done_testing();
