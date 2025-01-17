/*
This file is part of Gix-IDE, an IDE and platform for GnuCOBOL
Copyright (C) 2022 Marco Ridoni

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
USA.
*/

#include <cstring>
#include <string>
#include <vector>

#include "Logger.h"
#include "DbInterfacePGSQL.h"
#include "IConnection.h"
#include "utils.h"
#include "cobol_var_flags.h"

#define OID_BYTEA	17
#define OID_NUMERIC 1700
#define OID_VARCHAR 1043

static std::string __get_trimmed_hostref_or_literal(void* data, int l);
static std::string pgsql_fixup_parameters(const std::string& sql);
static std::string pg_get_sqlstate(PGresult* r);

template<typename T>
using deleted_unique_ptr = std::unique_ptr<T, std::function<void(T*)>>;
		
struct PGresult_deleter {
    void operator()(PGresult *ptr) {
      if (ptr)
	  	PQclear(ptr);
    }
};

struct PGConnectionParamArray_deleter
{
	int size;
    void operator()(char **ptr) {

		for (int i = 0; i < size; i++)
			if (ptr[i])
				free(ptr[i]);

    	delete[] ptr;
    }	
};

using PGresultPtr = std::unique_ptr<PGresult, PGresult_deleter>;
using PGConnectionParamArray = std::unique_ptr<char*[], PGConnectionParamArray_deleter>;

class pgsqlParamArray {

public:

	pgsqlParamArray(int sz) {
		_data = new char* [sz]();
		nitems = sz;
	}

	~pgsqlParamArray()
	{
		if (_data) {
			for (int i = 0; i < nitems; i++) {
				if (_data[i])
					delete _data[i];
			}
			delete[] _data;
		}
	}

	void assign(int i, char* d, int l)
	{
		if (d) {
			_data[i] = new char[l + 1];
			memcpy(_data[i], d, l);
			_data[i][l] = '\0';
		}
	}

	char** data() const
	{
		return _data;
	}

private:

	char** _data = nullptr;
	int nitems = 0;

};

DbInterfacePGSQL::DbInterfacePGSQL()
{}

DbInterfacePGSQL::~DbInterfacePGSQL()
{
	if (connaddr)
		PQfinish(connaddr);
}

int DbInterfacePGSQL::init(const std::shared_ptr<spdlog::logger>& _logger)
{
	connaddr = NULL;
	current_resultset_data = nullptr;
	last_rc = 0;
	last_state = "";

	auto lib_sink = _logger->sinks().at(0);
	lib_logger = std::make_shared<spdlog::logger>("libgixsql-pgsql", lib_sink);
	lib_logger->set_level(_logger->level());
	lib_logger->info("libgixsql-pgsql logger started");

	return DBERR_NO_ERROR;
}

int DbInterfacePGSQL::connect(std::shared_ptr<IDataSourceInfo> _conn_info, std::shared_ptr<IConnectionOptions> _conn_opts)
{
	PGconn* conn;

	lib_logger->trace(FMT_FILE_FUNC "PGSQL::connect - autocommit: {:d}, encoding: {}", __FILE__, __func__, (int)_conn_opts->autocommit, _conn_opts->client_encoding);

	connaddr = NULL;
	current_resultset_data = nullptr;

	last_rc = 0;
	last_error = "";
	last_state = "";

	std::map<std::string, std::string> connection_params;

	connection_params["dbname"] = _conn_info->getDbName().empty() ? "" : _conn_info->getDbName();
	connection_params["host"] = _conn_info->getHost().empty() ? "" : _conn_info->getHost();
	connection_params["port"] = _conn_info->getPort() == 0 ? "" : std::to_string(_conn_info->getPort());
	connection_params["user"] = _conn_info->getUsername().empty() ? "" : _conn_info->getUsername();
	connection_params["password"] = _conn_info->getPassword().empty() ? "" : _conn_info->getPassword();

	std::vector<std::string> supported_libpq_opts = {
		"hostaddr", "connect_timeout", "application_name", "keepalives", "keepalives_idle", "keepalives_interval", "keepalives_count", "sslmode",
		"requiressl", "sslcert", "sslkey", "sslrootcert", "sslcrl", "krbsrvname", "gsslib", "service"
	};

	auto opts = _conn_info->getOptions();
	for (std::string supported_libpq_opt : supported_libpq_opts) {
		if (opts.find(supported_libpq_opt) != opts.end())
			connection_params[supported_libpq_opt] = opts[supported_libpq_opt];
	}

	//std::unique_ptr<const char* []> libpq_opt_keys = std::make_unique<const char* []>(connection_params.size() + 1);
	//std::unique_ptr<const char* []> libpq_opt_vals = std::make_unique<const char* []>(connection_params.size() + 1);

	int szParams = (int)connection_params.size() + 1;
	PGConnectionParamArray libpq_opt_keys(new char* [connection_params.size() + 1], PGConnectionParamArray_deleter{ szParams });
	PGConnectionParamArray libpq_opt_vals(new char* [connection_params.size() + 1], PGConnectionParamArray_deleter{ szParams });

	int i = 0;
	for (auto it = connection_params.begin(); it != connection_params.end(); ++it) 
	{
		libpq_opt_keys[i] = strdup(it->first.c_str());
		libpq_opt_vals[i] = strdup(it->second.c_str());
		lib_logger->trace("libpq - connection parameter ({}): [{}] => [{}]", i, it->first, it->second);
		i++;
	}
	libpq_opt_keys[connection_params.size()] = nullptr;
	libpq_opt_vals[connection_params.size()] = nullptr;

	conn = PQconnectdbParams(libpq_opt_keys.get(), libpq_opt_vals.get(), 0);

	if (conn == NULL) {
		last_error = "Connection failed";
		last_rc = DBERR_CONNECTION_FAILED;
		return DBERR_CONNECTION_FAILED;
	}
	else if (PQstatus(conn) != CONNECTION_OK) {
		last_error = PQerrorMessage(conn);
		last_rc = PQstatus(conn);
		lib_logger->error("libpq: {}", last_error);
		PQfinish(conn);
		return DBERR_CONNECTION_FAILED;
	}

	if (!_conn_opts->client_encoding.empty()) {
		if (PQsetClientEncoding(conn, _conn_opts->client_encoding.c_str())) {
			last_rc = 1;
			last_error = PQerrorMessage(conn);
			lib_logger->error("libpq: {}", last_error);
			PQfinish(conn);
			return DBERR_CONNECTION_FAILED;
		}
	}


	if (opts.find("default_schema") != opts.end()) {
		std::string default_schema = opts["default_schema"];
		if (!default_schema.empty()) {
			std::string default_schema = opts["default_schema"];
			std::string spq = "set search_path to " + default_schema;
			auto r = PQexec(conn, spq.c_str());
			auto rc = PQresultStatus(r);
			if (rc != PGRES_COMMAND_OK) {
				last_rc = rc;
				last_error = PQresultErrorMessage(r);
				last_state = pg_get_sqlstate(r);
				lib_logger->error("libpq: {}", last_error);
				PQfinish(conn);
				return DBERR_CONNECTION_FAILED;
			}
		}
	}

	// Autocommit is set to off. Since PostgreSQL is ALWAYS in autocommit mode 
	// we will optionally start a transaction
	if (_conn_opts->autocommit == AutoCommitMode::Off) {
		lib_logger->trace(FMT_FILE_FUNC "PGSQL::connect: autocommit is off, starting initial transaction", __FILE__, __func__);

		PGresultPtr r(PQexec(conn, "BEGIN TRANSACTION"));

		auto rc = PQresultStatus(r.get());
		if (rc != PGRES_COMMAND_OK) {
			last_rc = rc;
			last_error = PQresultErrorMessage(r.get());
			last_state = pg_get_sqlstate(r.get());
			lib_logger->error("libpq: {}", last_error);
			PQfinish(conn);
			return DBERR_CONNECTION_FAILED;
		}
	}

	if (opts.find("decode_binary") != opts.end()) {
		std::string opt_decode_binary = opts["decode_binary"];
		if (!opt_decode_binary.empty()) {
			if (opt_decode_binary == "on" || opt_decode_binary == "1" || opt_decode_binary == "true") {
				this->decode_binary = DECODE_BINARY_ON;
			}

			if (opt_decode_binary == "off" || opt_decode_binary == "0" || opt_decode_binary == "false") {
				this->decode_binary = DECODE_BINARY_OFF;
			}
		}
	}

	if (opts.find("native_cursors") != opts.end()) {
		std::string opt_native_cursors = opts["native_cursors"];
		if (!opt_native_cursors.empty()) {
			if (opt_native_cursors == "on" || opt_native_cursors == "1" || opt_native_cursors == "true") {
				this->use_native_cursors = true;
			}

			if (opt_native_cursors == "off" || opt_native_cursors == "0" || opt_native_cursors == "false") {
				this->use_native_cursors = false;
			}
		}
	}

	connaddr = conn;

	this->connection_opts = _conn_opts;
	this->data_source_info = _conn_info;

	return DBERR_NO_ERROR;
}

int DbInterfacePGSQL::reset()
{
	int rc = terminate_connection();
	if (rc == DBERR_NO_ERROR)
		return DBERR_NO_ERROR;
	else
		return DBERR_CONN_RESET_FAILED;
}

int DbInterfacePGSQL::terminate_connection()
{
	if (connaddr) {
		PQfinish(connaddr);
		connaddr = NULL;
	}

	current_resultset_data.reset();

	return DBERR_NO_ERROR;
}

int DbInterfacePGSQL::prepare(const std::string& _stmt_name, const std::string& query)
{
	std::string prepared_sql;
	std::string stmt_name = to_lower(_stmt_name);

	lib_logger->trace(FMT_FILE_FUNC "PGSQL::prepare ({}) - SQL: {}", __FILE__, __func__, stmt_name, query);

	if (_prepared_stmts.find(stmt_name) != _prepared_stmts.end()) {
		return DBERR_PREPARE_FAILED;
	}

	if (connection_opts->fixup_parameters) {
		prepared_sql = pgsql_fixup_parameters(query);
		lib_logger->trace(FMT_FILE_FUNC "PGSQL::fixup parameters is on", __FILE__, __func__);
		lib_logger->trace(FMT_FILE_FUNC "PGSQL::prepare ({}) - SQL(P): {}", __FILE__, __func__, stmt_name, prepared_sql);
	}
	else {
		prepared_sql = query;
	}

	PGresult* res = PQprepare(connaddr, stmt_name.c_str(), prepared_sql.c_str(), 0, nullptr);

	last_rc = PQresultStatus(res);
	last_error = PQresultErrorMessage(res);
	last_state = pg_get_sqlstate(res);
	PQclear(res);

	lib_logger->trace(FMT_FILE_FUNC "PGSQL::prepare ({} - res: ({}) {}", __FILE__, __func__, stmt_name, last_rc, last_error);

	if (last_rc != PGRES_COMMAND_OK) {
		return DBERR_PREPARE_FAILED;
	}

	_prepared_stmts[stmt_name] = nullptr;	// for now we just track it, the actual result will be stored later

	return DBERR_NO_ERROR;
}

int DbInterfacePGSQL::exec_prepared(const std::string& _stmt_name, std::vector<CobolVarType> paramTypes, std::vector<std_binary_data>& paramValues, std::vector<unsigned long> paramLengths, const std::vector<uint32_t>& paramFlags)
{
	lib_logger->trace(FMT_FILE_FUNC "statement name: {}", __FILE__, __func__, _stmt_name);

	std::string stmt_name = to_lower(_stmt_name);
	
	if (_prepared_stmts.find(stmt_name) == _prepared_stmts.end()) {
		lib_logger->error("Invalid prepared statment name: {}", stmt_name);
		return DBERR_SQL_ERROR;
	}
	
	if (paramTypes.size() != paramValues.size() || paramTypes.size() != paramFlags.size()) {
		lib_logger->error(FMT_FILE_FUNC "Internal error: parameter count mismatch", __FILE__, __func__);
		last_error = "Internal error: parameter count mismatch";
		last_rc = DBERR_INTERNAL_ERR;
		return DBERR_INTERNAL_ERR;
	}

	if (paramTypes.size() != paramValues.size() || paramTypes.size() != paramFlags.size()) {
		lib_logger->error(FMT_FILE_FUNC "Internal error: parameter count mismatch", __FILE__, __func__);
		last_error = "Internal error: parameter count mismatch";
		last_rc = DBERR_INTERNAL_ERR;
		return DBERR_INTERNAL_ERR;
	}

	std::unique_ptr<pgsqlParamArray> param_vals = std::make_unique<pgsqlParamArray>(paramValues.size());

	std::unique_ptr<Oid[]> param_types = std::make_unique<Oid[]>(paramTypes.size());
	std::unique_ptr<int[]> param_lengths = std::make_unique<int[]>(paramLengths.size());	// will be used for binary data, currently ignored
	std::unique_ptr<int[]> param_formats = std::make_unique<int[]>(paramFlags.size());

	for (int i = 0; i < paramValues.size(); i++) {
		if (paramLengths.at(i) != DB_NULL) {
			param_vals->assign(i, (char*)paramValues[i].data(), paramLengths[i]);
			param_lengths[i] = paramLengths.at(i);
		}
		else {
			param_vals->assign(i, nullptr, 0);
			param_lengths[i] = 0;

		}
		param_types[i] = get_pgsql_type(paramTypes.at(i), paramFlags[i]);
		param_formats[i] = CBL_FIELD_IS_BINARY(paramFlags[i]) ? 1 : 0;
	}

	int ret = DBERR_NO_ERROR;

	std::shared_ptr<PGResultSetData> wk_rs = std::make_shared<PGResultSetData>();

	wk_rs->resultset = PQexecPrepared(connaddr, stmt_name.c_str(), paramValues.size(), param_vals->data(), param_lengths.get(), param_formats.get(), 0);

	last_rc = PQresultStatus(wk_rs->resultset);
	last_error = PQresultErrorMessage(wk_rs->resultset);
	last_state = pg_get_sqlstate(wk_rs->resultset);
	
	if (last_rc == PGRES_COMMAND_OK || last_rc == PGRES_TUPLES_OK) {
		_prepared_stmts[stmt_name] = wk_rs;
		return DBERR_NO_ERROR;
	}
	else {
		last_rc = -(10000 + last_rc);
		return DBERR_SQL_ERROR;
	}
}

DbPropertySetResult DbInterfacePGSQL::set_property(DbProperty p, std::variant<bool, int, std::string> v)
{
	return DbPropertySetResult::Unsupported;
}

int DbInterfacePGSQL::exec(std::string query)
{
	return _pgsql_exec(nullptr, query);
}

int DbInterfacePGSQL::_pgsql_exec(const std::shared_ptr<ICursor>& crsr, const std::string& query)
{
	lib_logger->trace(FMT_FILE_FUNC "SQL: #{}#", __FILE__, __func__, query);
	
	std::shared_ptr<PGResultSetData> wk_rs = (crsr != nullptr) ? std::static_pointer_cast<PGResultSetData>(crsr->getPrivateData()) : current_resultset_data;

	// Is this really necessary?
	if (wk_rs && wk_rs == current_resultset_data) {
		current_resultset_data.reset();
	}

	wk_rs = std::make_shared<PGResultSetData>();
	wk_rs->resultset = PQexecParams(connaddr, query.c_str(), 0, NULL, NULL, NULL, NULL, 0);

	last_rc = PQresultStatus(wk_rs->resultset);
	last_error = PQresultErrorMessage(wk_rs->resultset);
	last_state = pg_get_sqlstate(wk_rs->resultset);

	// we trap COMMIT/ROLLBACK
	if (connection_opts->autocommit == AutoCommitMode::Off && is_tx_termination_statement(query)) {
		
		// we clean up: whether the COMMIT/ROLLBACK failed or not this is probably useless anyway
		current_resultset_data.reset();

		if (last_rc == PGRES_COMMAND_OK) {	// COMMIT/ROLLBACK succeeded, we try to start a new transaction
			lib_logger->trace(FMT_FILE_FUNC "autocommit mode is disabled, trying to start a new transaction", __FILE__, __func__);
			auto new_tx_rs = PQexec(connaddr, "START TRANSACTION");
			last_rc = PQresultStatus(new_tx_rs);
			last_error = PQresultErrorMessage(new_tx_rs);
			last_state = pg_get_sqlstate(new_tx_rs);
			lib_logger->trace(FMT_FILE_FUNC "transaction start result: {} ({})", __FILE__, __func__, last_error, last_state);
			return (last_rc != PGRES_COMMAND_OK) ? DBERR_SQL_ERROR : DBERR_NO_ERROR;
		}

		// if COMMIT/ROLLBACK failed, the error code/state is already set, it will be handled below
	}

	if (last_rc == PGRES_COMMAND_OK) {
		if (is_update_or_delete_statement(query)) {
			int nrows = get_num_rows(wk_rs->resultset);
			if (nrows <= 0) {
				last_rc = 100;
				return DBERR_SQL_ERROR;
			}
		}
	}

	if (last_rc == PGRES_COMMAND_OK || last_rc == PGRES_TUPLES_OK) {
		if (crsr)
			crsr->setPrivateData(wk_rs);
		else
			current_resultset_data = wk_rs;

		return DBERR_NO_ERROR;
	}
	else {
		last_rc = -(10000 + last_rc);
		lib_logger->error("ERROR ({} - {}): {}", last_rc, last_state, last_error);
		return DBERR_SQL_ERROR;
	}

}

int DbInterfacePGSQL::exec_params(const std::string& query, const std::vector<CobolVarType>& paramTypes, const std::vector<std_binary_data>& paramValues, const std::vector<unsigned long>& paramLengths, const std::vector<uint32_t>& paramFlags)
{
	return _pgsql_exec_params(nullptr, query, paramTypes, paramValues, paramLengths, paramFlags);
}

int DbInterfacePGSQL::_pgsql_exec_params(const std::shared_ptr<ICursor>& crsr, const std::string& query, const std::vector<CobolVarType>& paramTypes, const std::vector<std_binary_data>& paramValues, const std::vector<unsigned long>& paramLengths, const std::vector<uint32_t>& paramFlags)
{

	lib_logger->trace(FMT_FILE_FUNC "SQL: #{}#", __FILE__, __func__, query);

	std::shared_ptr<PGResultSetData> wk_rs = (crsr != nullptr) ? std::static_pointer_cast<PGResultSetData>(crsr->getPrivateData()) : current_resultset_data;

	if (paramTypes.size() != paramValues.size() || paramTypes.size() != paramFlags.size()) {
		lib_logger->error(FMT_FILE_FUNC "Internal error: parameter count mismatch", __FILE__, __func__);
		last_error = "Internal error: parameter count mismatch";
		last_rc = DBERR_INTERNAL_ERR;
		return DBERR_INTERNAL_ERR;
	}

	std::unique_ptr<pgsqlParamArray> param_vals = std::make_unique<pgsqlParamArray>(paramValues.size());

	std::unique_ptr<Oid[]> param_types = std::make_unique<Oid[]>(paramTypes.size());
	std::unique_ptr<int[]> param_lengths = std::make_unique<int[]>(paramLengths.size());
	std::unique_ptr<int[]> param_formats = std::make_unique<int[]>(paramFlags.size());	

	for (int i = 0; i < paramValues.size(); i++) {
		if (paramLengths.at(i) != DB_NULL) {
			param_vals->assign(i, (char*)paramValues[i].data(), paramLengths[i]);
			param_lengths[i] = paramLengths.at(i);
		}
		else {
			param_vals->assign(i, nullptr, 0);
			param_lengths[i] = 0;
			
		}
		param_types[i] = get_pgsql_type(paramTypes.at(i), paramFlags[i]);
		param_formats[i] = CBL_FIELD_IS_BINARY(paramFlags[i]) ? 1 : 0;
	} 

	if (wk_rs && wk_rs == current_resultset_data) {
		current_resultset_data.reset();
	}

	wk_rs = std::make_shared<PGResultSetData>();
	wk_rs->resultset = PQexecParams(connaddr, query.c_str(), paramValues.size(), param_types.get(), param_vals->data(), param_lengths.get(), param_formats.get(), 0);
	wk_rs->num_rows = get_num_rows(wk_rs->resultset);

	last_rc = PQresultStatus(wk_rs->resultset);
	last_error = PQresultErrorMessage(wk_rs->resultset);
	last_state = pg_get_sqlstate(wk_rs->resultset);

	// we trap COMMIT/ROLLBACK
	if (connection_opts->autocommit == AutoCommitMode::Off && is_tx_termination_statement(query)) {

		// we clean up: if the COMMIT/ROLLBACK failed this is probably useless anyway
		current_resultset_data.reset();

		if (last_rc == PGRES_COMMAND_OK) {	// COMMIT/ROLLBACK succeeded, we try to start a new transaction
			lib_logger->trace(FMT_FILE_FUNC "autocommit mode is disabled, trying to start a new transaction", __FILE__, __func__);
			auto new_tx_rs = PQexec(connaddr, "START TRANSACTION");
			last_rc = PQresultStatus(new_tx_rs);
			last_error = PQresultErrorMessage(new_tx_rs);
			last_state = pg_get_sqlstate(new_tx_rs);
			lib_logger->trace(FMT_FILE_FUNC "transaction start result: {} ({})", __FILE__, __func__, last_error, last_state);
			return (last_rc != PGRES_COMMAND_OK) ? DBERR_SQL_ERROR : DBERR_NO_ERROR;
		}

		// if COMMIT/ROLLBACK failed, the error code/state is already set, it will be handled below
	}

	if (last_rc == PGRES_COMMAND_OK) {
		if (is_update_or_delete_statement(query)) {
			if (wk_rs->num_rows <= 0) {
				last_rc = 100;
				return DBERR_SQL_ERROR;
			}
		}
	}

	if (last_rc == PGRES_COMMAND_OK || last_rc == PGRES_TUPLES_OK) {
		if (crsr)
			crsr->setPrivateData(wk_rs);
		else
			current_resultset_data = wk_rs;

		return DBERR_NO_ERROR;
	}
	else {
		lib_logger->error("ERROR ({} - {}): {}", last_rc, last_state, last_error);
		last_rc = -(10000 + last_rc);
		return DBERR_SQL_ERROR;
	}
}


int DbInterfacePGSQL::cursor_close(const std::shared_ptr<ICursor>& cursor)
{
	int rc = DBERR_NO_ERROR;

	if (!cursor)
		return DBERR_CLOSE_CURSOR_FAILED;

	if (use_native_cursors) {
		std::string query = "CLOSE " + cursor->getName();
		int rc = exec(query);
	}

	if (cursor->getPrivateData()) {
		cursor->clearPrivateData();
	}

	return (rc == DBERR_NO_ERROR) ? DBERR_NO_ERROR : DBERR_CLOSE_CURSOR_FAILED;
}

int DbInterfacePGSQL::cursor_declare(const std::shared_ptr<ICursor>& cursor)
{
	if (!cursor)
		return DBERR_DECLARE_CURSOR_FAILED;

	std::map<std::string, std::shared_ptr<ICursor>>::iterator it = _declared_cursors.find(cursor->getName());
	if (it == _declared_cursors.end()) {
		_declared_cursors[cursor->getName()] = cursor;
	}

	return DBERR_NO_ERROR;
}

int DbInterfacePGSQL::cursor_open(const std::shared_ptr<ICursor>& crsr)
{
	if (!crsr)
		return DBERR_OPEN_CURSOR_FAILED;

	std::string sname = crsr->getName();
	std::string full_query;

	std::string squery = crsr->getQuery();
	void* src_addr = nullptr;
	int src_len = 0;

	if (squery.size() == 0) {
		crsr->getQuerySource(&src_addr, &src_len);
		squery = __get_trimmed_hostref_or_literal(src_addr, src_len);
	}

	if (starts_with(squery, "@")) {
		if (!retrieve_prepared_statement_source(squery.substr(1), squery)) {
			// last_error, etc. set by retrieve_prepared_statement_source
			return DBERR_OPEN_CURSOR_FAILED;
		}
	}

	if (squery.empty()) {
		last_rc = -1;
		last_error = "Empty query";
		return DBERR_OPEN_CURSOR_FAILED;
	}

	if (use_native_cursors) {
		if (crsr->isWithHold()) {
			full_query = "DECLARE " + sname + " CURSOR WITH HOLD FOR " + squery;
		}
		else {
			full_query = "DECLARE " + sname + " CURSOR FOR " + squery;
		}
	}
	else {
		full_query = squery;
	}

	// execute query

	auto param_vals = crsr->getParameterValues();
	auto param_types = crsr->getParameterTypes();
	auto param_lengths = crsr->getParameterLengths();	// will be used for binary data, currently ignored
	auto param_formats = crsr->getParameterFlags();
	int rc = _pgsql_exec_params(crsr, full_query, param_types, param_vals, param_lengths, param_formats);

	return (rc == DBERR_NO_ERROR) ? DBERR_NO_ERROR : DBERR_OPEN_CURSOR_FAILED;
}

int DbInterfacePGSQL::cursor_fetch_one(const std::shared_ptr<ICursor>& cursor, int fetchmode)
{
	if (!cursor)
		return DBERR_FETCH_ROW_FAILED;

	lib_logger->trace(FMT_FILE_FUNC "owner id: {}, cursor name: {}, mode: {}", __FILE__, __func__, cursor->getConnectionName(), cursor->getName(), FETCH_NEXT_ROW);

	std::string sname = cursor->getName();

	if (use_native_cursors) {
		std::string query;

		// execute query
		if (fetchmode == FETCH_CUR_ROW) {
			query = "FETCH RELATIVE 0 FROM " + sname;
		}
		else if (fetchmode == FETCH_PREV_ROW) {
			query = "FETCH RELATIVE -1 FROM " + sname;
		}
		else { // NEXT
			query = "FETCH RELATIVE 1 FROM " + sname;
		}

		last_rc = _pgsql_exec(cursor, query);
		if (last_rc != DBERR_NO_ERROR)
			return DBERR_SQL_ERROR;

		int ntuples = get_num_rows(cursor);
		if (ntuples < 1) {
			lib_logger->trace(FMT_FILE_FUNC "TUPLES NODATA", __FILE__, __func__);
			return DBERR_NO_DATA;
		}
		else if (ntuples > 1) {
			return DBERR_TOO_MUCH_DATA;
		}
	}
	else {
		std::shared_ptr<PGResultSetData> wk_rs = std::dynamic_pointer_cast<PGResultSetData>(cursor->getPrivateData());
		wk_rs->current_row_index++;
		if (wk_rs->current_row_index >= wk_rs->num_rows)
			return DBERR_NO_DATA;
	}

	return DBERR_NO_ERROR;
}

bool DbInterfacePGSQL::get_resultset_value(ResultSetContextType resultset_context_type, const IResultSetContextData& context, int row, int col, char* bfr, uint64_t bfrlen, uint64_t* value_len, bool
                                           * is_db_null)
{
	size_t to_length = 0;
	*value_len = 0;

	std::shared_ptr<PGResultSetData> wk_rs;

	switch (resultset_context_type) {

		case ResultSetContextType::CurrentResultSet:
			wk_rs = current_resultset_data;
			break;

		case ResultSetContextType::PreparedStatement:
		{
			PreparedStatementContextData& p = (PreparedStatementContextData&)context;

			std::string stmt_name = p.prepared_statement_name;
			stmt_name = to_lower(stmt_name);
			if (_prepared_stmts.find(stmt_name) == _prepared_stmts.end()) {
				lib_logger->error("Invalid prepared statement name: {}", stmt_name);
				return false;
			}

			wk_rs = _prepared_stmts[stmt_name];
		}
		break;

		case ResultSetContextType::Cursor:
		{
			CursorContextData& p = (CursorContextData&)context;
			std::shared_ptr <ICursor> c = p.cursor;
			if (!c) {
				lib_logger->error("Invalid cursor reference");
				return false;
			}
			wk_rs = std::dynamic_pointer_cast<PGResultSetData>(c->getPrivateData());
			// we overwrite the row index (for ?)
			if (wk_rs->current_row_index != -1) {
				row = wk_rs->current_row_index;
			}
		}
		break;

	}

	if (!wk_rs) {
		lib_logger->error("Invalid resultset");
		return false;
	}

	const char* res = PQgetvalue(wk_rs->resultset, row, col);
	if (!res) {
		lib_logger->error("Cannot retrieve return statement value for row {} col {}", row, col);
		return false;	// FIXME: this means "caller error", not a problem with the resultset value!
	}

	if (!strlen(res)) {
		if (PQgetisnull(wk_rs->resultset, row, col)) {
			*is_db_null = true;
			*value_len = 0;
			bfr[0] = 0;
			return true;
		}
	}

	auto type = PQftype(wk_rs->resultset, col);
	if (type != OID_BYTEA || !this->decode_binary) {
		to_length = strlen(res);
		if (to_length > bfrlen) {
			return false;
		}

		*value_len = (int)to_length;
		memcpy(bfr, res, to_length + 1);	// copy with trailing null

	}
	else {
		unsigned char* tmp_bfr = PQunescapeBytea((const unsigned char*)res, &to_length);
		if (!tmp_bfr)
			return false;

		if (to_length > bfrlen) {
			PQfreemem(tmp_bfr);
			return false;
		}

		*value_len = (int)to_length;
		memcpy(bfr, tmp_bfr, to_length);
		if (to_length < bfrlen) {
			memset(bfr + to_length, 0, bfrlen - to_length);
		}
		PQfreemem(tmp_bfr);
	}
	return true;
}

bool DbInterfacePGSQL::move_to_first_record(const std::string& _stmt_name)
{
	std::shared_ptr<PGResultSetData> wk_rs;
	std::string stmt_name = to_lower(_stmt_name);

	if (stmt_name.empty()) {
		wk_rs = current_resultset_data;
	}
	else {
		if (_prepared_stmts.find(stmt_name) == _prepared_stmts.end()) {
			lib_logger->error("Invalid prepared statement name: {}", stmt_name);
			pgsqlSetError(DBERR_MOVE_TO_FIRST_FAILED, "HY000", "Invalid statement reference");
			return false;
		}

		wk_rs = _prepared_stmts[stmt_name];
	}

	if (!wk_rs || !wk_rs->resultset) {
		pgsqlSetError(DBERR_MOVE_TO_FIRST_FAILED, "HY000", "Invalid statement reference");
		return false;
	}

	int nrows = get_num_rows(wk_rs->resultset);
	if (nrows <= 0) {
		pgsqlSetError(DBERR_NO_DATA, "02000", "No data");
		return false;
	}
	return true;
}

uint64_t DbInterfacePGSQL::get_native_features()
{
	return (uint64_t)DbNativeFeature::ResultSetRowCount;
}

int DbInterfacePGSQL::get_num_rows(const std::shared_ptr<ICursor>& crsr)
{
	std::shared_ptr<PGResultSetData> wk_rs = (crsr != nullptr) ? std::static_pointer_cast<PGResultSetData>(crsr->getPrivateData()) : current_resultset_data;

	if (!wk_rs)
		return -1;

	char* c = PQcmdTuples(wk_rs->resultset);
	if (!c)
		return -1;

	int n = atoi(c);
	return n;
}

int DbInterfacePGSQL::get_num_fields(const std::shared_ptr<ICursor>& crsr)
{
	std::shared_ptr<PGResultSetData> wk_rs = (crsr != nullptr) ? std::static_pointer_cast<PGResultSetData>(crsr->getPrivateData()) : current_resultset_data;
	if (!wk_rs)
		return -1;

	return PQnfields(wk_rs->resultset);
}

int DbInterfacePGSQL::get_num_rows(PGresult* r)
{
	if (!r)
		return -1;

	char* c = PQcmdTuples(r);
	if (!c)
		return -1;

	int n = atoi(c);
	return n;
}

void DbInterfacePGSQL::pgsqlClearError()
{
	last_error = "";
	last_rc = DBERR_NO_ERROR;
	last_state = "00000";
}

void DbInterfacePGSQL::pgsqlSetError(int err_code, std::string sqlstate, std::string err_msg)
{
	last_error = err_msg;
	last_rc = err_code;
	last_state = sqlstate;
}

Oid DbInterfacePGSQL::get_pgsql_type(CobolVarType t, uint32_t flags)
{
	switch (t) {
	case CobolVarType::COBOL_TYPE_UNSIGNED_NUMBER:
	case CobolVarType::COBOL_TYPE_SIGNED_NUMBER_TC:
	case CobolVarType::COBOL_TYPE_SIGNED_NUMBER_TS:
	case CobolVarType::COBOL_TYPE_SIGNED_NUMBER_LC:
	case CobolVarType::COBOL_TYPE_SIGNED_NUMBER_LS:
	case CobolVarType::COBOL_TYPE_UNSIGNED_NUMBER_PD:
	case CobolVarType::COBOL_TYPE_SIGNED_NUMBER_PD:
	case CobolVarType::COBOL_TYPE_UNSIGNED_BINARY:
	case CobolVarType::COBOL_TYPE_SIGNED_BINARY:
		return OID_NUMERIC;

	case CobolVarType::COBOL_TYPE_ALPHANUMERIC:
	case CobolVarType::COBOL_TYPE_JAPANESE:
		return CBL_FIELD_IS_BINARY(flags) ? OID_BYTEA : OID_VARCHAR;

	default:
		return 0;
	}
}

PGResultSetData::PGResultSetData()
{
	resultset = nullptr;
	current_row_index = -1;
}

PGResultSetData::~PGResultSetData()
{
	if (resultset)
		PQclear(resultset);
}

static std::string __get_trimmed_hostref_or_literal(void* data, int l)
{
	if (!data)
		return std::string();

	if (!l)
		return std::string((char*)data);

	if (l > 0) {
		std::string s = std::string((char*)data, l);
		return trim_copy(s);
	}

	// variable-length fields (negative length)
	void* actual_data = (char*)data + VARLEN_LENGTH_SZ;
	VARLEN_LENGTH_T* len_addr = (VARLEN_LENGTH_T*)data;
	int actual_len = (*len_addr);

	// Should we check the actual length against the defined length?
	//...

	std::string t = std::string((char*)actual_data, (-l) - VARLEN_LENGTH_SZ);
	return trim_copy(t);
}

static std::string pg_get_sqlstate(PGresult* r)
{
	char* c = PQresultErrorField(r, PG_DIAG_SQLSTATE);
	if (c)
		return std::string(c);
	else
		return "00000";
}


static std::string pgsql_fixup_parameters(const std::string& sql)
{
	int n = 1;
	bool in_single_quoted_string = false;
	bool in_double_quoted_string = false;
	bool in_param_id = false;
	std::string out_sql;

	for (auto itc = sql.begin(); itc != sql.end(); ++itc) {
		char c = *itc;

		if (in_param_id && isalnum(c))
			continue;
		else {
			in_param_id = false;
		}

		switch (c) {
		case '"':
			out_sql += c;
			in_double_quoted_string = !in_double_quoted_string;
			continue;

		case '\'':
			out_sql += c;
			in_single_quoted_string = !in_single_quoted_string;
			continue;

		case '?':
		case ':':
			if (!in_single_quoted_string && !in_double_quoted_string) {
				out_sql += ("$" + std::to_string(n++));
				in_param_id = true;
			}
			else
				out_sql += c;
			continue;

		default:
			out_sql += c;

		}
	}

	return out_sql;
}


const char* DbInterfacePGSQL::get_error_message()
{
	if (current_resultset_data != NULL)
		return PQresultErrorMessage(current_resultset_data->resultset);
	else
		if (connaddr != NULL)
			return PQerrorMessage(connaddr);
		else
			return NULL;
}

int DbInterfacePGSQL::get_error_code()
{
	return last_rc;
}

std::string DbInterfacePGSQL::get_state()
{
	return last_state;
}

std::string vector_join(const std::vector<std::string>& v, char sep)
{
	std::string s;

	for (std::vector< std::string>::const_iterator p = v.begin();
		p != v.end(); ++p) {
		s += *p;
		if (p != v.end() - 1)
			s += sep;
	}
	return s;
}


bool DbInterfacePGSQL::retrieve_prepared_statement_source(const std::string& prep_stmt_name, std::string& src)
{
	lib_logger->trace(FMT_FILE_FUNC "Retrieving SQL source for prepared statement {}", __FILE__, __func__, prep_stmt_name);

	auto pvals = std::make_unique<char*[]>(1);
	pvals[0] = (char*)prep_stmt_name.c_str();

	std::string qry = "select statement from pg_prepared_statements where lower(name) = lower($1)";
	
	PGresultPtr tr(PQexecParams(connaddr, qry.c_str(), 1, NULL, pvals.get(), NULL, NULL, 0));

	last_rc = PQresultStatus(tr.get());
	last_error = PQresultErrorMessage(tr.get());
	last_state = pg_get_sqlstate(tr.get());

	if (last_rc == PGRES_TUPLES_OK) {
		if (PQntuples(tr.get()) != 1) {
			last_rc = 42704;
			last_error = "\"" + prep_stmt_name + "\" not found";
			last_state = "42704";
			lib_logger->error("Cannot retrieve prepared statement source: {}", last_error);
			return false;
		}

		const char* res = PQgetvalue(tr.get(), 0, 0);
		if (!res) {
			last_rc = PQresultStatus(tr.get());
			last_error = PQresultErrorMessage(tr.get());
			last_state = pg_get_sqlstate(tr.get());
			lib_logger->error("Cannot retrieve prepared statement source: {}", last_error);
			return false;
		}
		src = res;
		return true;
	}

	return false;
}