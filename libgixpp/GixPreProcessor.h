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
#include <map>
#include <vector>
#include <variant>

#include "ITransformationStep.h"
#include "CopyResolver.h"
#include "ErrorData.h"

class FileData;

using variant = std::variant<int, float, bool, std::string>;
using variant_map = std::map<std::string, variant>;

class GixPreProcessor
{
public:
	GixPreProcessor();
	~GixPreProcessor();

	bool check_update_status = false;

	bool keep_temp_files = false;
	bool verbose = false;
	bool verbose_debug = false;

	void setCopyResolver(const CopyResolver *cr);
	CopyResolver *getCopyResolver() const;

	void addCustomStep(std::shared_ptr<ITransformationStep> stp);

	ErrorData err_data;

	bool process();
	
	void addStep(std::shared_ptr<ITransformationStep>);
	void setInputFile(std::string infile);
	void setOutputFile(std::string outfile);

	std::string getInputFile();
	std::string getOutputFile();

	variant_map& getOpts() const;
	variant getOpt(std::string id, bool b);
	variant getOpt(std::string id, std::string s);
	variant getOpt(std::string id, int i);
	void setOpt(std::string id, variant v);

	std::shared_ptr<ITransformationStep> firstStep();
	std::shared_ptr<ITransformationStep> lastStep();
	bool isLastStep(std::shared_ptr<ITransformationStep>);
	bool isFirstStep(std::shared_ptr<ITransformationStep>);

private:
	std::string _infile;
	std::string _outfile;

	TransformationStepData* input = nullptr;
	TransformationStepData* output = nullptr;

	std::vector<std::shared_ptr<ITransformationStep>> steps;
	std::string copy_file_path;
	std::vector<std::string> copy_dirs;

	variant_map opts;

	CopyResolver *copy_resolver;

	bool transform();
};

