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

#include <map>
#include <vector>
#include <string>

#include "popl.hpp"

#include "libgixpp.h"
#include "GixPreProcessor.h"
#include "TPSourceConsolidation.h"
#include "TPESQLParser.h"
#include "TPESQLProcessor.h"
#include "libcpputils.h"

#include "config.h"


#ifdef _WIN32
#define PATH_LIST_SEP ";"
#else
#define PATH_LIST_SEP ":"
#endif

#define Q(x) #x
#define QUOTE(x) Q(x)

#define GIXPP_VER VERSION

using namespace popl;

bool is_alias(const std::string& f, std::string& ext);
std::string get_basename(const std::string& f);

int main(int argc, char** argv)
{
	int rc = -1;

	GixPreProcessor gp;

	std::shared_ptr<TPESQLParser> esql_parser;
	std::shared_ptr<TPESQLProcessor> esql_generator;

	// Do processing here
	const auto args = argv;

	char vbfr[1024];
	sprintf(vbfr, "gixpp - the ESQL preprocessor for Gix-IDE/GixSQL\nVersion: %s\nlibgixpp version: %s\n\nOptions", GIXPP_VER, LIBGIXPP_VER);

	OptionParser options(vbfr);

	auto opt_help = options.add<Switch>("h", "help", "displays help on commandline options");
	auto opt_version = options.add<Switch>("V", "version", "displays version information");
	auto opt_copypath = options.add<Value<std::string>>("I", "copypath", "COPY file path list");

	auto opt_infile = options.add<Value<std::string>>("i", "infile", "input file");
	auto opt_outfile = options.add<Value<std::string>>("o", "outfile", "output file");
	auto opt_symfile = options.add<Value<std::string>>("s", "symfile", "output symbol file");
	auto opt_esql = options.add<Switch>("e", "esql", "preprocess for ESQL");
	auto opt_esql_preprocess_copy = options.add<Switch>("p", "esql-preprocess-copy", "ESQL: preprocess all included COPY files");
	auto opt_esql_copy_exts = options.add<Value<std::string>>("E", "esql-copy-exts", "ESQL: copy files extension list (comma-separated)");
	auto opt_esql_param_style = options.add<Value<std::string>>("z", "param-style", "ESQL: generated parameters style (=a|d|c", "d");
	auto opt_esql_static_calls = options.add<Switch>("S", "esql-static-calls", "ESQL: emit static calls");
	auto opt_debug_info = options.add<Switch>("g", "debug-info", "generate debug info");
	auto opt_consolidate = options.add<Switch>("c", "consolidate", "consolidate source to single-file");
	auto opt_keep = options.add<Switch>("k", "keep", "keep temporary files");
	auto opt_verbose = options.add<Switch>("v", "verbose", "verbose");
	auto opt_verbose_debug = options.add<Switch>("d", "verbose-debug", "verbose (debug)");
	auto opt_parser_scanner_debug = options.add<Switch>("D", "parser-scanner-debug", "parser/scanner debug output");
	auto opt_emit_map_file = options.add<Switch>("m", "map", "emit map file");
	auto opt_emit_cobol85 = options.add<Switch>("C", "cobol85", "emit COBOL85-compliant code");
	auto opt_varying_ids = options.add<Value<std::string>>("Y", "varying", "length/data suffixes for varlen fields (=LEN,ARR)");
	auto opt_picx_as_varchar = options.add<Value<std::string>>("P", "picx-as", "text field options (=char|charf|varchar)", "char");
	auto opt_no_rec_code = options.add<Value<std::string>>("", "no-rec-code", "custom code for \"no record\" condition(=nnn)");

	options.parse(argc, argv);

	if (options.unknown_options().size() > 0) {
		std::cout << options << std::endl;
		for (auto uo : options.unknown_options()) {
			fprintf(stderr, "ERROR: unknown option: %s\n", uo.c_str());
		}

		return 1;
	}

	if (opt_help->is_set()) {
		rc = 0;
		std::cout << options << std::endl;
	}
	else {
		if (opt_version->is_set()) {
			sprintf(vbfr, "gixpp - the ESQL preprocessor for Gix-IDE/GixSQL\nVersion: %s\nlibgixpp version: %s\n", GIXPP_VER, LIBGIXPP_VER);
			std::cout << vbfr << std::endl;
			rc = 0;
		}
		else {

			if (!opt_consolidate->is_set() && !opt_esql->is_set()) {
				std::cout << options << std::endl;
				fprintf(stderr, "ERROR: please enter at least one of the -e or -c options\n");
				return 1;
			}

			if (!opt_infile->is_set() || !opt_outfile->is_set()) {
				std::cout << options << std::endl;
				fprintf(stderr, "ERROR: please enter at least the input and output file parameters\n");
				return 1;
			}


			if (opt_picx_as_varchar->is_set() && opt_picx_as_varchar->value() != "char" && opt_picx_as_varchar->value() != "charf" && opt_picx_as_varchar->value() != "varchar") {
				std::cout << options << std::endl;
				fprintf(stderr, "ERROR: picx argument must be \"charf\" or \"varchar\"\n");
				fprintf(stderr, "ERROR: -P/--picx-as argument must be one of \"char\", \"charf\", \"varchar\"\n");
				return 1;
			}

			if (opt_varying_ids->is_set()) {
				std::string varying_ids = opt_varying_ids->value();
				int cpos = varying_ids.find(",");
				if (cpos == std::string::npos || varying_ids.size() < 3 || cpos == 0 || cpos == (varying_ids.size() - 1)) {
					std::cout << options << std::endl;
					fprintf(stderr, "ERROR: please enter suffixes as --varying=LEN,ARR\n");
					return 1;
				}
			}

			CopyResolver copy_resolver(filename_get_dir(filename_absolute_path(opt_infile->value())));

			copy_resolver.setVerbose(opt_verbose->is_set());

			if (opt_copypath->is_set()) {
				for (int i = 0; i < opt_copypath->count(); i++) {
					std::vector<std::string> copy_dirs = string_split(opt_copypath->value(i), PATH_LIST_SEP);
					if (copy_dirs.size() && !copy_dirs.at(0).empty()) {
						copy_resolver.addCopyDirs(copy_dirs);
					}
				}
			}

			gp.setCopyResolver(&copy_resolver);

			if (opt_consolidate->is_set())
				gp.addStep(std::make_shared<TPSourceConsolidation>(&gp));


			if (opt_esql->is_set()) {
				if (opt_varying_ids->is_set())
					gp.setOpt("varlen_suffixes", opt_varying_ids->value());

				gp.setOpt("emit_static_calls", opt_esql_static_calls->is_set());
				gp.setOpt("params_style", opt_esql_param_style->value());
				gp.setOpt("preprocess_copy_files", opt_esql_preprocess_copy->is_set());
				gp.setOpt("consolidated_map", true);
				gp.setOpt("emit_map_file", opt_emit_map_file->is_set());
				gp.setOpt("emit_cobol85", opt_emit_cobol85->is_set());
				gp.setOpt("picx_as_varchar", to_lower(opt_picx_as_varchar->value()) == "varchar");
				gp.setOpt("debug_parser_scanner", opt_parser_scanner_debug->is_set());

				if (opt_esql_copy_exts->is_set())
					copy_resolver.setExtensions(string_split(opt_esql_copy_exts->value(), ","));

				if (opt_no_rec_code->is_set()) {
					std::string c = opt_no_rec_code->value();
					int i = atoi(c.c_str());
					if (i != 0 && i >= -999999999 && i <= 999999999) {
						gp.setOpt("no_rec_code", i);
					}
				}

				gp.addStep(std::make_shared<TPESQLParser>(&gp));

				esql_generator = std::make_shared<TPESQLProcessor>(&gp);
				gp.addStep(esql_generator);
			}

			gp.setOpt("emit_debug_info", opt_debug_info->is_set());
			gp.verbose = opt_verbose->is_set();
			gp.verbose_debug = opt_verbose_debug->is_set();


			std::string infile = opt_infile->value(0);
			std::string outfile = opt_outfile->value(0);
			std::string outext;

			if (is_alias(outfile, outext)) {
				outfile = get_basename(infile) + "." + outext;
			}

			if (infile == outfile) {
				fprintf(stderr, "ERROR: input and output file must be different\n");
				return 1;
			}

			gp.setInputFile(infile);
			gp.setOutputFile(outfile);

			bool b = gp.process();
			if (!b) {
				rc = gp.err_data.err_code;
				for (std::string m : gp.err_data.err_messages)
					fprintf(stderr, "%s\n", m.c_str());
			}

			for (std::string w : gp.err_data.warnings)
				fprintf(stderr, "%s\n", w.c_str());

			rc = gp.err_data.err_code;

		}

		return rc;
	}

}

bool is_alias(const std::string& f, std::string& ext)
{
	std::filesystem::path p(f);

	if (p.stem().string() == "@") {
		ext = p.extension().string();
		return true;
	}

	return false;
}

std::string get_basename(const std::string& f)
{
	std::filesystem::path p(f);

	return p.stem().string();
}
