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

#include "GixPreProcessor.h"

#include <string>

#include "FileData.h"
#include "libcpputils.h"
#include "TPESQLParser.h"
#include "TPESQLProcessor.h"

#define SET_PP_ERR(I,S) err_data.err_code = I; err_data.err_messages.push_back(S)

GixPreProcessor::GixPreProcessor()
{
	check_update_status = true;
	keep_temp_files = false;
	verbose = false;
	verbose_debug = false;

	err_data.err_code = 0;
}

GixPreProcessor::~GixPreProcessor()
{
	//for (auto step : steps) {
	//	if (step)
	//		delete step;
	//}

	if (input)
		delete input;

	if (output)
		delete output;
}

void GixPreProcessor::setCopyResolver(const CopyResolver *cr)
{
	copy_resolver = const_cast<CopyResolver *>(cr);
}

CopyResolver *GixPreProcessor::getCopyResolver() const
{
	return copy_resolver;
}

//void GixPreProcessor::setCopyDirs(const std::stringList cdl)
//{
//	copy_dirs = cdl;
//}


void GixPreProcessor::addCustomStep(std::shared_ptr<ITransformationStep> stp)
{
	this->addStep(stp);
}

struct AnyGet
{
	std::string operator()(bool value) { return value ? "true" : "false"; }
	std::string operator()(char value) { return std::string(1, value); }
	std::string operator()(int value) { return std::to_string(value); }
	std::string operator()(double value) { return std::to_string(value); }
	std::string operator()(const std::string &value) { return value; }
};

static std::string variant_to_string(const variant &input)
{
	return std::visit(AnyGet{}, input);
}


bool GixPreProcessor::process()
{

	if (steps.empty()) {
		return false;
	}

	TransformationStepData* infile = new TransformationStepData();
	infile->setType(TransformationStepDataType::Filename);
	infile->setFilename(_infile);
	firstStep()->setInput(infile);
	input = infile;

	TransformationStepData* outfile = new TransformationStepData();
	outfile->setType(TransformationStepDataType::Filename);
	outfile->setFilename(_outfile);
	lastStep()->setOutput(outfile);
	output = outfile;

    if (!input->isValid()) {
		SET_PP_ERR(1, "Bad input file");
        return false;
    }

    if (!std::get<bool>(getOpt("no_output", false)) && (output == nullptr || !output->isValid())) {
		SET_PP_ERR(2, "Bad output file");
        return false;
    }

    if (!file_exists(input->filename())) {
		SET_PP_ERR(4, "Input file does not exist");
        return false;
    }

	if (verbose) {
		printf("ESQL: Input file: %s\n", input->string().c_str());
		printf("ESQL: Output file: %s\n", output->string().c_str());
		for (std::string cd : copy_resolver->getCopyDirs()) {
			printf("ESQL: Copy dir: %s\n", cd.c_str());
		}
		for (std::string ce : copy_resolver->getExtensions()) {
			printf("ESQL: Copy extension: %s\n", ce.c_str());
		}
		for (auto it = this->getOpts().begin(); it != this->getOpts().end(); ++it) {
			std::string k = it->first;
			std::string v = variant_to_string(it->second);
			printf("ESQL: Option [%s] : [%s]\n", k.c_str(), v.c_str());
		}
	}

	bool b = this->transform();

	return b;
}

bool GixPreProcessor::transform()
{
	std::shared_ptr<ITransformationStep> prev_step = nullptr;
	for (std::shared_ptr<ITransformationStep> step : this->steps) {

		if (step != steps.at(0))
			step->setInput(prev_step->getOutput());

		if (!step->run(prev_step)) {
			return false;
		}
		
		prev_step = step;
		
	}

	return true;
}

void GixPreProcessor::addStep(std::shared_ptr<ITransformationStep> s)
{
	steps.push_back(s);
}

void GixPreProcessor::setInputFile(std::string i)
{
	_infile = i;
}

void GixPreProcessor::setOutputFile(std::string o)
{
	_outfile = o;

}

std::string GixPreProcessor::getInputFile()
{
	return input->filename();
}

std::string GixPreProcessor::getOutputFile()
{
	return output->filename();
}

variant GixPreProcessor::getOpt(std::string id, bool b)
{	
	return map_contains<std::string, variant>(opts, id) ? opts[id] : variant(b);
}

variant GixPreProcessor::getOpt(std::string id, std::string s)
{
	return map_contains<std::string, variant>(opts, id) ? opts[id] : variant(s);
}

variant GixPreProcessor::getOpt(std::string id, int i)
{
	return map_contains<std::string, variant>(opts, id) ? opts[id] : variant(i);
}

variant_map& GixPreProcessor::getOpts() const
{
	return const_cast<variant_map &>(opts);
}

void GixPreProcessor::setOpt(std::string id, variant v)
{
	opts[id] = v;
}

std::shared_ptr<ITransformationStep> GixPreProcessor::firstStep()
{
	return (steps.size() > 0) ? steps.front() : nullptr;
}

std::shared_ptr<ITransformationStep> GixPreProcessor::lastStep()
{
	return (steps.size() > 0) ? steps.back() : nullptr;
}

bool GixPreProcessor::isLastStep(std::shared_ptr<ITransformationStep> s)
{
	return s == steps.back();
}

bool GixPreProcessor::isFirstStep(std::shared_ptr<ITransformationStep> s)
{
	return s == steps.front();
}
