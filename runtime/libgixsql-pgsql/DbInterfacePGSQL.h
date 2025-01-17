/*
This file is part of Gix-IDE, an IDE and platform for GnuCOBOL
Copyright (C) 2021 Marco Ridoni

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

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <tuple>
#include <libpq-fe.h>

#include "varlen_defs.h"
#include "ICursor.h"
#include "IDbInterface.h"
#include "IDbManagerInterface.h"
#include "IDataSourceInfo.h"
#include "ISchemaManager.h"
#include "cobol_var_types.h"

#define DECODE_BINARY_ON		1
#define DECODE_BINARY_OFF		0
#define DECODE_BINARY_DEFAULT	DECODE_BINARY_ON

struct PGResultSetData : public IPrivateStatementData {
	PGResultSetData();
	~PGResultSetData();

	PGresult *resultset = nullptr;
	int current_row_index = -1;
	int num_rows = 0;
};

// struct PGResultSetData_Deleter;

class DbInterfacePGSQL : public IDbInterface, public IDbManagerInterface
{
public:
	DbInterfacePGSQL();
	~DbInterfacePGSQL();

	virtual int init(const std::shared_ptr<spdlog::logger>& _logger) override;
	virtual int connect(std::shared_ptr<IDataSourceInfo>, std::shared_ptr<IConnectionOptions>) override;
	virtual int reset() override;
	virtual int terminate_connection() override;
	virtual int exec(std::string) override;
	virtual int exec_params(const std::string& query, const std::vector<CobolVarType>& paramTypes, const std::vector<std_binary_data>& paramValues, const std::vector<unsigned long>& paramLengths, const std::vector<uint32_t>& paramFlags) override;
	virtual int cursor_declare(const std::shared_ptr<ICursor>& crsr) override;
	virtual int cursor_open(const std::shared_ptr<ICursor>& crsr) override;
	virtual int cursor_close(const std::shared_ptr<ICursor>& crsr) override;
	virtual int cursor_fetch_one(const std::shared_ptr<ICursor>& crsr, int) override;
	virtual bool get_resultset_value(ResultSetContextType resultset_context_type, const IResultSetContextData& context, int row, int col, char* bfr, uint64_t bfrlen, uint64_t* value_len, bool *is_db_null) override;
	virtual bool move_to_first_record(const std::string& stmt_name = "") override;
	virtual uint64_t get_native_features() override;
	virtual int get_num_rows(const std::shared_ptr<ICursor>& crsr) override;
	virtual int get_num_fields(const std::shared_ptr<ICursor>& crsr) override;
	virtual const char* get_error_message() override;
	virtual int get_error_code() override;
	virtual std::string get_state() override;
	virtual int prepare(const std::string& stmt_name, const std::string& query) override;
	virtual int exec_prepared(const std::string& stmt_name, std::vector<CobolVarType> paramTypes, std::vector<std_binary_data>& paramValues, std::vector<unsigned long> paramLengths, const std::vector<uint32_t>& paramFlags) override;
	virtual DbPropertySetResult set_property(DbProperty p, std::variant<bool, int, std::string> v) override;

	virtual bool getSchemas(std::vector<SchemaInfo*>& res) override;
	virtual bool getTables(std::string table, std::vector<TableInfo*>& res) override;
	virtual bool getColumns(std::string schema, std::string table, std::vector<ColumnInfo*>& columns) override;
	virtual bool getIndexes(std::string schema, std::string tabl, std::vector<IndexInfo*>& idxs) override;

private:
	PGconn *connaddr = nullptr;

	std::shared_ptr<IDataSourceInfo> data_source_info;
	std::shared_ptr<IConnectionOptions> connection_opts;

	std::shared_ptr<PGResultSetData> current_resultset_data;

	int last_rc = 0;
	std::string last_error;
	std::string last_state;

	std::map<std::string, std::shared_ptr<ICursor>> _declared_cursors;
	std::map<std::string, std::shared_ptr<PGResultSetData>> _prepared_stmts;

	int decode_binary = DECODE_BINARY_DEFAULT;

	int _pgsql_exec(const std::shared_ptr<ICursor>& crsr, const std::string& query);
	int _pgsql_exec_params(const std::shared_ptr<ICursor>& crsr, const std::string& query, const std::vector<CobolVarType>& paramTypes, const std::vector<std_binary_data>& paramValues, const std::vector<unsigned long>& paramLengths, const std::vector<uint32_t>& paramFlags);

	bool retrieve_prepared_statement_source(const std::string& prep_stmt_name, std::string& src);

	int get_num_rows(PGresult* r);

	void pgsqlClearError();
	void pgsqlSetError(int err_code, std::string sqlstate, std::string err_msg);

	Oid get_pgsql_type(CobolVarType t, uint32_t flags);

	bool use_native_cursors = true;
};

