/* contrib/pg_pipeline_graph/pg_pipeline_graph--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_pipeline_graph" to load this file. \quit

/*
 * pg_pipeline_graph v1 - diagnostic PlanState classifier prototype
 *
 * CREATE EXTENSION only records extension metadata in the catalog.
 * It does NOT activate the hook/GUC in the current backend because no
 * SQL-callable C function is exposed by this extension.
 *
 * To activate the classifier:
 *   LOAD 'pg_pipeline_graph';    -- loads library, registers GUC + hook
 *   SET pg_pipeline_graph.enabled = on;
 *   SET client_min_messages = debug1;
 *   -- run queries; diagnostic output appears as DEBUG1 messages
 *
 * This is a developer prototype.  No stable SQL API is provided.
 */
