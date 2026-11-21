#include "VerilogChecker.h"
#include "State.h"
#include "StateMaker.h"
#include "Timer.h"
#include "Utils.h"
#include "Shell.h"
#include "../emit/AssertionWriter.h"
#include <unistd.h>

#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>

#include <filesystem>

namespace fs = std::filesystem;

std::string trim(const std::string &str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

VerilogChecker::VerilogChecker() {
    verilogSrcPath = "";
    formalFilePath = "";
    tracePath = "";
    bound = 3;
}

VerilogChecker::VerilogChecker(std::string verilogSrcPath, BackEndSolver solver) {
    this->verilogSrcPath = verilogSrcPath;
    this->solver = solver;
    bound = 3;
    unboundCheck = false;
}

VerilogChecker::VerilogChecker(std::string verilogSrcPath,std::string homePath,BackEndSolver solver) {
    this->verilogSrcPath = verilogSrcPath;
    this->homePath = homePath;
    this->solver = solver;
    bound = 3;
    unboundCheck = false;
    std::string relatePath = homePath+"/runtime/verilog";

    try {
        for (const auto& entry : fs::directory_iterator(relatePath)) {
            if (entry.is_regular_file()) {
                auto path = entry.path();
                if ((path.extension() == ".sv" || path.extension() == ".v") && path != verilogSrcPath) {
                    relatedFilePaths.push_back(path);
                }
            }
        }
    } catch (const fs::filesystem_error&) {
    }
    std::sort(relatedFilePaths.begin(), relatedFilePaths.end());
}

VerilogChecker::~VerilogChecker() {
}

void VerilogChecker::setVerilogSrcPath(std::string path) {
    verilogSrcPath = path;
}

void VerilogChecker::setformalFilePath(std::string path) {
    formalFilePath = path;
}

void VerilogChecker::setTracePath(std::string path) {
    tracePath = path;
}

void VerilogChecker::setDesignFiles(const std::vector<std::string>& files)
{
    if(files.empty())
        return;
    const auto target = std::find(files.begin(), files.end(), verilogSrcPath);
    if(target == files.end())
        throw std::runtime_error("EBMC input list is missing " + verilogSrcPath);

    designFileIndex = static_cast<std::size_t>(target - files.begin());
    relatedFilePaths = files;
    relatedFilePaths.erase(relatedFilePaths.begin() + designFileIndex);
}

void VerilogChecker::writeVerilogFile()
{
    std::ifstream input(verilogSrcPath);
    if(!input)
        throw std::runtime_error("cannot read " + verilogSrcPath);
    std::ostringstream text;
    text << input.rdbuf();
    const auto source = text.str();
    if(propertyLabel.empty())
        propertyLabel = smart::helper::freshPropertyLabel(source, relatedFilePaths);

    std::ofstream output(formalFilePath);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << smart::emit::injectAssertions(source, injectModule.empty() ? topModule : injectModule,
                                           properties, clock, propertyLabel);
    output.close();
}

void VerilogChecker::addProperty(State* state,PropertyType type) {
    propertyTypes.push_back(type);

    //if we want to verifiy that a state is reachable
    //we should make !state as the property
    //if there is verified, then the state is unreachable otherwise it is reachable
    if(type == PropertyType::REACHABILITY_PROPERTY) {
        assert(!state->isUndefined());//only defined states can be verified

        properties.push_back("(!"+state->toVerilogExpr()+")");
    }
}

void VerilogChecker::addProperty(SygusFunction* func,PropertyType type) {
    propertyTypes.push_back(type);

    if(type == PropertyType::REACHABILITY_PROPERTY){
        properties.push_back(func->getBodyVerilogExpr());
    }
    else if(type == PropertyType::SAFT_PROPERTY){
        properties.push_back(func->getBodyVerilogExpr());
    }
    else{
        printError("Error: Property type not supported\n");
        exit(1);
    }
    
}

void VerilogChecker::cleanProperties() {
    properties.clear();
    propertyTypes.clear();
}

smart::helper::EbmcResult VerilogChecker::runEBMC(const std::string& trace,
                                                  const std::string& scope)
{
    smart::helper::EbmcOptions options;
    options.topModule = topModule;
    options.extraFiles = relatedFilePaths;
    options.designFileIndex = designFileIndex;
    options.bound = bound;
    options.unbounded = unboundCheck;

    timer->start(timerType::FM_Timer);
    const auto result = smart::helper::checkProperty(formalFilePath, options, propertyLabel,
                                                    trace, scope);
    timer->stop(timerType::FM_Timer);
    if(result.status == smart::helper::EbmcStatus::Error)
        throw std::runtime_error("EBMC failed for " + formalFilePath + ": " + result.detail);
    return result;
}

//The check function will return true if the state is reachable
bool VerilogChecker::checkStateReachability(State* state) {
    cleanProperties();
    generateFormalFilePath(PropertyType::REACHABILITY_PROPERTY);
    addProperty(state,PropertyType::REACHABILITY_PROPERTY);
    writeVerilogFile();
    bool result = false;
    if(solver==BackEndSolver::EBMC) {
        // Reachability gets its own depth, because it asks a different
        // question from "does this candidate hold": too shallow and a state
        // the design can enter looks unreachable, gets handed to SyGuS as a
        // negative example, and every candidate built on it is refuted.
        //
        // -1 selects k-induction, which never mislabels a reachable state —
        // but it is often inconclusive, and an inconclusive answer loses the
        // negative example altogether. Measured on a deep pipeline design:
        // bound 10 -> 0 assertions, bound 40 -> 2, k-induction -> 0. A false
        // negative example costs search time, not soundness (the final check
        // still gates every emitted assertion), so depth beats rigour here.
        const bool savedUnboundCheck = unboundCheck;
        const int savedBound = bound;
        if(reachabilityBound < 0){
            unboundCheck = true;
        }
        else{
            unboundCheck = false;
            bound = reachabilityBound;
        }
        // Only a proof of !state supplies a negative example.
        result = runEBMC().status == smart::helper::EbmcStatus::Verified;
        unboundCheck = savedUnboundCheck;
        bound = savedBound;
    }
    else if(solver==BackEndSolver::SBY) {
        result = runSby();
    }
    if(deleteTempFile)
        deleteVerilogFile();
    return !result;
}

//The check function will return true if the property is verified/ the state is safe
smart::helper::EbmcResult VerilogChecker::checkExprSafety(SygusFunction* func,
                                                           const std::string& tracePath,
                                                           const std::string& scope)
{
    using smart::helper::EbmcStatus;
    cleanProperties();
    generateFormalFilePath(PropertyType::SAFT_PROPERTY);
    addProperty(func, PropertyType::SAFT_PROPERTY);
    writeVerilogFile();

    smart::helper::EbmcResult result{EbmcStatus::Error, {}};
    if(solver == BackEndSolver::EBMC)
        result = runEBMC(tracePath, scope);
    else if(solver == BackEndSolver::SBY)
    {
        result.status = runSby() ? EbmcStatus::Verified : EbmcStatus::Refuted;
        if(result.status == EbmcStatus::Refuted)
        {
            const auto vcdPath = homePath + "/runtime/formal/dis_task/engine_0/trace.vcd";
            fs::copy_file(vcdPath, tracePath, fs::copy_options::overwrite_existing);
            result.counterexample = tracePath;
        }
    }
    if(deleteTempFile)
        deleteVerilogFile();
    return result;
}

std::string VerilogChecker::generateFormalFilePath(PropertyType type) {

    auto now = std::chrono::system_clock::now();

    std::time_t timeNow = std::chrono::system_clock::to_time_t(now);

    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&timeNow), "%Y%m%d_%H%M%S") 
        << "_" << std::setw(3) << std::setfill('0') << millis;

    std::string filename = homePath + "/runtime/formal";
    if(type == PropertyType::REACHABILITY_PROPERTY) {
        filename +="/reachability_";
    }
    else if(type == PropertyType::SAFT_PROPERTY) {
        filename +="/safety_";
    }
    else {
        printError("Error: Property type not supported\n");
        exit(1);
    }
    filename += oss.str();
    filename += std::to_string(getpid());
    filename += "_";
    filename +=".sv";
    formalFilePath = filename;
    return filename;
}

void VerilogChecker::setBound(int bound) {
    this->bound = bound;
}

void VerilogChecker::setReachabilityBound(int reachabilityBound) {
    this->reachabilityBound = reachabilityBound;
}

void VerilogChecker::setUnboundCheck(bool unboundCheck) {
    this->unboundCheck = unboundCheck;
}

void VerilogChecker::setInjectModule(std::string injectModule) {
    this->injectModule = injectModule;
}

void VerilogChecker::setTopModule(std::string topModule) {
    this->topModule = topModule;
}

void VerilogChecker::setClock(std::string clock) {
    this->clock = std::move(clock);
}

bool VerilogChecker::runSby() {
    //we should write an dis.sby file
    std::string sbyFilePath = "runtime/formal/dis.sby";
    std::ofstream sbyFile(sbyFilePath);
    if(!sbyFile.is_open()) {
        printError("Error: Unable to open sby file "+sbyFilePath+"\n");
        exit(1);
    }    
    sbyFile << "[tasks]" << std::endl;
    sbyFile << "task bmc_check files" << std::endl;
    sbyFile << std::endl;

    sbyFile << "[options]" << std::endl;
    sbyFile << "bmc_check:" << std::endl;
    sbyFile << "mode bmc" << std::endl;
    sbyFile << "depth " << 3 << std::endl;
    // sbyFile << "depth " << bound << std::endl;
    sbyFile << "timeout 120" << std::endl;
    sbyFile << "vcd_sim on"<<std::endl;
    // sbyFile << "append 10"<<std::endl;
    // sbyFile << "skip 5"<<std::endl;
    
    // sbyFile << "vcd_sim on" << std::endl;
    sbyFile << std::endl;

    sbyFile << "[engines]" << std::endl;
    sbyFile << "bmc_check:" << std::endl;
    sbyFile << "smtbmc --unroll"<<std::endl;
    sbyFile << std::endl;

    sbyFile << "[script]" << std::endl;
    sbyFile << "files:" << std::endl;
    sbyFile << "read -sv " << formalFilePath << std::endl;
    for(auto &path : relatedFilePaths) {
        sbyFile << "read -sv " << path << std::endl;
    }
    sbyFile << "prep -top " << topModule << std::endl;
    sbyFile.close();

    // print("Successfully wrote sby file "+sbyFilePath+"\n");


    std::string command = "";
    command += smart::helper::shellQuote("sby")+" "+smart::helper::shellQuote(sbyFilePath);
    command += " -f";

    command += " > /dev/null 2>&1";
    printDebug("Running SBY with command: "+command+"\n",1);
    timer->start(timerType::FM_Timer);
    int status = system(command.c_str());
    // print("The command is "+command+"\n");
    // print("The status is "+std::to_string(status)+"\n");
    timer->stop(timerType::FM_Timer);
    printDebug("SBY result is "+std::to_string(status),1);
    return status==0;
}

void VerilogChecker::setHomePath(std::string path) {
    homePath = path;
}

Constrains VerilogChecker::fixupConstrains(Constrains constrains) {
    int timeStamp = constrains.constraints[0].size();
    int signalSize = constrains.constraints.size();
    bool isUnDefined = false;

    Constrains newConstrains;
    
    for(auto constraint : constrains.constraints) {
        std::vector<Value*> newConstraint;
        newConstrains.constraints.push_back(newConstraint);
    }
    newConstrains.tracePath = constrains.tracePath; 
    newConstrains.isTrue = constrains.isTrue;

    for(int time=0;time<timeStamp;time++) {
        int count = 0;
        isUnDefined = false;
        //time Stamp i
        for(int signalIndex=0;signalIndex<signalSize;signalIndex++) {
            //signal i
            Value* value = constrains.constraints[signalIndex][time];
            if(value->isUndefined()) {
                isUnDefined = true;
                count++;
                break;
            }
        }
        //if there is any undefined value, we should fix up the value
        if(isUnDefined) {
            //Time is time
            std::vector<Value*> values;
            for(int signalIndex=0;signalIndex<signalSize;signalIndex++) {
                //signal i
                Value* value = constrains.constraints[signalIndex][time];
                values.push_back(value);
            }

            // print("\t\tSMT File constains "+std::to_string(count)+" undefined values");

            //make the state reachable
            State* state = makeReachableState(values);
            std::vector<Value*> newValues = state->getValues();
            delete state;
            
            //add the new values to the new constrains
            for(int signalIndex=0;signalIndex<signalSize;signalIndex++) {
                newConstrains.constraints[signalIndex].push_back(newValues[signalIndex]);
            }

            continue;
        //else we just use the original value
        }else{
            for(int signalIndex=0;signalIndex<signalSize;signalIndex++) {
                //signal i
                Value* value = constrains.constraints[signalIndex][time];
                if(value->isUndefined()){
                    print("Error: Undefined value in the constrains\n");
                    exit(1);
                }
                newConstrains.constraints[signalIndex].push_back(value);
            }
        }
    }
    return newConstrains;
}

void VerilogChecker::setSignals(std::vector<Signal>* setSignals) {
    this->signals = setSignals;
}

State* VerilogChecker::makeReachableState(std::vector<Value*> values){
    // print("\t\tTry to fix up the state\n");
    StateMaker sm(signals);
    State* state = sm.fixUpState(values);

    if(checkStateReachability(state)) {
        return state;
    }
    else{
        State* refined = sm.fixUpState(state,values);
        delete state;
        return refined;
    }
}

void VerilogChecker::setTimer(Timer* timer) {
    this->timer = timer;
}

void VerilogChecker::deleteVerilogFile(){
    std::error_code error;
    const bool removed = fs::remove(formalFilePath, error);
    if(error || !removed) {
        printError("Error: Unable to delete file "+formalFilePath+"\n");
        exit(1);
    }
    printDebug("Deleted file "+formalFilePath,3);
}
