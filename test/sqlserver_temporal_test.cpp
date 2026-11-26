#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "odbc_filter_pushdown.hpp"
#include "odbc_parameter.hpp"
#include "odbc_types.hpp"
#include "odbc_utils.hpp"
#include "sqlserver_temporal_utils.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace duckdb;

static void Require(bool value, const char *msg) {
	if (!value) {
		throw std::runtime_error(msg);
	}
}

template <typename Fn>
static void RequireConversionError(Fn &&fn, const char *msg) {
	bool threw = false;
	try {
		fn();
	} catch (const ConversionException &) {
		threw = true;
	}
	Require(threw, msg);
}

int main() {
	// 1. Range & tick helpers
	Require(IsSQLServerDateYearInRange(1) && IsSQLServerDateYearInRange(9999), "valid SQL Server year range");
	Require(!IsSQLServerDateYearInRange(0) && !IsSQLServerDateYearInRange(-1) && !IsSQLServerDateYearInRange(10000),
	        "invalid SQL Server year range");
	Require(IsSQLServerDateTimeInRange(1753) && !IsSQLServerDateTimeInRange(1752), "SQL Server datetime year min 1753");
	Require(IsSQLServerDateTimeExactTick(0) && IsSQLServerDateTimeExactTick(123000) &&
	            IsSQLServerDateTimeExactTick(997000),
	        "valid SQL Server datetime 1/300s ticks");
	Require(!IsSQLServerDateTimeExactTick(123456) && !IsSQLServerDateTimeExactTick(124000) &&
	            !IsSQLServerDateTimeExactTick(999000),
	        "non-tick datetime micros must fall back");
	Require(IsSQLServerTimeInRange(0, 0, 0, 0) && IsSQLServerTimeInRange(23, 59, 59, 999999) &&
	            !IsSQLServerTimeInRange(24, 0, 0, 0),
	        "SQL Server time range excludes 24:00:00");

	// 2. TryTransformConstant
	string out;
	Value d_ad = Value::DATE(Date::FromDate(2026, 1, 2));
	Require(ODBCUtils::TryTransformConstant(d_ad, "date", out) && out == "CAST('2026-01-02' AS DATE)",
	        "DATE pushdown SQL");
	Value d_bc = Value::DATE(Date::FromDate(0, 1, 1));
	Require(!ODBCUtils::TryTransformConstant(d_bc, "date", out), "BC DATE must fall back");
	Require(!ODBCUtils::TryTransformConstant(Value::DATE(date_t::infinity()), "date", out),
	        "infinity DATE must fall back");

	Value t_micro = Value::TIME(Time::FromTime(3, 4, 5, 123456));
	Require(ODBCUtils::TryTransformConstant(t_micro, "time", out) && out == "CAST('03:04:05.123456' AS TIME(6))",
	        "TIME pushdown SQL");
	Value t_24 = Value::TIME(Time::FromTime(24, 0, 0, 0));
	Require(!ODBCUtils::TryTransformConstant(t_24, "time", out), "24:00:00 TIME must fall back");

	Value ts_dt_ok = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 2), Time::FromTime(3, 4, 5, 123000)));
	Require(ODBCUtils::TryTransformConstant(ts_dt_ok, "datetime", out) &&
	            out == "CONVERT(datetime, '2026-01-02T03:04:05.123', 126)",
	        "datetime exact tick pushdown");
	Value ts_dt_bad_ms =
	    Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 2), Time::FromTime(3, 4, 5, 124000)));
	Require(!ODBCUtils::TryTransformConstant(ts_dt_bad_ms, "datetime", out),
	        "datetime .124 must fall back to avoid tick rounding");
	Value ts_dt_us =
	    Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 2), Time::FromTime(3, 4, 5, 123456)));
	Require(!ODBCUtils::TryTransformConstant(ts_dt_us, "datetime", out), "datetime .123456 must fall back");
	Value ts_dt_1700 = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(1700, 1, 1), Time::FromTime(0, 0, 0, 0)));
	Require(!ODBCUtils::TryTransformConstant(ts_dt_1700, "datetime", out), "datetime year < 1753 must fall back");

	Require(ODBCUtils::TryTransformConstant(ts_dt_us, "datetime2", out) &&
	            out == "CAST('2026-01-02T03:04:05.123456' AS DATETIME2(6))",
	        "datetime2 pushdown");
	Require(ODBCUtils::TryTransformConstant(ts_dt_bad_ms, "smalldatetime", out) &&
	            out == "CAST('2026-01-02T03:04:05.124000' AS DATETIME2(6))",
	        "smalldatetime widens via DATETIME2(6)");

	Value tstz = Value::TIMESTAMPTZ(
	    timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(2026, 1, 1), Time::FromTime(19, 4, 5, 123456))));
	Require(ODBCUtils::TryTransformConstant(tstz, "datetimeoffset", out) &&
	            out == "CAST('2026-01-01T19:04:05.123456+00:00' AS DATETIMEOFFSET(6))",
	        "datetimeoffset pushdown preserves UTC instant +00:00");

	// 3. ParseSQLServerTimestampTZText
	timestamp_tz_t parsed_tz;
	Require(ParseSQLServerTimestampTZText("2026-01-02 03:04:05.123456 +08:00", parsed_tz) &&
	            parsed_tz == tstz.GetValueUnsafe<timestamp_tz_t>(),
	        "ParseSQLServerTimestampTZText +08:00");
	Require(ParseSQLServerTimestampTZText("2026-01-02 03:04:05.123456 +8:00", parsed_tz) &&
	            parsed_tz == tstz.GetValueUnsafe<timestamp_tz_t>(),
	        "ParseSQLServerTimestampTZText single-digit offset");
	Require(!ParseSQLServerTimestampTZText("2026-01-01 19:04:05.123456", parsed_tz),
	        "offsetless string must be rejected as TIMESTAMPTZ");

	// 4. ODBCFilterPushdown::TransformFiltersWithFallback (DuckDB 2.0 ExpressionFilter)
	{
		TableFilterSet filter_set;
		filter_set.PushFilter(ProjectionIndex(0),
		                      ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, ts_dt_ok));
		filter_set.PushFilter(ProjectionIndex(1),
		                      ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, ts_dt_us));
		auto or_left = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, d_ad);
		auto or_right = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, d_bc);
		auto or_expr = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(or_left->expr),
		                                                     std::move(or_right->expr));
		filter_set.PushFilter(ProjectionIndex(2), make_uniq<ExpressionFilter>(std::move(or_expr)));
		auto opt_child = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHAN, d_bc);
		auto opt_expr = CreateOptionalFilterExpression(std::move(opt_child->expr), LogicalType::DATE);
		filter_set.PushFilter(ProjectionIndex(3), make_uniq<ExpressionFilter>(std::move(opt_expr)));

		vector<column_t> col_ids = {0, 1, 2, 2};
		vector<string> names = {"dt_ok", "dt_bad", "d"};
		vector<string> remote_types = {"datetime", "datetime", "date"};

		auto res = ODBCFilterPushdown::TransformFiltersWithFallback(col_ids, &filter_set, names, remote_types);
		Require(res.where_clause == "[dt_ok] = CONVERT(datetime, '2026-01-02T03:04:05.123', 126)",
		        "only pushable filter column emitted to WHERE");
		Require(res.unpushed_filter_columns.size() == 2 && res.unpushed_filter_columns[0] == 1 &&
		            res.unpushed_filter_columns[1] == 2,
		        "unpushed columns recorded cleanly (OR falls back whole column; optional safely omitted)");
	}

	// 5. ODBCParameter bindings & guards
	{
		ODBCParameter p_date("q", d_ad);
		Require(p_date.buffer_type == SQL_TYPE_DATE && p_date.column_size == 10 && p_date.decimal_digits == 0,
		        "DATE parameter metadata");
		SQL_DATE_STRUCT ds {};
		std::memcpy(&ds, p_date.bind_buffer.data(), sizeof(SQL_DATE_STRUCT));
		Require(ds.year == 2026 && ds.month == 1 && ds.day == 2, "DATE param struct");

		ODBCParameter p_time("q", t_micro);
		Require(p_time.buffer_type == SQL_VARCHAR && p_time.use_text_buffer && p_time.text_buffer == "03:04:05.123456",
		        "TIME parameter preserves micros via ISO text");

		ODBCParameter p_ts("q", ts_dt_us);
		Require(p_ts.buffer_type == SQL_TYPE_TIMESTAMP && p_ts.column_size == 26 && p_ts.decimal_digits == 6,
		        "TIMESTAMP parameter metadata DecimalDigits=6");

		ODBCParameter p_tz("q", tstz);
		Require(p_tz.buffer_type == SQL_VARCHAR && p_tz.use_text_buffer &&
		            p_tz.text_buffer == "2026-01-01T19:04:05.123456+00:00",
		        "TIMESTAMP_TZ parameter preserves +00:00");
	}
	RequireConversionError([&] { ODBCParameter p("q", d_bc); }, "BC DATE param must throw");
	RequireConversionError([&] { ODBCParameter p("q", t_24); }, "24:00:00 TIME param must throw");
	RequireConversionError([&] { ODBCParameter p("q", Value::TIMESTAMP(timestamp_t::infinity())); },
	                       "infinity TIMESTAMP param must throw");
	RequireConversionError([&] { ODBCParameter p("q", Value::TIMESTAMPTZ(timestamp_tz_t(timestamp_t::infinity()))); },
	                       "infinity TIMESTAMPTZ param must throw");

	std::cout << "PASS: sqlserver temporal range, pushdown fallback, offset parsing, and parameter contracts\n";
	return 0;
}
