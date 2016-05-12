/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file 
 * distributed with this source distribution.
 * 
 * This file is part of REDHAWK core.
 * 
 * REDHAWK core is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU Lesser General Public License as published by the 
 * Free Software Foundation, either version 3 of the License, or (at your 
 * option) any later version.
 * 
 * REDHAWK core is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License 
 * for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License 
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */


#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <algorithm>
#include <functional>
#include <set>
#include <list>
#include <unistd.h>

#include <boost/filesystem/path.hpp>

#include <ossie/CF/WellKnownProperties.h>
#include <ossie/FileStream.h>
#include <ossie/prop_helpers.h>
#include <ossie/prop_utils.h>

#include "Application_impl.h"
#include "ApplicationFactory_impl.h"
#include "createHelper.h"
#include "DomainManager_impl.h"
#include "AllocationManager_impl.h"
#include "RH_NamingContext.h"

namespace fs = boost::filesystem;
using namespace ossie;
using namespace std;

ScopedAllocations::ScopedAllocations(AllocationManager_impl& allocator):
    _allocator(allocator)
{
}

ScopedAllocations::~ScopedAllocations()
{
    try {
        deallocate();
    } catch (...) {
        // Destructors must not throw
    }
}

void ScopedAllocations::push_back(const std::string& allocationID)
{
    _allocations.push_back(allocationID);
}

template <class T>
void ScopedAllocations::transfer(T& dest)
{
    std::copy(_allocations.begin(), _allocations.end(), std::back_inserter(dest));
    _allocations.clear();
}

void ScopedAllocations::transfer(ScopedAllocations& dest)
{
    transfer(dest._allocations);
}

void ScopedAllocations::deallocate()
{
    if (!_allocations.empty()) {
        LOG_TRACE(ApplicationFactory_impl, "Deallocating " << _allocations.size() << " allocations");
        _allocator.deallocate(_allocations.begin(), _allocations.end());
    }
}



/* Rotates a device list to put the device with the given identifier first
 */
static void rotateDeviceList(DeviceList& devices, const std::string& identifier)
{
    const DeviceList::iterator begin = devices.begin();
    for (DeviceList::iterator node = begin; node != devices.end(); ++node) {
        if ((*node)->identifier == identifier) {
            if (node != begin) {
                std::rotate(devices.begin(), node, devices.end());
            }
            return;
        }
    }
}

static std::vector<std::string> mergeProcessorDeps(const ossie::ImplementationInfo::List& implementations)
{
    // this function merges the overlap in processors between the different components that have been selected
    std::vector<std::string> processorDeps;
    for (ossie::ImplementationInfo::List::const_iterator impl = implementations.begin(); impl != implementations.end(); ++impl) {
        const std::vector<std::string>& implDeps = (*impl)->getProcessorDeps();
        if (!implDeps.empty()) {
            if (processorDeps.empty()) {
                // No prior processor dependencies, so overwrite
                processorDeps = implDeps;
            } else {
                std::vector<std::string> toremove;
                toremove.resize(0);
                for (std::vector<std::string>::iterator proc = processorDeps.begin(); proc != processorDeps.end(); ++proc) {
                    if (std::find(implDeps.begin(), implDeps.end(), *proc) == implDeps.end()) {
                        toremove.push_back(*proc);
                    }
                }
                for (std::vector<std::string>::iterator _rem = toremove.begin(); _rem != toremove.end(); ++_rem) {
                    std::vector<std::string>::iterator proc = std::find(processorDeps.begin(), processorDeps.end(), *_rem);
                    if (proc != processorDeps.end()) {
                        processorDeps.erase(proc);
                    }
                }
            }
        }
    }
    return processorDeps;
}

static std::vector<ossie::SPD::NameVersionPair> mergeOsDeps(const ossie::ImplementationInfo::List& implementations)
{
    // this function merges the overlap in operating systems between the different components that have been selected
    std::vector<ossie::SPD::NameVersionPair> osDeps;
    for (ossie::ImplementationInfo::List::const_iterator impl = implementations.begin(); impl != implementations.end(); ++impl) {
        const std::vector<ossie::SPD::NameVersionPair>& implDeps = (*impl)->getOsDeps();
        if (!implDeps.empty()) {
            if (osDeps.empty()) {
                // No prior OS dependencies, so overwrite
                osDeps = implDeps;
            } else {
                std::vector<ossie::SPD::NameVersionPair> toremove;
                toremove.resize(0);
                for (std::vector<ossie::SPD::NameVersionPair>::iterator pair = osDeps.begin(); pair != osDeps.end(); ++pair) {
                    if (std::find(implDeps.begin(), implDeps.end(), *pair) == implDeps.end()) {
                        toremove.push_back(*pair);
                    }
                }
                for (std::vector<ossie::SPD::NameVersionPair>::iterator _rem = toremove.begin(); _rem != toremove.end(); ++_rem) {
                    std::vector<ossie::SPD::NameVersionPair>::iterator pair = std::find(osDeps.begin(), osDeps.end(), *_rem);
                    if (pair != osDeps.end()) {
                        osDeps.erase(pair);
                    }
                }
            }
        }
    }
    return osDeps;
}

PREPARE_CF_LOGGING(ApplicationFactory_impl);

void
ApplicationFactory_impl::ValidateFileLocation( CF::FileManager_ptr fileMgr, const std::string &profile_file)
{
    TRACE_ENTER(ApplicationFactory_impl)

    if (profile_file == "") {
        TRACE_EXIT(ApplicationFactory_impl)
        return;
    }

    // Verify file within the provided FileMgr
    LOG_TRACE(ApplicationFactory_impl, "Validating that profile " << profile_file << " exists");
    if (!fileMgr->exists (profile_file.c_str())) {
        string msg = "File ";
        msg += profile_file;
        msg += " does not exist.";
        throw CF::FileException (CF::CF_ENOENT, msg.c_str());
    }
}


void ApplicationFactory_impl::ValidateSoftPkgDep (CF::FileManager_ptr fileMgr, DomainManager_impl *domMgr, const std::string& sfw_profile )  {
  SoftPkg pkg;
  ValidateSPD(fileMgr, domMgr, pkg, sfw_profile, false, false );
}

std::string ApplicationFactory_impl::xmlParsingVersionMismatch(DomainManager_impl *domMgr, std::string &component_version)
{
    std::string added_message;
    if (!component_version.empty()) {
        try {
            static std::string version = domMgr->getRedhawkVersion();
            if (redhawk::compareVersions(component_version, version) < 0) {
                added_message = "Attempting to run a component from version ";
                added_message += component_version;
                added_message += " on REDHAWK version ";
                added_message += version;
                added_message += ". ";
            }
        } catch ( ... ) {}
    }
    return added_message;
}

void ApplicationFactory_impl::ValidateSPD(CF::FileManager_ptr fileMgr, 
                                          DomainManager_impl *domMgr, 
                                          SoftPkg &spdParser, 
                                          const std::string& sfw_profile, 
                                          const bool require_prf, 
                                          const bool require_scd) {
    TRACE_ENTER(ApplicationFactory_impl)

    if ( sfw_profile == "" ) {
      LOG_WARN( ApplicationFactory_impl, "No Software Profile Provided.");
      throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "No software profile provided");
      TRACE_EXIT(ApplicationFactory_impl);
    }

    try {
        LOG_TRACE(ApplicationFactory_impl, "Validating SPD " << sfw_profile);
        ValidateFileLocation(fileMgr, sfw_profile);

        // check the filename ends with the extension given in the spec
        if ((strstr (sfw_profile.c_str(), ".spd.xml")) == NULL)
            { LOG_ERROR(ApplicationFactory_impl, "File " << sfw_profile << " should end with .spd.xml"); }
        LOG_TRACE(ApplicationFactory_impl, "validating " << sfw_profile);

        try {
            File_stream _spd(fileMgr, sfw_profile.c_str());
            spdParser.load( _spd,  sfw_profile.c_str() );
            _spd.close();
        } catch (ossie::parser_error& ex) {
            File_stream _spd(fileMgr, sfw_profile.c_str());
            std::string line;
            std::string component_version;
            while (std::getline(_spd, line)) {
                size_t type_idx = line.find("type");
                if (type_idx != std::string::npos) {
                    size_t first_quote = line.find('"', type_idx);
                    if (first_quote == std::string::npos)
                        continue;
                    size_t second_quote = line.find('"', first_quote + 1);
                    if (second_quote == std::string::npos)
                        continue;
                    component_version = line.substr(first_quote + 1, second_quote-(first_quote+1));
                    break;
                }
            }
            ostringstream eout;
            eout << xmlParsingVersionMismatch(domMgr, component_version);
            std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
            eout << "Failed to parse SPD: " << sfw_profile << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what();
            LOG_ERROR(ApplicationFactory_impl, eout.str() );
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, eout.str().c_str());
        } catch (CF::InvalidFileName ex) {
            LOG_ERROR(ApplicationFactory_impl, "Failed to validate SPD: " << sfw_profile << ". Invalid file name exception: " << ex.msg);
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
        } catch (CF::FileException ex) {
            LOG_ERROR(ApplicationFactory_impl, "Failed to validate SPD: " << sfw_profile << ". File exception: " << ex.msg);
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
        } catch ( ... ) {
            LOG_ERROR(ApplicationFactory_impl, "Unexpected error validating SPD: " << sfw_profile );
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "");
        }

        //
        // validate each implementation
        //
        const ossie::SPD::Implementations& impls = spdParser.getImplementations();
        ossie::SPD::Implementations::const_iterator impl = impls.begin();
        for( ; impl != impls.end(); impl++ ) {

          
          // validate code file exists
          try {
            boost::filesystem::path implPath = boost::filesystem::path( spdParser.getSPDPath()) /  impl->getCodeFile();
            LOG_TRACE(ApplicationFactory_impl, "Validating Implmentation existance: " << implPath.string() );
            ValidateFileLocation( fileMgr, implPath.string().c_str() );
          } catch (CF::InvalidFileName ex) {
            LOG_ERROR(ApplicationFactory_impl, "Invalid Code File,  PROFILE:" << sfw_profile << "  CODE:" << impl->getCodeFile());
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
          } catch (CF::FileException ex) {
            LOG_ERROR(ApplicationFactory_impl, "Invalid Code File,  PROFILE:" << sfw_profile << "  CODE:" << impl->getCodeFile());
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
          } catch ( ... ) {
            LOG_ERROR(ApplicationFactory_impl, "Unexpected error validating PRF " << spdParser.getPRFFile());
            throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "");
          }

          // validate all the soft package dependencies....
          const ossie::SPD::SoftPkgDependencies& deps = impl->getSoftPkgDependencies();
          ossie::SPD::SoftPkgDependencies::const_iterator dep = deps.begin();
          for(; dep != deps.end(); dep++ ) {
            try {
              LOG_TRACE(ApplicationFactory_impl, "Validating Dependency: " << dep->localfile);
              ValidateSoftPkgDep(fileMgr, domMgr, dep->localfile);
            } catch (CF::InvalidFileName ex) {
              LOG_ERROR(ApplicationFactory_impl, "Invalid Code File,  PROFILE:" << sfw_profile << "  CODE:" << impl->getCodeFile());
              throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
            } catch (CF::FileException ex) {
              LOG_ERROR(ApplicationFactory_impl, "Invalid Code File,  PROFILE:" << sfw_profile << "  CODE:" << impl->getCodeFile());
              throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
            }
            
          }

        }
        
        // query SPD for PRF
        if (spdParser.getPRFFile() != 0) {
            LOG_TRACE(ApplicationFactory_impl, "validating " << spdParser.getPRFFile());
            try {
              ValidateFileLocation ( fileMgr, spdParser.getPRFFile ());

                // check the file name ends with the extension given in the spec
                if (spdParser.getPRFFile() && (strstr (spdParser.getPRFFile (), ".prf.xml")) == NULL) {
                    LOG_ERROR(ApplicationFactory_impl, "File " << spdParser.getPRFFile() << " should end in .prf.xml.");
                }

                LOG_TRACE(ApplicationFactory_impl, "Creating file stream")
                File_stream prfStream(fileMgr, spdParser.getPRFFile());
                LOG_TRACE(ApplicationFactory_impl, "Loading parser")
                Properties prfParser(prfStream);
                LOG_TRACE(ApplicationFactory_impl, "Closing stream")
                prfStream.close();
            } catch (ossie::parser_error& ex ) {
                ostringstream eout;
                std::string component_version(spdParser.getSoftPkgType());
                eout << xmlParsingVersionMismatch(domMgr, component_version);
                std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
                eout <<  "Failed to parse PRF: " << spdParser.getPRFFile() << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what();
                LOG_ERROR(ApplicationFactory_impl, eout.str() );
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, eout.str().c_str());
            } catch (CF::InvalidFileName ex) {
              if ( require_prf ) {
                  LOG_ERROR(ApplicationFactory_impl, "Failed to validate PRF: " << spdParser.getPRFFile() << " Invalid file name exception: "  << ex.msg);
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
              }
            } catch (CF::FileException ex) {
              if ( require_prf ) {
                  LOG_ERROR(ApplicationFactory_impl, "Failed to validate PRF: " << spdParser.getPRFFile() << " File exception: "  << ex.msg);
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
              }
            } catch ( ... ) {
                LOG_ERROR(ApplicationFactory_impl, "Unexpected error validating PRF: " << spdParser.getPRFFile());
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "");
            }
        } else {
            LOG_TRACE(ApplicationFactory_impl, "No PRF file to validate")
        }

        if (spdParser.getSCDFile() != 0) {
            try {
              // query SPD for SCD
              LOG_TRACE(ApplicationFactory_impl, "validating " << spdParser.getSCDFile());
              ValidateFileLocation ( fileMgr, spdParser.getSCDFile ());

              // Check the filename ends with  the extension given in the spec
              if ((strstr (spdParser.getSCDFile (), ".scd.xml")) == NULL)
                { LOG_ERROR(ApplicationFactory_impl, "File " << spdParser.getSCDFile() << " should end with .scd.xml."); }

                File_stream _scd(fileMgr, spdParser.getSCDFile());
                ComponentDescriptor scdParser (_scd);
                _scd.close();
            } catch (ossie::parser_error& ex) {
                ostringstream eout;
                std::string component_version(spdParser.getSoftPkgType());
                eout << xmlParsingVersionMismatch(domMgr, component_version);
                std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
                eout << "Failed to parse SCD: " << spdParser.getSCDFile() << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what();
                LOG_ERROR(ApplicationFactory_impl, eout.str() );
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, eout.str().c_str());
            } catch (CF::InvalidFileName ex) {
              if ( require_scd ){
                  LOG_ERROR(ApplicationFactory_impl, "Failed to validate SCD: " << spdParser.getSCDFile() << " Invalid file name exception: " << ex.msg);
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
              }
            } catch (CF::FileException ex) {
              if ( require_scd ) {
                LOG_ERROR(ApplicationFactory_impl, "Failed to validate SCD: " << spdParser.getSCDFile() <<  " File exception: " << ex.msg);
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
              }
            } catch ( ... ) {
                LOG_ERROR(ApplicationFactory_impl, "Unexpected error validating SCD: " << spdParser.getSCDFile());
                throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "");
            }
        } else if (spdParser.isScaCompliant() and require_scd ) {
            LOG_ERROR(ApplicationFactory_impl, "SCA compliant component is missing SCD file reference");
            throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, "SCA compliant components require SCD file");
        } else {
            LOG_TRACE(ApplicationFactory_impl, "No SCD file to validate")
        }

    } catch (CF::InvalidFileName& ex) {
        LOG_ERROR(ApplicationFactory_impl, "Failed to validate SPD: " << sfw_profile << ", exception: " << ex.msg);
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
    } catch (CF::FileException& ex) {
        LOG_ERROR(ApplicationFactory_impl, "Failed to validate SPD: " << sfw_profile << ", exception: " << ex.msg);
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
    } catch (CF::DomainManager::ApplicationInstallationError& ex) {
        throw;
    } catch ( ... ) {
        LOG_ERROR(ApplicationFactory_impl, "Unexpected error validating SPD: " << sfw_profile);
        throw CF::DomainManager::ApplicationInstallationError ();
    }


}


ApplicationFactory_impl::ApplicationFactory_impl (const std::string& softwareProfile,
                                                  const std::string& domainName, 
                                                  DomainManager_impl* domainManager) :
    _softwareProfile(softwareProfile),
    _domainName(domainName),
    _domainManager(domainManager),
    _lastWaveformUniqueId(0)
{
    // Get the naming context from the domain
    _domainContext = RH_NamingContext::GetNamingContext( _domainName, !_domainManager->bindToDomain() );
    if (CORBA::is_nil(_domainContext)) {
        LOG_ERROR(ApplicationFactory_impl, "CosNaming::NamingContext::_narrow threw Unknown Exception");
        throw;
    }

    _dmnMgr = domainManager->_this();

    try {
        _fileMgr = _dmnMgr->fileMgr();
    } catch ( std::exception& ex ) {
        ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<" while retrieving the File Manager";
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch ( const CORBA::Exception& ex ) {
        ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<" while retrieving the File Manager";
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch( ... ) {
        LOG_ERROR(ApplicationFactory_impl, "_dmnMgr->_fileMgr failed with Unknown Exception");
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, "Could not get File Manager from Domain Manager");
    }

    try {

      LOG_INFO(ApplicationFactory_impl, "Installing application " << _softwareProfile.c_str());
      ValidateFileLocation ( _fileMgr, _softwareProfile );

      File_stream _sad(_fileMgr, _softwareProfile.c_str());
      _sadParser.load(_sad);
      _sad.close();
    } catch (const ossie::parser_error& ex) {
        ostringstream eout;
        std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
        eout << "Failed to parse SAD file: " << _softwareProfile << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what();
        LOG_ERROR(ApplicationFactory_impl, eout.str());
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch ( std::exception& ex ) {
        ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<". While loading "<<_softwareProfile;
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch( CF::InvalidFileName& ex ) {
        ostringstream eout;
        eout << "The following InvalidFileName exception occurred, profile: " << _softwareProfile;
        LOG_ERROR(ApplicationFactory_impl, eout.str());
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, eout.str().c_str());
    } catch ( const CF::FileException& ex ) {
        ostringstream eout;
        eout << "The following FileException occurred: "<<ex.msg<<"  While loading "<<_softwareProfile;
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch ( const CORBA::Exception& ex ) {
        ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<" While loading "<<_softwareProfile;
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_EBADF, eout.str().c_str());
    } catch( ... ) {
        ostringstream eout;
        eout << "Parsing SAD file: " <<_softwareProfile << " Failed with unknown exception.";
        LOG_ERROR(ApplicationFactory_impl, eout.str());
        throw CF::DomainManager::ApplicationInstallationError(CF::CF_ENOENT, eout.str().c_str());
    }

    // Makes sure all external port names are unique
    const std::vector<SoftwareAssembly::Port>& ports = _sadParser.getExternalPorts();
    std::vector<std::string> extPorts;
    for (std::vector<SoftwareAssembly::Port>::const_iterator port = ports.begin(); port != ports.end(); ++port) {
        // Gets name to use
        std::string extName;
        if (port->externalname != "") {
            extName = port->externalname;
        } else {
            extName = port->identifier;
        }
        // Check for duplicate
        if (std::find(extPorts.begin(), extPorts.end(), extName) == extPorts.end()) {
            extPorts.push_back(extName);
        } else {
            ostringstream eout;
            eout << "Duplicate External Port name: " << extName;
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::DomainManager::ApplicationInstallationError(CF::CF_NOTSET, eout.str().c_str());
        }
    }

    // Gets the assembly controller software profile by looping through each
    // component instantiation to find a matching ID to the AC's
    std::string assemblyControllerId = _sadParser.getAssemblyControllerRefId();
    SoftPkg ac_spd;
    CORBA::String_var ac_profile = "";
    bool ac_found = false;
    std::vector<ComponentPlacement> components = _sadParser.getAllComponents();
    for (std::vector<ComponentPlacement>::const_iterator comp = components.begin();
            comp != components.end(); ++comp) {
      SoftPkg comp_pkg;
      std::string p_name;
      try {
        if ( _sadParser.getSPDById(comp->getFileRefId())) {
            p_name = _sadParser.getSPDById(comp->getFileRefId());
            LOG_DEBUG(ApplicationFactory_impl, "Validating...  COMP profile: " << p_name);
            ValidateSPD(_fileMgr, _domainManager, comp_pkg, p_name) ;
        }
        else {
          LOG_ERROR(ApplicationFactory_impl, "installApplication: invalid  componentfileref: " << comp->getFileRefId() );
          throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, "installApplication: invalid  componentfileref"); 
        }
      } catch (CF::FileException& ex) {
        LOG_ERROR(ApplicationFactory_impl, "installApplication: While validating the SAD profile: " << ex.msg);
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, ex.msg);
      } catch( CF::InvalidFileName& ex ) {
        std::ostringstream eout;
        eout << "Invalid file name: " << p_name;
        LOG_ERROR(ApplicationFactory_impl, "installApplication: Invalid file name: " << p_name);
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_EBADF, eout.str().c_str());
      } catch (CF::DomainManager::ApplicationInstallationError& e) {
        LOG_TRACE(ApplicationFactory_impl, "rethrowing ApplicationInstallationError" << e.msg);
        throw;
      } catch ( std::exception& ex ) {
        std::ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<" while restoring the application factories";
        LOG_ERROR(ApplicationFactory_impl, eout.str());
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_NOTSET, eout.str().c_str());
      } catch (...) {
        LOG_ERROR(ApplicationFactory_impl, "unexpected exception occurred while installing application");
        throw CF::DomainManager::ApplicationInstallationError (CF::CF_NOTSET, "unknown exception");
      }

      if ( !ac_found ) {
        std::vector<ComponentInstantiation> compInstantiations = comp->instantiations;
        for (std::vector<ComponentInstantiation>::const_iterator compInst = compInstantiations.begin();
             compInst != compInstantiations.end(); ++compInst){
          if (assemblyControllerId == compInst->instantiationId) {
            ac_spd = comp_pkg;
            ac_profile = _sadParser.getSPDById(comp->getFileRefId());
            ac_found = true;
            break;
          }
        }
      }
    }

    // Gets the assembly controllers properties
    Properties prf;
    if (ac_found) {
        if ( ac_spd.getPRFFile() ) {
          std::string prf_file(ac_spd.getPRFFile());
            try {
              File_stream _prf(_fileMgr, prf_file.c_str());
                prf.load(_prf);
                _prf.close();
            } catch(ossie::parser_error& ex ) {
              std::ostringstream os;
              std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
              os << "Invalid PRF file: " << prf_file << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what();
              LOG_ERROR(ApplicationFactory_impl, os.str() );
              throw CF::DomainManager::ApplicationInstallationError(CF::CF_NOTSET, os.str().c_str());
            } catch( ... ) {
                // Errors are reported at create time
            }
        }
    }

    // Makes sure all external property names are unique
    const std::vector<SoftwareAssembly::Property>& properties = _sadParser.getExternalProperties();
    std::vector<std::string> extProps;
    for (std::vector<SoftwareAssembly::Property>::const_iterator prop = properties.begin(); prop != properties.end(); ++prop) {
        // Gets name to use
        std::string extName;
        if (prop->externalpropid != "") {
            extName = prop->externalpropid;
        } else {
            extName = prop->propid;
        }
        // Check for duplicate
        if (std::find(extProps.begin(), extProps.end(), extName) == extProps.end()) {
            extProps.push_back(extName);
        } else {
            ostringstream eout;
            eout << "Duplicate External Property name: " << extName;
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::DomainManager::ApplicationInstallationError(CF::CF_NOTSET, eout.str().c_str());
        }
    }

    // Make sure AC prop ID's aren't in conflict with external ones
    const std::vector<const Property*>& acProps = prf.getProperties();
    for (unsigned int i = 0; i < acProps.size(); ++i) {
        // Check for duplicate
        if (std::find(extProps.begin(), extProps.end(), acProps[i]->getID()) == extProps.end()) {
            extProps.push_back(acProps[i]->getID());
        } else {
            ostringstream eout;
            eout << "Assembly controller property in use as External Property: " << acProps[i]->getID();
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::DomainManager::ApplicationInstallationError(CF::CF_NOTSET, eout.str().c_str());
        }
    }

    _name = _sadParser.getName();
    _identifier = _sadParser.getID();
}

ApplicationFactory_impl::~ApplicationFactory_impl ()
{
  try {
    //
    // remove the naming context assocated with the factory that generates new
    // naming contexts for each application.
    //
    if ( _domainManager && _domainManager->bindToDomain() ) _domainContext->destroy();
  }
  catch(...)
    {};

}

/*
 * Check to make sure assemblyController was initialized if it was SCA compliant
 */
void createHelper::_checkAssemblyController(
    CF::Resource_ptr      assemblyController,
    ossie::ComponentInfo* assemblyControllerComponent) const
{
    if (CORBA::is_nil(assemblyController)) {
        if ((assemblyControllerComponent==NULL) || 
            (assemblyControllerComponent->isScaCompliant())
           ) {
        LOG_DEBUG(ApplicationFactory_impl, "assembly controller is not Sca Compliant or has not been assigned");
        throw (CF::ApplicationFactory::CreateApplicationError(
                    CF::CF_NOTSET, 
                    "assembly controller is not Sca Compliant or has not been assigned"));
        }
    }
}

void createHelper::_connectComponents(ossie::ApplicationDeployment& appDeployment,
                                      std::vector<ConnectionNode>& connections){
    try{
        connectComponents(appDeployment, connections, _baseNamingContext);
    } catch (CF::ApplicationFactory::CreateApplicationError& ex) {
        throw;
    } CATCH_THROW_LOG_TRACE(
        ApplicationFactory_impl,
        "Connecting components failed (unclear where this occurred)",
        CF::ApplicationFactory::CreateApplicationError(
            CF::CF_EINVAL, 
            "Connecting components failed (unclear where this occurred)"));
}

void createHelper::_configureComponents(const DeploymentList& deployments)
{
    try{
        configureComponents(deployments);
    } catch (CF::ApplicationFactory::CreateApplicationError& ex) {
        throw;
    } CATCH_THROW_LOG_TRACE(
        ApplicationFactory_impl, 
        "Configure on component failed (unclear where in the process this occurred)",
        CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, "Configure of component failed (unclear where in the process this occurred)"))
}

void createHelper::assignPlacementsToDevices(ossie::ApplicationPlacement& appPlacement,
                                             ossie::ApplicationDeployment& appDeployment,
                                             const std::string& appIdentifier, const DeviceAssignmentMap& devices)
{
    typedef ossie::ApplicationPlacement::PlacementList PlacementPlanList;
    const PlacementPlanList& placements = appPlacement.getPlacements();
    for (PlacementPlanList::const_iterator plan = placements.begin(); plan != placements.end(); ++plan) {
        const std::vector<ComponentInfo*>& components = (*plan)->getComponents();
        if (components.size() > 1) {
            LOG_TRACE(ApplicationFactory_impl, "Placing host collocation " << (*plan)->getId()
                      << " " << (*plan)->getName());
            _placeHostCollocation(appDeployment, appIdentifier, components, devices);
            LOG_TRACE(ApplicationFactory_impl, "-- Completed placement for Collocation ID:"
                      << (*plan)->getId() << " Components Placed: " << components.size());
        } else {
            ComponentInfo* component = components[0];
            std::string assigned_device;
            DeviceAssignmentMap::const_iterator device = devices.find(component->getInstantiationIdentifier());
            if (device != devices.end()) {
                assigned_device = device->second;
                LOG_TRACE(ApplicationFactory_impl, "Component " << component->getInstantiationIdentifier()
                          << " is assigned to device " << assigned_device);
            }
            ossie::ComponentDeployment* deployment = allocateComponent(component, assigned_device, appIdentifier);
            appDeployment.addComponentDeployment(deployment);
        }
    }
}

void createHelper::_validateDAS(ossie::ApplicationPlacement& appPlacement,
                                const DeviceAssignmentMap& deviceAssignments)
{
    LOG_TRACE(ApplicationFactory_impl, "Validating device assignment sequence (length "
              << deviceAssignments.size() << ")");
    for (DeviceAssignmentMap::const_iterator ii = deviceAssignments.begin(); ii != deviceAssignments.end(); ++ii) {
        const std::string& componentId = ii->first;
        const std::string& assignedDeviceId = ii->second;
        ossie::ComponentInfo* component = appPlacement.getComponent(componentId);

        if (!component) {
            LOG_ERROR(ApplicationFactory_impl, "Failed to create application; "
                      << "unknown component " << componentId 
                      << " in user assignment (DAS)");
            CF::DeviceAssignmentSequence badDAS;
            badDAS.length(1);
            badDAS[0].componentId = componentId.c_str();
            badDAS[0].assignedDeviceId = assignedDeviceId.c_str();
            throw CF::ApplicationFactory::CreateApplicationRequestError(badDAS);
        }
    }
}

void createHelper::_resolveImplementations(PlacementList::iterator comp, PlacementList& compList, std::vector<ossie::ImplementationInfo::List> &res_vec)
{
    if (comp == compList.end()) {
        return;
    }
    const ossie::ImplementationInfo::List& comp_imps = (*comp)->getImplementations();
    std::vector<ossie::ImplementationInfo::List> tmp_res_vec = res_vec;
    unsigned int old_res_vec_size = res_vec.size();
    if (old_res_vec_size == 0) {
        res_vec.resize(comp_imps.size());
        for (unsigned int ii=0; ii<comp_imps.size(); ii++) {
            res_vec[ii].resize(1);
            res_vec[ii][0] = comp_imps[ii];
        }
    } else {
        res_vec.resize(old_res_vec_size * comp_imps.size());
        for (unsigned int i=0; i<old_res_vec_size; i++) {
            for (unsigned int ii=0; ii<comp_imps.size(); ii++) {
                unsigned int res_vec_idx = i*comp_imps.size()+ii;
                res_vec[res_vec_idx] = tmp_res_vec[i];
                res_vec[res_vec_idx].insert(res_vec[res_vec_idx].begin(),comp_imps[ii]);
            }
        }
    }
    this->_resolveImplementations(++comp, compList, res_vec);
    return;
}

void createHelper::_removeUnmatchedImplementations(std::vector<ossie::ImplementationInfo::List> &res_vec)
{
    std::vector<ossie::ImplementationInfo::List>::iterator impl_list = res_vec.begin();
    while (impl_list != res_vec.end()) {
        std::vector<ossie::ImplementationInfo::List>::iterator old_impl_list = impl_list;
        ossie::ImplementationInfo::List::iterator impl = (*impl_list).begin();
        std::vector<ossie::SPD::NameVersionPair> reference_pair = (*impl)->getOsDeps();
        std::vector<std::string> reference_procs = (*impl)->getProcessorDeps();
        bool os_init_to_zero = (reference_pair.size()==0);
        bool proc_init_to_zero = (reference_procs.size()==0);
        impl++;
        bool match = true;
        while (impl != (*impl_list).end()) {
            std::vector<ossie::SPD::NameVersionPair> pair = (*impl)->getOsDeps();
            std::vector<std::string> procs = (*impl)->getProcessorDeps();
            bool os_must_match = false;
            bool proc_must_match = false;
            if (os_init_to_zero)
                os_must_match = false;
            if (proc_init_to_zero)
                proc_must_match = false;
            if ((reference_pair.size() != 0) and (pair.size() != 0)) {os_must_match = true;}
            if ((reference_procs.size() != 0) and (procs.size() != 0)) {proc_must_match = true;}
            // if os must match (because both lists are non-zero length), check that at least one of the sets matches
            if (os_must_match) {
                bool at_least_one_match = false;
                for (std::vector<ossie::SPD::NameVersionPair>::iterator ref=reference_pair.begin(); ref<reference_pair.end(); ref++) {
                    for (std::vector<ossie::SPD::NameVersionPair>::iterator cur=pair.begin(); cur<pair.end(); cur++) {
                        if ((*ref)==(*cur)) {
                            at_least_one_match = true;
                            break;
                        }
                    }
                }
                if (!at_least_one_match) {match = false;break;}
            }
            // if proc must match (because both lists are non-zero length), check that at least one of the sets matches
            if (proc_must_match) {
                bool at_least_one_match = false;
                for (std::vector<std::string>::iterator ref=reference_procs.begin(); ref<reference_procs.end(); ref++) {
                    for (std::vector<std::string>::iterator cur=procs.begin(); cur<procs.end(); cur++) {
                        if ((*ref)==(*cur)) {
                            at_least_one_match = true;
                        }
                    }
                }
                if (!at_least_one_match) {match = false;break;}
            }
            // reduce the number of os that can be used as a reference to the overlapping set
            if (reference_pair.size()>pair.size()) {
                for (std::vector<ossie::SPD::NameVersionPair>::iterator ref=reference_pair.begin(); ref<reference_pair.end(); ref++) {
                    bool found_match = false;
                    for (std::vector<ossie::SPD::NameVersionPair>::iterator cur=pair.begin(); cur<pair.end(); cur++) {
                        if ((*ref)==(*cur)) {
                            found_match = true;
                        }
                    }
                    if (not found_match) {
                        reference_pair.erase(ref);
                    }
                }
            }
            // reduce the number of procs that can be used as a reference to the overlapping set
            if (reference_procs.size()>procs.size()) {
                for (std::vector<std::string>::iterator ref=reference_procs.begin(); ref<reference_procs.end(); ref++) {
                    bool found_match = false;
                    for (std::vector<std::string>::iterator cur=procs.begin(); cur<procs.end(); cur++) {
                        if ((*ref)==(*cur)) {
                            found_match = true;
                        }
                    }
                    if (not found_match) {
                        reference_procs.erase(ref);
                    }
                }
            }
            // if the initial entity did not have an os, add it if the current one holds an os requirement
            if (os_init_to_zero) {
                if (pair.size() != 0) {
                    os_init_to_zero = false;
                    for (std::vector<ossie::SPD::NameVersionPair>::iterator cur=pair.begin(); cur<pair.end(); cur++) {
                        reference_pair.push_back((*cur));
                    }
                }
            }
            // if the initial entity did not have a proc, add it if the current one holds a proc requirement
            if (proc_init_to_zero) {
                if (procs.size() != 0) {
                    proc_init_to_zero = false;
                    for (std::vector<std::string>::iterator cur=procs.begin(); cur<procs.end(); cur++) {
                        reference_procs.push_back((*cur));
                    }
                }
            }
            impl++;
        }
        if (not match) {
            (*impl_list).erase(impl);
        }
        impl_list++;
    }
    return;
}

void createHelper::_consolidateAllocations(const ossie::ImplementationInfo::List& impls, CF::Properties& allocs)
{
    allocs.length(0);
    for (ossie::ImplementationInfo::List::const_iterator impl= impls.begin(); impl != impls.end(); ++impl) {
        const std::vector<SPD::PropertyRef>& deps = (*impl)->getDependencyProperties();
        for (std::vector<SPD::PropertyRef>::const_iterator dep = deps.begin(); dep != deps.end(); ++dep) {
          ossie::ComponentProperty *prop = dep->property.get();
          if (dynamic_cast<const SimplePropertyRef*>( prop ) != NULL) {
                const SimplePropertyRef* dependency = dynamic_cast<const SimplePropertyRef*>(prop);
                ossie::corba::push_back(allocs, convertPropertyToDataType(dependency));
            } else if (dynamic_cast<const SimpleSequencePropertyRef*>(prop) != NULL) {
                const SimpleSequencePropertyRef* dependency = dynamic_cast<const SimpleSequencePropertyRef*>(prop);
                ossie::corba::push_back(allocs, convertPropertyToDataType(dependency));
            } else if (dynamic_cast<const ossie::StructPropertyRef*>(prop) != NULL) {
                const ossie::StructPropertyRef* dependency = dynamic_cast<const ossie::StructPropertyRef*>(prop);
                ossie::corba::push_back(allocs, convertPropertyToDataType(dependency));
            } else if (dynamic_cast<const ossie::StructSequencePropertyRef*>(prop) != NULL) {
                const ossie::StructSequencePropertyRef* dependency = dynamic_cast<const ossie::StructSequencePropertyRef*>(prop);
                ossie::corba::push_back(allocs, convertPropertyToDataType(dependency));
            }
        }
    }
}

void createHelper::_placeHostCollocation(ossie::ApplicationDeployment& appDeployment,
                                         const std::string &appIdentifier,
                                         const PlacementList& collocatedComponents,
                                         const DeviceAssignmentMap& devices)
{
    // Keep track of devices to which some of the components have
    // been assigned.
    DeviceIDList assignedDevices;
    for (PlacementList::const_iterator placement = collocatedComponents.begin();
         placement != collocatedComponents.end();
         ++placement) {
        DeviceAssignmentMap::const_iterator device = devices.find((*placement)->getInstantiationIdentifier());
        if (device != devices.end()) {
            assignedDevices.push_back(device->second);
        }
    }

    PlacementList placingComponents = collocatedComponents;

    // create every combination of implementations for the components in the set
    // for each combination:
    //  consolidate allocations
    //  attempt allocation
    //  if the allocation succeeds, break the loop
    std::vector<ossie::ImplementationInfo::List> res_vec;
    this->_resolveImplementations(placingComponents.begin(), placingComponents, res_vec);
    this->_removeUnmatchedImplementations(res_vec);

    // Get the executable devices for the domain; if there were any devices
    // assigned, filter out all other devices
    ossie::DeviceList deploymentDevices = _executableDevices;
    if (!assignedDevices.empty()) {
        for (ossie::DeviceList::iterator node = deploymentDevices.begin(); node != deploymentDevices.end(); ++node) {
            if (std::find(assignedDevices.begin(), assignedDevices.end(), (*node)->identifier) == assignedDevices.end()) {
                node = deploymentDevices.erase(node);
            }
        }
    }
    

    for (size_t index = 0; index < res_vec.size(); ++index) {
        // Merge processor and OS dependencies from all implementations
        std::vector<std::string> processorDeps = mergeProcessorDeps(res_vec[index]);
        std::vector<ossie::SPD::NameVersionPair> osDeps = mergeOsDeps(res_vec[index]);

        // Consolidate the allocation properties into a single list
        CF::Properties allocationProperties;
        this->_consolidateAllocations(res_vec[index], allocationProperties);

        const std::string requestid = ossie::generateUUID();
        ossie::AllocationResult response = this->_allocationMgr->allocateDeployment(requestid, allocationProperties, deploymentDevices, appIdentifier, processorDeps, osDeps);
        if (!response.first.empty()) {
            // Ensure that all capacities get cleaned up
            this->_allocations.push_back(response.first);

            // Convert from response back into a device node
            boost::shared_ptr<ossie::DeviceNode>& node = response.second;
            const std::string& deviceId = node->identifier;

            PlacementList::iterator comp = placingComponents.begin();
            ossie::ImplementationInfo::List::iterator impl = res_vec[index].end()-1;
            for (unsigned int i=0; i<placingComponents.size(); i++,comp++,impl--) {
                ossie::ComponentDeployment* deployment = new ossie::ComponentDeployment(*comp, *impl);
                deployment->setAssignedDevice(node);
                if (!resolveSoftpkgDependencies(deployment, *node)) {
                    LOG_TRACE(ApplicationFactory_impl, "Unable to resolve softpackage dependencies for component "
                              << (*comp)->getIdentifier() << " implementation " << (*impl)->getId());
                    delete deployment;
                    continue;
                }
                appDeployment.addComponentDeployment(deployment);
            }
            
            // Move the device to the front of the list
            rotateDeviceList(_executableDevices, deviceId);
            return;
        }
    }

    std::ostringstream eout;
    //eout << "Could not collocate components for collocation NAME: " << collocation.getName() << "  ID:" << collocation.id;
    eout << "Could not collocate components for collocation";
    LOG_ERROR(ApplicationFactory_impl, eout.str());
    throw CF::ApplicationFactory::CreateApplicationRequestError();
}

void createHelper::_handleUsesDevices(ossie::ApplicationPlacement& appPlacement,
                                      ossie::ApplicationDeployment& appDeployment,
                                      const std::string& appName)
{
    // Gets all uses device info from the SAD file
    const UsesDeviceInfo::List& usesDevices = _appInfo.getUsesDevices();
    LOG_TRACE(ApplicationFactory_impl, "Application has " << usesDevices.size() << " usesdevice dependencies");

    // Get the assembly controller's configure properties for context in the
    // allocations
    CF::Properties appProperties;
    ossie::ComponentInfo* assembly_controller = appPlacement.getAssemblyController();
    if (assembly_controller) {
        appProperties = assembly_controller->getConfigureProperties();
    }

    // The device assignments for SAD-level usesdevices are never stored
    std::vector<ossie::UsesDeviceAssignment*> assignedDevices;
    if (!allocateUsesDevices(usesDevices, appProperties, assignedDevices, this->_allocations)) {
        // There were unsatisfied usesdevices for the application
        ostringstream eout;
        eout << "Failed to satisfy 'usesdevice' dependencies ";
        bool first = true;
        for (UsesDeviceInfo::List::const_iterator uses = usesDevices.begin(); uses != usesDevices.end(); ++uses) {
            if ((*uses)->getAssignedDeviceId().empty()) {
                if (!first) {
                    eout << ", ";
                } else {
                    first = false;
                }
                eout << (*uses)->getId();
            }
        }
        eout << "for application '" << appName << "'";
        LOG_DEBUG(ApplicationFactory_impl, eout.str());
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_ENOSPC, eout.str().c_str());
    }
    for (std::vector<ossie::UsesDeviceAssignment*>::iterator dev=assignedDevices.begin(); dev!=assignedDevices.end(); dev++) {
        //dev->deviceAssignment.componentId = assembly_controller->getIdentifier().c_str();
        appDeployment.addUsesDeviceAssignment(*dev);
    }
}

void createHelper::setUpExternalPorts(ossie::ApplicationDeployment& appDeployment,
                                      Application_impl* application)
{
    typedef std::vector<SoftwareAssembly::Port> PortList;
    const PortList& ports = _appFact._sadParser.getExternalPorts();
    LOG_TRACE(ApplicationFactory_impl,
              "Mapping " << ports.size() << " external port(s)");

    for (PortList::const_iterator port = ports.begin(); port != ports.end(); ++port) {
        LOG_TRACE(ApplicationFactory_impl,
                  "Port component: " << port->componentrefid
                        << " Port identifier: " << port->identifier);

        // Get the component from the instantiation identifier.
        ossie::ComponentDeployment* deployment = appDeployment.getComponentDeployment(port->componentrefid);
        if (!deployment) {
            LOG_ERROR(ApplicationFactory_impl,
                      "Invalid componentinstantiationref ("
                            <<port->componentrefid
                            <<") given for an external port ");
            throw(CF::ApplicationFactory::CreateApplicationError(
                CF::CF_NOTSET,
                "Invalid componentinstantiationref given for external port"));
        }

        CF::Resource_var resource = deployment->getResourcePtr();
        CORBA::Object_var obj;

        if (port->type == SoftwareAssembly::Port::SUPPORTEDIDENTIFIER) {
            if (!resource->_is_a(port->identifier.c_str())) {
                LOG_ERROR(
                    ApplicationFactory_impl,
                    "Component does not support requested interface: "
                        << port->identifier);
                throw(CF::ApplicationFactory::CreateApplicationError(
                    CF::CF_NOTSET,
                    "Component does not support requested interface"));
            }
            obj = CORBA::Object::_duplicate(resource);
        } else {
            // Must be either "usesidentifier" or "providesidentifier",
            // which are equivalent unless you want to be extra
            // pedantic and check how the port is described in the
            // component's SCD.
            // Try to look up the port.
            try {
                obj = resource->getPort(port->identifier.c_str());
            } CATCH_THROW_LOG_ERROR(
                ApplicationFactory_impl,
                "Invalid port id",
                CF::ApplicationFactory::CreateApplicationError(
                    CF::CF_NOTSET,
                    "Invalid port identifier"))
        }

        // Add it to the list of external ports on the application object.
        std::string name = port->externalname;
        if (name.empty()) {
            name = port->identifier;
        }
        application->addExternalPort(name, obj);
    }
}

void createHelper::setUpExternalProperties(ossie::ApplicationDeployment& appDeployment,
                                           Application_impl* application)
{
    const std::vector<SoftwareAssembly::Property>& props = _appFact._sadParser.getExternalProperties();
    LOG_TRACE(ApplicationFactory_impl, "Mapping " << props.size() << " external property(ies)");
    for (std::vector<SoftwareAssembly::Property>::const_iterator prop = props.begin(); prop != props.end(); ++prop) {
        LOG_TRACE(ApplicationFactory_impl, "Property component: " << prop->comprefid << " Property identifier: " << prop->propid);

        // Get the component from the compref identifier.
        ossie::ComponentDeployment* deployment = appDeployment.getComponentDeployment(prop->comprefid);
        if (!deployment) {
            LOG_ERROR(ApplicationFactory_impl, "Unable to find component for comprefid " << prop->comprefid);
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_NOTSET, "Unable to find component for given comprefid");
        }
        const Property* property = deployment->getComponent()->prf.getProperty(prop->propid);
        if (!property){
            LOG_ERROR(ApplicationFactory_impl, "Attempting to promote property: '" <<
                    prop->propid << "' that does not exist in component: '" << prop->comprefid << "'");
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_NOTSET,
                    "Attempting to promote property that does not exist in component");
        }

        CF::Resource_var comp = deployment->getResourcePtr();
        std::string external_id = prop->externalpropid;
        if (external_id.empty()) {
            external_id = prop->propid;
        }
        application->addExternalProperty(prop->propid, external_id, comp);
    }
}

/* Creates and instance of the application.
 *  - Assigns components to devices
 *      - First based on user-provided DAS if one is passed in
 *        (deviceAssignments)
 *      - Then based on property matching and allocation matching
 *  - Attempts to honor host collocation
 *  @param name user-friendly name of the application to be instantiated
 *  @param initConfiguration properties that can override those from the SAD
 *  @param deviceAssignments optional user-provided component-to-device
 *         assignments
 */
CF::Application_ptr ApplicationFactory_impl::create (
    const char* name,
    const CF::Properties& initConfiguration,
    const CF::DeviceAssignmentSequence& deviceAssignments)
throw (CORBA::SystemException, CF::ApplicationFactory::CreateApplicationError,
        CF::ApplicationFactory::CreateApplicationRequestError,
        CF::ApplicationFactory::CreateApplicationInsufficientCapacityError,
        CF::ApplicationFactory::InvalidInitConfiguration)
{
    TRACE_ENTER(ApplicationFactory_impl);
    LOG_TRACE(ApplicationFactory_impl, "Creating application " << name);

    // must declare these here, so we can pass to the createHelper instance
    string _waveform_context_name;
    string base_naming_context;
    CosNaming::NamingContext_var _waveformContext;

    ///////////////////////////////////////////////////
    // Establish new naming context for waveform
    LOG_TRACE(ApplicationFactory_impl, "Establishing waveform naming context");
    try {
        // VERY IMPORTANT: we must first lock the operations in this try block
        //    in order to prevent a naming context collision due to multiple create calls
        boost::mutex::scoped_lock lock(_pendingCreateLock);

        // get new naming context name
        _waveform_context_name = getWaveformContextName(name);
        base_naming_context = getBaseWaveformContext(_waveform_context_name);

        _waveformContext = CosNaming::NamingContext::_nil();

        // create the new naming context
        CosNaming::Name WaveformContextName;
        WaveformContextName.length(1);
        WaveformContextName[0].id = _waveform_context_name.c_str();

        LOG_TRACE(ApplicationFactory_impl, "Binding new context " << _waveform_context_name.c_str());
        try {
            _waveformContext = _domainContext->bind_new_context(WaveformContextName);
        } catch( ... ) {
            // just in case it bound, unbind and error
            // roughly the same code as _cleanupNewContext
            try {
                _domainContext->unbind(WaveformContextName);
            } catch ( ... ) {
            }
            LOG_ERROR(ApplicationFactory_impl, "bind_new_context threw Unknown Exception");
            throw;
        }

    } catch(...){
    }

    // Convert the device assignments into a map for easier lookup
    std::map<std::string,std::string> deviceAssignmentMap;
    for (size_t index = 0; index < deviceAssignments.length(); ++index) {
        const std::string componentId(deviceAssignments[index].componentId);
        const std::string assignedDeviceId(deviceAssignments[index].assignedDeviceId);
        deviceAssignmentMap.insert(std::make_pair(componentId, assignedDeviceId));
    }

    // now use the createHelper class to actually run 'create'
    // - createHelper is needed to allow concurrent calls to 'create' without
    //   each instance stomping on the others
    LOG_TRACE(ApplicationFactory_impl, "Creating new createHelper class.");
    createHelper new_createhelper(*this, _waveform_context_name, base_naming_context, _waveformContext, _domainContext);

    // now actually perform the create operation
    LOG_TRACE(ApplicationFactory_impl, "Performing 'create' function.");
    CF::Application_ptr new_app = new_createhelper.create(name, initConfiguration, deviceAssignmentMap);
    // return the new Application
    TRACE_EXIT(ApplicationFactory_impl);
    return new_app;
}

CF::Application_ptr createHelper::create (
    const char*                         name,
    const CF::Properties&               initConfiguration,
    const DeviceAssignmentMap& deviceAssignments)
throw (CORBA::SystemException,
       CF::ApplicationFactory::CreateApplicationError,
       CF::ApplicationFactory::CreateApplicationRequestError,
       CF::ApplicationFactory::InvalidInitConfiguration)
{
    TRACE_ENTER(ApplicationFactory_impl);
    
    bool aware_application = true;
    
    CF::Properties modifiedInitConfiguration;

    try {
        ///////////////////////////////////////////////////////////////////
        // Check to see if this is an aware application and 
        //  check to see if a different GPP reservation setting is defined
        const std::string aware_app_property_id(ExtendedCF::WKP::AWARE_APPLICATION);
        for (unsigned int initCount = 0; initCount < initConfiguration.length(); initCount++) {
            if (std::string(initConfiguration[initCount].id) == aware_app_property_id) {
                initConfiguration[initCount].value >>= aware_application;
                modifiedInitConfiguration.length(initConfiguration.length()-1);
                for (unsigned int rem_idx=0; rem_idx<initConfiguration.length()-1; rem_idx++) {
                    unsigned int idx_mod = 0;
                    if (rem_idx == initCount)
                        idx_mod = 1;
                    modifiedInitConfiguration[rem_idx] = initConfiguration[rem_idx+idx_mod];
                    //modifiedInitConfiguration[rem_idx].id = initConfiguration[rem_idx+idx_mod].id;
                    //modifiedInitConfiguration[rem_idx].value = initConfiguration[rem_idx+idx_mod].value;
                }
            }
        }
        
        if (modifiedInitConfiguration.length() == 0) {
            modifiedInitConfiguration = initConfiguration;
        }

        const std::string specialized_reservation("SPECIALIZED_CPU_RESERVATION");
        for (unsigned int initCount = 0; initCount < modifiedInitConfiguration.length(); initCount++) {
            if (std::string(modifiedInitConfiguration[initCount].id) == specialized_reservation) {
                CF::Properties *reservations;
                if (modifiedInitConfiguration[initCount].value >>= reservations) {
                    for (unsigned int rem_idx=0; rem_idx<reservations->length(); rem_idx++) {
                        double value = 0;
                        std::string component_id((*reservations)[rem_idx].id);
                        if ((*reservations)[rem_idx].value >>= value) {
                            specialized_reservations[component_id] = value;
                        }
                    }
                } else {
                    // the value of the any is of the wrong type
                }
                for (unsigned int rem_idx=initCount; rem_idx<modifiedInitConfiguration.length()-1; rem_idx++) {
                    modifiedInitConfiguration[rem_idx] = modifiedInitConfiguration[rem_idx+1];
                }
                modifiedInitConfiguration.length(modifiedInitConfiguration.length()-1);
            }
        }

        // Get a list of all device currently in the domain
        _registeredDevices = _appFact._domainManager->getRegisteredDevices();
        _executableDevices.clear();
        for (DeviceList::iterator iter = _registeredDevices.begin(); iter != _registeredDevices.end(); ++iter) {
            if ((*iter)->isExecutable) {
                _executableDevices.push_back(*iter);
            }
        }

        // Fail immediately if there are no available devices to execute components
        if (_executableDevices.empty()) {
            const char* message = "Domain has no executable devices (GPPs) to run components";
            LOG_WARN(ApplicationFactory_impl, message);
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_ENODEV, message);
        }

        const std::string lastExecutableDevice = _appFact._domainManager->getLastDeviceUsedForDeployment();
        if (!lastExecutableDevice.empty()) {
            LOG_TRACE(ApplicationFactory_impl, "Placing device " << lastExecutableDevice
                      << " first in deployment list");
            rotateDeviceList(_executableDevices, lastExecutableDevice);
        }

        //////////////////////////////////////////////////
        // Load the components to instantiate from the SAD
        ossie::ApplicationPlacement placement;
        getRequiredComponents(placement);

        ossie::ComponentInfo* assemblyControllerComponent = placement.getAssemblyController();
        if (assemblyControllerComponent) {
            overrideProperties(modifiedInitConfiguration, assemblyControllerComponent);
        }

        //////////////////////////////////////////////////
        // Store information about this application
        _appInfo.populateApplicationInfo(_appFact._sadParser);

        overrideExternalProperties(placement, modifiedInitConfiguration);

        ////////////////////////////////////////////////
        // Assign components to devices
        ////////////////////////////////////////////////
        ossie::ApplicationDeployment app_deployment;

        // Allocate any usesdevice capacities specified in the SAD file
        _handleUsesDevices(placement, app_deployment, name);

        // Give the application a unique identifier of the form 
        // "softwareassemblyid:ApplicationName", where the application 
        // name includes the serial number generated for the naming context
        // (e.g. "Application_1").
        std::string appIdentifier = 
            _appFact._identifier + ":" + _waveformContextName;

        // Catch invalid device assignments
        _validateDAS(placement, deviceAssignments);

        // Assign all components to devices
        assignPlacementsToDevices(placement, app_deployment, appIdentifier, deviceAssignments);

        ////////////////////////////////////////////////
        // Create the Application servant
        _application = new Application_impl(appIdentifier,
                                            name, 
                                            _appFact._softwareProfile, 
                                            _appFact._domainManager, 
                                            _waveformContextName, 
                                            _waveformContext,
                                            aware_application,
                                            _domainContext);

        // Activate the new Application servant
        PortableServer::ObjectId_var oid = Application_impl::Activate(_application);

        std::vector<ConnectionNode> connections;
        std::vector<std::string> allocationIDs;

        CF::ApplicationRegistrar_var app_reg = _application->appReg();
        loadAndExecuteComponents(app_deployment.getComponentDeployments(), app_reg);
        waitForComponentRegistration(app_deployment.getComponentDeployments());
        initializeComponents(app_deployment.getComponentDeployments());

        // Check that the assembly controller is valid
        CF::Resource_var assemblyController;
        if (assemblyControllerComponent) {
            const std::string& assemblyControllerId = assemblyControllerComponent->getInstantiationIdentifier();
            ossie::ComponentDeployment* deployment = app_deployment.getComponentDeployment(assemblyControllerId);
            assemblyController = deployment->getResourcePtr();
        }
        _checkAssemblyController(assemblyController, assemblyControllerComponent);

        _connectComponents(app_deployment, connections);
        _configureComponents(app_deployment.getComponentDeployments());

        setUpExternalPorts(app_deployment, _application);
        setUpExternalProperties(app_deployment, _application);

        ////////////////////////////////////////////////
        // Create the application
        //
        // We are assuming that all components and their resources are 
        // collocated. This means that we assume the SAD <partitioning> 
        // element contains the <hostcollocation> element. NB: Ownership 
        // of the ConnectionManager is passed to the application.
        _allocations.transfer(allocationIDs);

        // Fill in the uses devices for the application
        CF::DeviceAssignmentSequence app_devices;
        typedef std::vector<ossie::UsesDeviceAssignment*> UsesList;
        const UsesList& app_uses = app_deployment.getUsesDeviceAssignments();
        for (UsesList::const_iterator uses = app_uses.begin(); uses != app_uses.end(); ++uses) {
            CF::DeviceAssignmentType assignment;
            assignment.componentId = CORBA::string_dup(name);
            std::string deviceId;
            try {
                deviceId = ossie::corba::returnString((*uses)->getAssignedDevice()->identifier());
            } catch (...) {
            }
            assignment.assignedDeviceId = deviceId.c_str();
            ossie::corba::push_back(app_devices, assignment);
        }

        const DeploymentList& deployments = app_deployment.getComponentDeployments();
        for (DeploymentList::const_iterator dep = deployments.begin(); dep != deployments.end(); ++dep) {
            ossie::ComponentInfo* component = (*dep)->getComponent();
            CF::DeviceAssignmentType comp_assignment;
            comp_assignment.componentId = component->getIdentifier().c_str();
            comp_assignment.assignedDeviceId = (*dep)->getAssignedDevice()->identifier.c_str();
            ossie::corba::push_back(app_devices, comp_assignment);

            const UsesList& dep_uses = (*dep)->getUsesDeviceAssignments();
            for (UsesList::const_iterator uses = dep_uses.begin(); uses != dep_uses.end(); ++uses) {
                CF::DeviceAssignmentType assignment;
                assignment.componentId = component->getIdentifier().c_str();
                std::string deviceId;
                try {
                    deviceId = ossie::corba::returnString((*uses)->getAssignedDevice()->identifier());
                } catch (...) {
                }
                assignment.assignedDeviceId = deviceId.c_str();
                ossie::corba::push_back(app_devices, assignment);
            }
        }

        std::vector<CF::Resource_var> start_order = getStartOrder(app_deployment.getComponentDeployments());
        _application->populateApplication(
            assemblyController,
            app_devices, 
            start_order, 
            connections, 
            allocationIDs);

        // Add a reference to the new application to the 
        // ApplicationSequence in DomainManager
        CF::Application_var appObj = _application->_this();
        try {
            _appFact._domainManager->addApplication(_application);
        } catch (CF::DomainManager::ApplicationInstallationError& ex) {
            // something bad happened - clean up
            LOG_ERROR(ApplicationFactory_impl, ex.msg);
            throw CF::ApplicationFactory::CreateApplicationError(ex.errorNumber, ex.msg);
        }

        // After all components have been deployed, we know that the first
        // executable device in the list was used for the last deployment,
        // so update the domain manager
        _appFact._domainManager->setLastDeviceUsedForDeployment(_executableDevices.front()->identifier);

        if ( _appFact._domainManager ) {
          _appFact._domainManager->sendAddEvent( _appFact._identifier.c_str(), 
                                                 appIdentifier.c_str(), 
                                                 name,
                                                 appObj,
                                                 StandardEvent::APPLICATION);
        }

        LOG_INFO(ApplicationFactory_impl, "Done creating application " << appIdentifier << " " << name);
        _isComplete = true;
        return appObj._retn();
    } catch (CF::ApplicationFactory::CreateApplicationError& ex) {
        LOG_ERROR(ApplicationFactory_impl, "Error in application creation; " << ex.msg);
        throw;
    } catch (CF::ApplicationFactory::CreateApplicationRequestError& ex) {
        LOG_ERROR(ApplicationFactory_impl, "Error in application creation")
        throw;
    } catch ( std::exception& ex ) {
        ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<" while creating the application";
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw (CF::ApplicationFactory::CreateApplicationError(CF::CF_EBADF, eout.str().c_str()));
    } catch ( const CORBA::Exception& ex ) {
        ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<" while creating the application";
        LOG_ERROR(ApplicationFactory_impl, eout.str())
        throw (CF::ApplicationFactory::CreateApplicationError(CF::CF_NOTSET, eout.str().c_str()));
    } catch ( ... ) {
        LOG_ERROR(ApplicationFactory_impl, "Unexpected error in application creation - see log")
        throw (CF::ApplicationFactory::CreateApplicationError(CF::CF_NOTSET, "Unexpected error in application creation - see log."));
    }

}

void createHelper::overrideExternalProperties(ossie::ApplicationPlacement& appPlacement,
                                              const CF::Properties& initConfiguration)
{
    const std::vector<SoftwareAssembly::Property>& props = _appFact._sadParser.getExternalProperties();

    for (unsigned int i = 0; i < initConfiguration.length(); ++i) {
        for (std::vector<SoftwareAssembly::Property>::const_iterator prop = props.begin(); prop != props.end(); ++prop) {
            std::string id;
            if (prop->externalpropid == "") {
                id = prop->propid;
            } else {
                id = prop->externalpropid;
            }

            if (id == static_cast<const char*>(initConfiguration[i].id)) {
                ComponentInfo *comp = appPlacement.getComponent(prop->comprefid);
                // Only configure on non AC components
                if (comp != 0 && !comp->isAssemblyController()) {
                    comp->overrideProperty(prop->propid.c_str(), initConfiguration[i].value);
                }
            }
        }
    }
}

void createHelper::overrideProperties(const CF::Properties& initConfiguration,
                                      ossie::ComponentInfo* component) {
    // Override properties
    for (unsigned int initCount = 0; initCount < initConfiguration.length(); initCount++) {
        const std::string init_id(initConfiguration[initCount].id);
        if (init_id == "LOGGING_CONFIG_URI"){
            // See if the LOGGING_CONFIG_URI has already been set
            // via <componentproperties> or initParams
            bool alreadyHasLoggingConfigURI = false;
            CF::Properties execParameters = component->getExecParameters();
            for (unsigned int i = 0; i < execParameters.length(); ++i) {
                const std::string propid(execParameters[i].id);
                if (propid == "LOGGING_CONFIG_URI") {
                    alreadyHasLoggingConfigURI = true;
                    break;
                }
            }
            // If LOGGING_CONFIG_URI isn't already an exec param, add it
            // Otherwise, don't override component exec param value 
            if (!alreadyHasLoggingConfigURI) {
                // Add LOGGING_CONFIG_URI as an exec param now so that it can be set to the overridden value
                CF::DataType lcuri = initConfiguration[initCount];
                component->addExecParameter(lcuri);
                LOG_TRACE(ApplicationFactory_impl, "Adding LOGGING_CONFIG_URI as exec param with value "
                      << ossie::any_to_string(lcuri.value));
            }
        } else {
            LOG_TRACE(ApplicationFactory_impl, "Overriding property " << init_id
                      << " with " << ossie::any_to_string(initConfiguration[initCount].value));
            component->overrideProperty(init_id.c_str(), initConfiguration[initCount].value);
        }
    }
}

CF::AllocationManager::AllocationResponseSequence* createHelper::allocateUsesDeviceProperties(const UsesDeviceInfo::List& usesDevices, const CF::Properties& configureProperties)
{
    CF::AllocationManager::AllocationRequestSequence request;
    request.length(usesDevices.size());
    
    for (unsigned int usesdev_idx=0; usesdev_idx< usesDevices.size(); usesdev_idx++) {
        const std::string requestid = usesDevices[usesdev_idx]->getId();
        request[usesdev_idx].requestID = requestid.c_str();

        // Get the usesdevice dependency properties, first from the SPD...
        CF::Properties& allocationProperties = request[usesdev_idx].allocationProperties;
        const std::vector<SPD::PropertyRef>&prop_refs = usesDevices[usesdev_idx]->getProperties();
        this->_castRequestProperties(allocationProperties, prop_refs);
        
        // ...then from the SAD; in practice, these are mutually exclusive, but
        // there is no harm in doing both, as one set will always be empty
        const std::vector<SoftwareAssembly::PropertyRef>& sad_refs = usesDevices[usesdev_idx]->getSadDeps();
        this->_castRequestProperties(allocationProperties, sad_refs, allocationProperties.length());
        
        this->_evaluateMATHinRequest(allocationProperties, configureProperties);
    }
    
    return this->_allocationMgr->allocate(request);
}
                                                          
/* Check all allocation dependencies for a particular component and assign it to a device.
 *  - Check component's overall usesdevice dependencies
 *  - Allocate capacity on usesdevice(s)
 *  - Find and implementation that has it's implementation-specific usesdevice dependencies satisfied
 *  - Allocate the component to a particular device

 Current implementation takes advantage of single failure then clean up everything..... To support collocation
 allocation failover for mulitple devices, then we need to clean up only the allocations that we made during a failed
 collocation request.  This requires that we know and cleanup only those allocations that we made..

 */
ossie::ComponentDeployment* createHelper::allocateComponent(ossie::ComponentInfo* component,
                                                            const std::string& assignedDeviceId,
                                                            const std::string& appIdentifier)
{
    CF::Properties configureProperties = component->getConfigureProperties();
    const CF::Properties &construct_props = component->getConstructProperties();
    unsigned int configlen = configureProperties.length();
    configureProperties.length(configureProperties.length()+construct_props.length());
    for (unsigned int i=0; i<construct_props.length(); i++) {
      configureProperties[i+configlen] = construct_props[i];
    }
    
    // Find the devices that allocate the SPD's minimum required usesdevices properties
    const UsesDeviceInfo::List &usesDevVec = component->getUsesDevices();
    std::vector<ossie::UsesDeviceAssignment*> assignedDevices;
    if (!allocateUsesDevices(usesDevVec, configureProperties, assignedDevices, this->_allocations)) {
        // There were unsatisfied usesdevices for the component
        ostringstream eout;
        eout << "Failed to satisfy 'usesdevice' dependencies ";
        bool first = true;
        for (UsesDeviceInfo::List::const_iterator uses = usesDevVec.begin(); uses != usesDevVec.end(); ++uses) {
            if ((*uses)->getAssignedDeviceId().empty()) {
                if (!first) {
                    eout << ", ";
                } else {
                    first = false;
                }
                eout << (*uses)->getId();
            }
        }
        eout << "for component '" << component->getIdentifier() << "'";
        LOG_DEBUG(ApplicationFactory_impl, eout.str());
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_ENOSPC, eout.str().c_str());
    }

    // now attempt to find an implementation that can have it's allocation requirements met
    const ossie::ImplementationInfo::List& implementations = component->getImplementations();
    for (size_t implCount = 0; implCount < implementations.size(); implCount++) {
        ossie::ImplementationInfo* impl = implementations[implCount];

        // Handle 'usesdevice' dependencies for the particular implementation
        std::vector<ossie::UsesDeviceAssignment*> implAssignedDevices;
        ScopedAllocations implAllocations(*this->_allocationMgr);
        const UsesDeviceInfo::List &implUsesDevVec = impl->getUsesDevices();
        
        if (!allocateUsesDevices(implUsesDevVec, configureProperties, implAssignedDevices, implAllocations)) {
            LOG_DEBUG(ApplicationFactory_impl, "Unable to satisfy 'usesdevice' dependencies for component "
                      << component->getIdentifier() << " implementation " << impl->getId());
            continue;
        }

        std::auto_ptr<ossie::ComponentDeployment> deployment(new ossie::ComponentDeployment(component, impl));
        for (std::vector<ossie::UsesDeviceAssignment*>::iterator ii = assignedDevices.begin();
             ii != assignedDevices.end(); ++ii) {
            deployment->addUsesDeviceAssignment(*ii);
        }
        
        // Found an implementation which has its 'usesdevice' dependencies
        // satisfied, now perform assignment/allocation of component to device
        LOG_DEBUG(ApplicationFactory_impl, "Trying to find the device");
        ossie::AllocationResult response = allocateComponentToDevice(deployment.get(), assignedDeviceId, appIdentifier);
        
        if (response.first.empty()) {
            LOG_DEBUG(ApplicationFactory_impl, "Unable to allocate device for component "
                      << component->getIdentifier() << " implementation " << impl->getId());
            continue;
        }
        
        // Track successful deployment allocation
        implAllocations.push_back(response.first);
        
        // Convert from response back into a device node
        deployment->setAssignedDevice(response.second);
        DeviceNode& node = *(response.second);
        const std::string& deviceId = node.identifier;
        
        if (!resolveSoftpkgDependencies(deployment.get(), node)) {
            LOG_DEBUG(ApplicationFactory_impl, "Unable to resolve softpackage dependencies for component "
                      << component->getIdentifier() << " implementation " << impl->getId());
            continue;
        }
        
        // Allocation to a device succeeded
        LOG_DEBUG(ApplicationFactory_impl, "Assigned component " << component->getInstantiationIdentifier()
                  << " implementation " << impl->getId() << " to device " << deviceId);

        // Move the device to the front of the list
        rotateDeviceList(_executableDevices, deviceId);
        
        // Store the implementation-specific usesdevice allocations and
        // device assignments
        implAllocations.transfer(this->_allocations);

        for (std::vector<ossie::UsesDeviceAssignment*>::iterator ii = implAssignedDevices.begin();
             ii != implAssignedDevices.end(); ++ii) {
            deployment->addUsesDeviceAssignment(*ii);
        }
        
        return deployment.release();
    }

    bool allBusy = true;
    for (ossie::DeviceList::iterator dev = _executableDevices.begin(); dev != _executableDevices.end(); ++dev) {
        CF::Device::UsageType state;
        try {
            state = (*dev)->device->usageState();
        } catch (...) {
            LOG_WARN(ApplicationFactory_impl, "Device " << (*dev)->identifier << " is not reachable");
            continue;
        }
        if (state != CF::Device::BUSY) {
            allBusy = false;
            break;
        }
    }
    if (allBusy) {
        // Report failure
        std::ostringstream eout;
        eout << "Unable to launch component '"<<component->getName()<<"'. All executable devices (i.e.: GPP) in the Domain are busy";
        LOG_DEBUG(ApplicationFactory_impl, eout.str());
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_ENOSPC, eout.str().c_str());
    }

    // Report failure
    std::ostringstream eout;
    eout << "Failed to satisfy device dependencies for component: '";
    eout << component->getName() << "' with component id: '" << component->getIdentifier() << "'";
    LOG_DEBUG(ApplicationFactory_impl, eout.str());
    throw CF::ApplicationFactory::CreateApplicationError(CF::CF_ENOSPC, eout.str().c_str());
}

bool createHelper::allocateUsesDevices(const ossie::UsesDeviceInfo::List& usesDevices,
                                       const CF::Properties& configureProperties,
                                       std::vector<ossie::UsesDeviceAssignment*>& deviceAssignments,
                                       ScopedAllocations& allocations)
{
    // Create a temporary lookup table for reconciling allocation requests with
    // usesdevice identifiers
    typedef std::map<std::string,UsesDeviceInfo*> UsesDeviceMap;
    UsesDeviceMap usesDeviceMap;
    for (UsesDeviceInfo::List::const_iterator iter = usesDevices.begin(); iter != usesDevices.end(); ++iter) {
        // Ensure that no devices are assigned to start; the caller can check
        // for unassigned devices to report which usesdevices failed
        (*iter)->clearAssignedDeviceId();
        usesDeviceMap[(*iter)->getId()] = *iter;
    }
    
    // Track allocations made internally, either to clean up on failure or to
    // pass to the caller
    ScopedAllocations localAllocations(*_allocationMgr);
    
    CF::AllocationManager::AllocationResponseSequence_var response = allocateUsesDeviceProperties(usesDevices, configureProperties);
    for (unsigned int resp = 0; resp < response->length(); resp++) {
        // Ensure that this allocation is recorded so that it can be cleaned up
        const std::string allocationId(response[resp].allocationID);
        LOG_TRACE(ApplicationFactory_impl, "Allocated " << allocationId);
        localAllocations.push_back(allocationId);
        
        // Find the usesdevice that matches the request and update it, removing
        // the key from the map
        const std::string requestID(response[resp].requestID);
        UsesDeviceMap::iterator uses = usesDeviceMap.find(requestID);
        if (uses == usesDeviceMap.end()) {
            // This condition should never occur
            LOG_WARN(ApplicationFactory_impl, "Allocation request " << requestID
                     << " does not match any usesdevice");
            continue;
        }
        const std::string deviceId = ossie::corba::returnString(response[resp].allocatedDevice->identifier());
        uses->second->setAssignedDeviceId(deviceId);
        usesDeviceMap.erase(uses);

        ossie::UsesDeviceAssignment* assignment = new ossie::UsesDeviceAssignment(uses->second);
        assignment->setAssignedDevice(response[resp].allocatedDevice);
        deviceAssignments.push_back(assignment);
    }
    
    if (usesDeviceMap.empty()) {
        // All usesdevices were satisfied; give the caller ownership of all the
        // allocations
        localAllocations.transfer(allocations);
        return true;
    } else {
        // Some usesdevices were not satisfied--these will have no assigned
        // device id; successful allocations will be deallocated when the
        // ScopedAllocations goes out of scope
        return false;
    }
}

void createHelper::_evaluateMATHinRequest(CF::Properties &request, const CF::Properties &configureProperties)
{
    for (unsigned int math_prop=0; math_prop<request.length(); math_prop++) {
        CF::Properties *tmp_prop;
        if (request[math_prop].value >>= tmp_prop) {
            this->_evaluateMATHinRequest(*tmp_prop, configureProperties);
            request[math_prop].value <<= *tmp_prop;
            continue;
        }
        std::string value = ossie::any_to_string(request[math_prop].value);
        if (value.find("__MATH__") != string::npos) {
            // Turn propvalue into a string for easy parsing
            std::string mathStatement = value.substr(8);
            if ((*mathStatement.begin() == '(') && (*mathStatement.rbegin() == ')')) {
                mathStatement.erase(mathStatement.begin(), mathStatement.begin() + 1);
                mathStatement.erase(mathStatement.end() - 1, mathStatement.end());
                std::vector<std::string> args;
                while ((mathStatement.length() > 0) && (mathStatement.find(',') != std::string::npos)) {
                    LOG_TRACE(ApplicationFactory_impl, "__MATH__ ARG: " << mathStatement.substr(0, mathStatement.find(',')) );
                    args.push_back(mathStatement.substr(0, mathStatement.find(',')));
                    mathStatement.erase(0, mathStatement.find(',') + 1);
                }
                args.push_back(mathStatement);

                if (args.size() != 3) {
                    std::ostringstream eout;
                    eout << " invalid __MATH__ statement; '" << mathStatement << "'";
                    throw ossie::PropertyMatchingError(eout.str());
                }

                double operand = strtod(args[0].c_str(), NULL);
                if (args[0].size() == 0) {
                    std::ostringstream eout;
                    eout << " invalid __MATH__ argument (argument empty);";
                    throw ossie::PropertyMatchingError(eout.str());
                }
                if (not std::isdigit(args[0][0])) { // if the first character is not numeric, then cannot apply __MATH__
                    std::ostringstream eout;
                    eout << " invalid __MATH__ argument; '" << args[0] << "'";
                    if (args[0][0] != '.') {
                        throw ossie::PropertyMatchingError(eout.str());
                    }
                    if (args[0].size() == 1) { // the string is only '.'
                        throw ossie::PropertyMatchingError(eout.str());
                    }
                    if (not std::isdigit(args[0][1])) { // the string starts with '.' but is not followed by a number
                        throw ossie::PropertyMatchingError(eout.str());
                    }
                }

                // See if there is a property in the component
                const CF::DataType* matchingCompProp = 0;
                for (unsigned int j = 0; j < configureProperties.length(); j++) {
                    if (strcmp(configureProperties[j].id, args[1].c_str()) == 0) {
                        matchingCompProp = &configureProperties[j];
                    }
                }

                CF::Properties *tmp_prop;
                if (matchingCompProp == 0) {
                    // see if it's in a struct
                    for (unsigned int j = 0; j < configureProperties.length(); j++) {
                        if (configureProperties[j].value >>= tmp_prop) {
                            for (unsigned int jj = 0; jj < (*tmp_prop).length(); jj++) {
                                if (strcmp((*tmp_prop)[jj].id, args[1].c_str()) == 0) {
                                    matchingCompProp = &(*tmp_prop)[jj];
                                    break;
                                }
                            }
                        }
                        if (matchingCompProp != 0)
                            break;
                    }
                }

                if (matchingCompProp == 0) {
                    std::ostringstream eout;
                    eout << " failed to match component property in __MATH__ statement; property id = " << args[1] << " does not exist in component as a configure property";
                    throw ossie::PropertyMatchingError(eout.str());
                }

                std::string math = args[2];
                CORBA::Any compValue = matchingCompProp->value;
                CORBA::TypeCode_var matchingCompPropType = matchingCompProp->value.type();
                request[math_prop].value = ossie::calculateDynamicProp(operand, compValue, math, matchingCompPropType->kind());
                std::string retval = ossie::any_to_string(request[math_prop].value);
                LOG_DEBUG(ApplicationFactory_impl, "__MATH__ RESULT: " << retval << " op1: " << operand << " op2:" << ossie::any_to_string(compValue) );
            } else {
                std::ostringstream eout;
                eout << " invalid __MATH__ statement; '" << mathStatement << "'";
                throw ossie::PropertyMatchingError(eout.str());
            }
        }
    }
}

/* Perform allocation/assignment of a particular component to the device.
 *  - First do allocation/assignment based on user provided DAS
 *  - If not specified in DAS, then iterate through devices looking for a device that satisfies
 *    the allocation properties
 */
ossie::AllocationResult createHelper::allocateComponentToDevice(ossie::ComponentDeployment* deployment,
                                              const std::string& assignedDeviceId,
                                              const std::string& appIdentifier)
{
    const ossie::ComponentInfo* component = deployment->getComponent();
    const ossie::ImplementationInfo* implementation = deployment->getImplementation();
    ossie::DeviceList devices = _registeredDevices;

    // First check to see if the component was assigned in the user provided DAS
    // See if a device was assigned in the DAS
    if (!assignedDeviceId.empty()) {
        LOG_TRACE(ApplicationFactory_impl, "User-provided DAS: Component: '" << component->getName() <<
                  "'  Assigned device: '" << assignedDeviceId << "'");
        ossie::DeviceList::iterator device;
        for (device = devices.begin(); device != devices.end(); ++device) {
            if (assignedDeviceId == (*device)->identifier) {
                break;
            }
        }

        if (device == devices.end()) {
            LOG_DEBUG(ApplicationFactory_impl, "DAS specified unknown device " << assignedDeviceId <<
                      " for component " << component->getIdentifier());
            CF::DeviceAssignmentSequence badDAS;
            badDAS.length(1);
            badDAS[0].componentId = component->getIdentifier().c_str();
            badDAS[0].assignedDeviceId = assignedDeviceId.c_str();
            throw CF::ApplicationFactory::CreateApplicationRequestError(badDAS);
        }

        // Remove all non-requested devices
        devices.erase(devices.begin(), device++);
        devices.erase(device, devices.end());
    }

    const std::string requestid = ossie::generateUUID();
    std::vector<SPD::PropertyRef> prop_refs = implementation->getDependencyProperties();
    redhawk::PropertyMap allocationProperties;
    this->_castRequestProperties(allocationProperties, prop_refs);

    CF::Properties configure_props = component->getConfigureProperties();
    CF::Properties construct_props = component->getConstructProperties();
    unsigned int initial_length = configure_props.length();
    configure_props.length(configure_props.length()+construct_props.length());
    for (unsigned int i=0; i<construct_props.length(); i++) {
        configure_props[i+initial_length] = construct_props[i];
    }
    this->_evaluateMATHinRequest(allocationProperties, configure_props);
    
    LOG_TRACE(ApplicationFactory_impl, "alloc prop size " << allocationProperties.size() );
    redhawk::PropertyMap::iterator iter=allocationProperties.begin();
    for( ; iter != allocationProperties.end(); iter++){
      LOG_TRACE(ApplicationFactory_impl, "alloc prop: " << iter->id  <<" value:" <<  ossie::any_to_string(iter->value) );
    }
    
    redhawk::PropertyMap::iterator nic_alloc = allocationProperties.find("nic_allocation");
    std::string alloc_id;
    if (nic_alloc != allocationProperties.end()) {
        redhawk::PropertyMap& substr = nic_alloc->getValue().asProperties();
        alloc_id = substr["nic_allocation::identifier"].toString();
        if (alloc_id.empty()) {
          alloc_id = ossie::generateUUID();
          substr["nic_allocation::identifier"] = alloc_id;
        }
    }
    
    ossie::AllocationResult response = this->_allocationMgr->allocateDeployment(requestid, allocationProperties, devices, appIdentifier, implementation->getProcessorDeps(), implementation->getOsDeps());
    if (allocationProperties.contains("nic_allocation")) {
        if (!response.first.empty()) {
            redhawk::PropertyMap query_props;
            query_props["nic_allocation_status"] = redhawk::Value();
            response.second->device->query(query_props);
            redhawk::ValueSequence& retstruct = query_props["nic_allocation_status"].asSequence();
            for (redhawk::ValueSequence::iterator it = retstruct.begin(); it!=retstruct.end(); it++) {
                redhawk::PropertyMap& struct_prop = it->asProperties();
                std::string identifier = struct_prop["nic_allocation_status::identifier"].toString();
                if (identifier == alloc_id) {
                    const std::string interface = struct_prop["nic_allocation_status::interface"].toString();
                    LOG_DEBUG(ApplicationFactory_impl, "Allocation NIC assignment: " << interface );
                    deployment->setNicAssignment(interface);

                    // RESOLVE - need SAD file directive to control this behavior.. i.e if promote_nic_to_affinity==true...
                    // for now add nic assignment as application affinity to all components deployed by this device
                    _app_affinity = deployment->getAffinityOptionsWithAssignment();
                }
            }
        }
    }
    TRACE_EXIT(ApplicationFactory_impl);
    return response;
}

void createHelper::_castRequestProperties(CF::Properties& allocationProperties, const std::vector<ossie::SPD::PropertyRef> &prop_refs, unsigned int offset)
{
    allocationProperties.length(offset+prop_refs.size());
    for (unsigned int i=0; i<prop_refs.size(); i++) {
        allocationProperties[offset+i] = ossie::convertPropertyRefToDataType(prop_refs[i].property.get());
    }
}

void createHelper::_castRequestProperties(CF::Properties& allocationProperties, const std::vector<ossie::SoftwareAssembly::PropertyRef> &prop_refs, unsigned int offset)
{
    allocationProperties.length(offset+prop_refs.size());
    for (unsigned int i=0; i<prop_refs.size(); i++) {
        allocationProperties[offset+i] = ossie::convertPropertyRefToDataType(prop_refs[i].property.get());
    }
}

bool createHelper::resolveSoftpkgDependencies(ossie::SoftpkgDeployment* deployment, ossie::DeviceNode& device)
{
    const ossie::ImplementationInfo* implementation = deployment->getImplementation();
    const std::vector<ossie::SoftpkgInfo*>& tmpSoftpkg = implementation->getSoftPkgDependency();
    std::vector<ossie::SoftpkgInfo*>::const_iterator iterSoftpkg;

    for (iterSoftpkg = tmpSoftpkg.begin(); iterSoftpkg != tmpSoftpkg.end(); ++iterSoftpkg) {
        // Find an implementation whose dependencies match
        ossie::SoftpkgDeployment* dependency = resolveDependencyImplementation(*iterSoftpkg, device);
        if (dependency) {
            deployment->addDependency(dependency);
        } else {
            LOG_DEBUG(ApplicationFactory_impl, "resolveSoftpkgDependencies: implementation match not found between soft package dependency and device");
            return false;
        }
    }

    return true;
}

ossie::SoftpkgDeployment* createHelper::resolveDependencyImplementation(ossie::SoftpkgInfo* softpkg,
                                                                        ossie::DeviceNode& device)
{
    const ossie::ImplementationInfo::List& spd_list = softpkg->getImplementations();

    for (size_t implCount = 0; implCount < spd_list.size(); implCount++) {
        ossie::ImplementationInfo* implementation = spd_list[implCount];
        // Check that this implementation can run on the device
        if (!implementation->checkProcessorAndOs(device.prf)) {
            continue;
        }

        ossie::SoftpkgDeployment* dependency = new ossie::SoftpkgDeployment(softpkg, implementation);
        // Recursively check any softpkg dependencies
        if (resolveSoftpkgDependencies(dependency, device)) {
            return dependency;
        }
        delete dependency;
    }

    return 0;
}

ossie::ComponentInfo* createHelper::buildComponentInfo(const ComponentPlacement& component)
{
    // Extract required data from SPD file
    ossie::ComponentInfo* newComponent = 0;
    LOG_TRACE(ApplicationFactory_impl, "Getting the SPD Filename");
    const char *spdFileName = _appFact._sadParser.getSPDById(component.getFileRefId());
    if (spdFileName == NULL) {
        ostringstream eout;
        eout << "The SPD file reference for componentfile "<<component.getFileRefId()<<" is missing";
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, eout.str().c_str());
    }
    LOG_TRACE(ApplicationFactory_impl, "Building Component Info From SPD File");
    newComponent = ossie::ComponentInfo::buildComponentInfoFromSPDFile(_appFact._fileMgr, spdFileName);
    if (newComponent == 0) {
        ostringstream eout;
        eout << "Error loading component information for file ref " << component.getFileRefId();
        LOG_ERROR(ApplicationFactory_impl, eout.str());
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, eout.str().c_str());
    }

    LOG_TRACE(ApplicationFactory_impl, "Done building Component Info From SPD File")
    // Even though it is possible for there to be more than one instantiation per component,
    //  the tooling doesn't support that, so supporting this at a framework level would add
    //  substantial complexity without providing any appreciable improvements. It is far
    //  easier to have multiple placements rather than multiple instantiations.
    const vector<ComponentInstantiation>& instantiations = component.getInstantiations();

    const ComponentInstantiation& instance = instantiations[0];

    ostringstream identifier;
    identifier << instance.getID();
    // Violate SR:172, we use the uniquified name rather than the passed in name
    identifier << ":" << _waveformContextName;
    newComponent->setIdentifier(identifier.str().c_str(), instance.getID());

    newComponent->setNamingService(instance.isNamingService());

    if (newComponent->isNamingService()) {
        ostringstream nameBinding;
        nameBinding << instance.getFindByNamingServiceName();
#if UNIQUIFY_NAME_BINDING
// DON'T USE THIS YET AS IT WILL BREAK OTHER PARTS OF REDHAWK
        nameBinding << "_" << i;  // Add a _UniqueIdentifier, per SR:169
#endif
        newComponent->setNamingServiceName(nameBinding.str().c_str());  // SR:169
    } else {
        if (newComponent->isScaCompliant()) {
            LOG_WARN(ApplicationFactory_impl, "component instantiation is sca compliant but does not provide a 'findcomponent' name...this is probably an error")
        }
    }

    newComponent->setUsageName(instance.getUsageName());
    newComponent->setAffinity( instance.getAffinity() );
    newComponent->setLoggingConfig( instance.getLoggingConfig() );

    if (strlen(instance.getStartOrder()) > 0) {
        int start_order = atoi(instance.getStartOrder());
        newComponent->setStartOrder(start_order);
    }

    const ossie::ComponentPropertyList & ins_prop = instance.getProperties();

    int docker_image_idx = -1;
    for (unsigned int i = 0; i < ins_prop.size(); ++i) {
        if (ins_prop[i]._id == "__DOCKER_IMAGE__") {
            docker_image_idx = i;
            continue;
        }
        newComponent->overrideProperty(&ins_prop[i]);
    }

    if (docker_image_idx > -1) {
        CF::Properties tmp;
        redhawk::PropertyMap& tmpProp = redhawk::PropertyMap::cast(tmp);
        tmpProp["__DOCKER_IMAGE__"].setValue(dynamic_cast<const SimplePropertyRef &>(ins_prop[docker_image_idx]).getValue());
        newComponent->addExecParameter(tmpProp[0]);
    }

    return newComponent;
}

/* Create a vector of all the components for the SAD associated with this App Factory
 *  - Get component information from the SAD and store in _requiredComponents vector
 */
void createHelper::getRequiredComponents(ossie::ApplicationPlacement& appPlacement)
{
    TRACE_ENTER(ApplicationFactory_impl);

    const std::string assemblyControllerRefId = _appFact._sadParser.getAssemblyControllerRefId();

    // Walk through the host collocations first
    const std::vector<SoftwareAssembly::HostCollocation>& collocations = _appFact._sadParser.getHostCollocations();
    for (size_t index = 0; index < collocations.size(); ++index) {
        const SoftwareAssembly::HostCollocation& collocation = collocations[index];
        LOG_TRACE(ApplicationFactory_impl, "Building component info for host collocation "
                  << collocation.getID());
        ossie::PlacementPlan* plan = new ossie::PlacementPlan(collocation.getID(), collocation.getName());
        appPlacement.addPlacement(plan);

        const std::vector<ComponentPlacement>& placements = collocations[index].getComponents();
        for (unsigned int i = 0; i < placements.size(); i++) {
            ossie::ComponentInfo* component = buildComponentInfo(placements[i]);
            if (component->getInstantiationIdentifier() == assemblyControllerRefId) {
                component->setIsAssemblyController(true);
            }
            plan->addComponent(component);
        }
    }

    // Then, walk through the remaining non-collocated components
    const std::vector<ComponentPlacement>& componentsFromSAD = _appFact._sadParser.getComponentPlacements();
    for (unsigned int i = 0; i < componentsFromSAD.size(); i++) {
        ossie::PlacementPlan* plan = new ossie::PlacementPlan();
        appPlacement.addPlacement(plan);
        ossie::ComponentInfo* component = buildComponentInfo(componentsFromSAD[i]);
        if (component->getInstantiationIdentifier() == assemblyControllerRefId) {
            component->setIsAssemblyController(true);
        }
        plan->addComponent(component);
    }

    TRACE_EXIT(ApplicationFactory_impl);
}

/* Given a waveform/application name, return a unique waveform naming context
 *  - Returns a unique waveform naming context
 *  THIS FUNCTION IS NOT THREAD SAFE
 */
string ApplicationFactory_impl::getWaveformContextName(string name )
{
    //
    // Find a new unique waveform naming for the naming context
    //


    bool found_empty = false;
    string waveform_context_name;

    // iterate through N for waveformname_N until a unique naming context if found
    CosNaming::NamingContext_ptr inc = ossie::corba::InitialNamingContext();
    do {
        ++_lastWaveformUniqueId;
        // Never use 0
        if (_lastWaveformUniqueId == 0) ++_lastWaveformUniqueId;
        waveform_context_name = "";
        waveform_context_name.append(name);
        std::string mod_waveform_context_name = waveform_context_name;
        for (int i=mod_waveform_context_name.size()-1;i>=0;i--) {
            if (mod_waveform_context_name[i]=='.') {
                mod_waveform_context_name.insert(i, 1, '\\');
            }
        }
        waveform_context_name.append("_");
        mod_waveform_context_name.append("_");
        ostringstream number_str;
        number_str << _lastWaveformUniqueId;
        waveform_context_name.append(number_str.str());
        mod_waveform_context_name.append(number_str.str());
        string temp_waveform_context(_domainName + string("/"));
        temp_waveform_context.append(mod_waveform_context_name);
        CosNaming::Name_var cosName = ossie::corba::stringToName(temp_waveform_context);
        try {
            CORBA::Object_var obj_WaveformContext = inc->resolve(cosName);
        } catch (const CosNaming::NamingContext::NotFound&) {
            found_empty = true;
        }
    } while (!found_empty);

    return waveform_context_name;

}

/* Given a waveform/application-specific context, return the full waveform naming context
 *  - Returns a full context path for the waveform
 */
string ApplicationFactory_impl::getBaseWaveformContext(string waveform_context)
{
    string base_naming_context(_domainName + string("/"));
    base_naming_context.append(waveform_context);

    return base_naming_context;
}

void createHelper::loadDependencies(const ossie::ComponentInfo& component,
                                    CF::LoadableDevice_ptr device,
                                    const std::vector<ossie::SoftpkgDeployment*>& dependencies)
{
    for (std::vector<SoftpkgDeployment*>::const_iterator deployment = dependencies.begin(); deployment != dependencies.end(); ++deployment) {
        ossie::SoftpkgInfo* dep = (*deployment)->getSoftpkg();
        const ossie::ImplementationInfo* implementation = (*deployment)->getImplementation();
        if (!implementation) {
            LOG_ERROR(ApplicationFactory_impl, "No implementation selected for dependency " << dep->getName());
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, "Missing implementation");
        }

        // Recursively load dependencies
        LOG_TRACE(ApplicationFactory_impl, "Loading dependencies for soft package " << dep->getName());
        loadDependencies(component, device, (*deployment)->getDependencies());

        // Determine absolute path of dependency's local file
        CF::LoadableDevice::LoadType codeType = implementation->getCodeType();
        const std::string fileName = (*deployment)->getLocalFile();
        LOG_DEBUG(ApplicationFactory_impl, "Loading dependency local file " << fileName);
        try {
             device->load(_appFact._fileMgr, fileName.c_str(), codeType);
        } catch (...) {
            LOG_ERROR(ApplicationFactory_impl, "Failure loading file " << fileName);
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, "Failed to load file");
        }
        _application->addComponentLoadedFile(component.getIdentifier(), fileName);
    }
}

/* Perform 'load' and 'execute' operations to launch component on the assigned device
 *  - Actually loads and executes the component on the given device
 */
void createHelper::loadAndExecuteComponents(const DeploymentList& deployments,
                                            CF::ApplicationRegistrar_ptr _appReg)
{
    LOG_TRACE(ApplicationFactory_impl, "Loading and Executing " << deployments.size() << " components");
    // apply application affinity options to required components
    applyApplicationAffinityOptions(deployments);

    for (unsigned int rc_idx = 0; rc_idx < deployments.size (); rc_idx++) {
        ossie::ComponentDeployment* deployment = deployments[rc_idx];
        ossie::ComponentInfo* component = deployment->getComponent();
        const ossie::ImplementationInfo* implementation = deployment->getImplementation();

        boost::shared_ptr<ossie::DeviceNode> device = deployment->getAssignedDevice();
        if (!device) {
            std::ostringstream message;
            message << "component " << component->getIdentifier() << " was not assigned to a device";
            throw std::logic_error(message.str());
        }

        LOG_TRACE(ApplicationFactory_impl, "Component - " << component->getName()
                  << "   Assigned device - " << device->identifier);
        LOG_INFO(ApplicationFactory_impl, "APPLICATION: " << _waveformContextName << " COMPONENT ID: " 
                 << component->getIdentifier()  << " ASSIGNED TO DEVICE ID/LABEL: " << device->identifier << "/" << device->label);

        // Let the application know to expect the given component
        _application->addComponent(component->getIdentifier(), component->getSpdFileName());
        _application->setComponentImplementation(component->getIdentifier(), implementation->getId());
        if (component->isNamingService()) {
            std::string lookupName = _appFact._domainName + "/" + _waveformContextName + "/" + component->getNamingServiceName() ;
            _application->setComponentNamingContext(component->getIdentifier(), lookupName);
        }
        _application->setComponentDevice(component->getIdentifier(), device->device);

        // get the code.localfile
        LOG_TRACE(ApplicationFactory_impl, "Host is " << device->label << " Local file name is "
                  << implementation->getLocalFileName());

        // Get file name, load if it is not empty
        std::string codeLocalFile = deployment->getLocalFile();
        if (codeLocalFile.empty()) {
            ostringstream eout;
            eout << "code.localfile is empty for component: '";
            eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
            eout << " with implementation id: '" << implementation->getId() << "'";
            eout << " on device id: '" << device->identifier << "'";
            eout << " in waveform '" << _waveformContextName<<"'";
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            LOG_TRACE(ApplicationFactory_impl, eout.str())
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EBADF, eout.str().c_str());
        }

        // narrow to LoadableDevice interface
        CF::LoadableDevice_var loadabledev = ossie::corba::_narrowSafe<CF::LoadableDevice>(device->device);
        if (CORBA::is_nil(loadabledev)) {
            std::ostringstream message;
            message << "component " << component->getIdentifier() << " was assigned to non-loadable device "
                    << device->identifier;
            throw std::logic_error(message.str());
        }

        loadDependencies(*component, loadabledev, deployment->getDependencies());

        // load the file(s)
        ostringstream load_eout; // used for any error messages dealing with load
        try {
            try {
                LOG_TRACE(ApplicationFactory_impl, "loading " << codeLocalFile << " on device " << ossie::corba::returnString(loadabledev->label()));
                loadabledev->load(_appFact._fileMgr, codeLocalFile.c_str(), implementation->getCodeType());
            } catch( ... ) {
                load_eout << "'load' failed for component: '";
                load_eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
                load_eout << " with implementation id: '" << implementation->getId() << "';";
                load_eout << " on device id: '" << device->identifier << "'";
                load_eout << " in waveform '" << _waveformContextName<<"'";
                load_eout << "\nError occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
                throw;
            }
        } catch( CF::InvalidFileName& _ex ) {
            load_eout << " with error: <" << _ex.msg << ">;";
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, load_eout.str().c_str());
        } catch( CF::Device::InvalidState& _ex ) {
            load_eout << " with error: <" << _ex.msg << ">;";
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, load_eout.str().c_str());
        } CATCH_THROW_LOG_TRACE(ApplicationFactory_impl, "", CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, load_eout.str().c_str()));

        // Mark the file as loaded
        _application->addComponentLoadedFile(component->getIdentifier(), codeLocalFile);
                
        // OSSIE extends section D.2.1.6.3 to support loading a directory
        // and execute a file in that directory using a entrypoint
        // 1. Executable means to use CF LoadableDevice::load and CF ExecutableDevice::execute operations. This is a "main" process.
        //    - A Executable that references a directory instead of a file means to recursively load the contents of the directory
        //      and then execute the program specified via entrypoint
        // 2. Driver and Kernel Module means load only.
        // 3. SharedLibrary means dynamic linking.
        // 4. A (SharedLibrary) Without a code entrypoint element means load only.
        // 5. A (SharedLibrary) With a code entrypoint element means load and CF Device::execute.
        if (((implementation->getCodeType() == CF::LoadableDevice::EXECUTABLE) ||
                (implementation->getCodeType() == CF::LoadableDevice::SHARED_LIBRARY)) && (implementation->getEntryPoint().size() != 0)) {

            // See if the LOGGING_CONFIG_URI has already been set
            // via <componentproperties> or initParams
            bool alreadyHasLoggingConfigURI = false;
            std::string logging_uri("");
            CF::DataType* logcfg_prop = NULL;
            CF::Properties execParameters = component->getExecParameters();
            for (unsigned int i = 0; i < execParameters.length(); ++i) {
                std::string propid = static_cast<const char*>(execParameters[i].id);
                if (propid == "LOGGING_CONFIG_URI") {
                  logcfg_prop = &execParameters[i];
                  const char* tmpstr;
                  if ( ossie::any::isNull(logcfg_prop->value) == true ) {
                    LOG_WARN(ApplicationFactory_impl, "Missing value for LOGGING_CONFIG_URI, component: " << _baseNamingContext << "/" << component->getNamingServiceName() );
                  }
                  else {
                    logcfg_prop->value >>= tmpstr;
                    LOG_TRACE(ApplicationFactory_impl, "Resource logging configuration provided, logcfg:" << tmpstr);
                    logging_uri = string(tmpstr);
                    alreadyHasLoggingConfigURI = true;
                  }
                  break;
                }
            }

            ossie::logging::LogConfigUriResolverPtr logcfg_resolver = ossie::logging::GetLogConfigUriResolver();
            std::string logcfg_path = ossie::logging::GetComponentPath( _appFact._domainName, _waveformContextName, component->getNamingServiceName() );
            if ( _appFact._domainManager->getUseLogConfigResolver() && logcfg_resolver ) {
                  std::string t_uri = logcfg_resolver->get_uri( logcfg_path );
                  LOG_DEBUG(ApplicationFactory_impl, "Using LogConfigResolver plugin: path " << logcfg_path << " logcfg:" << t_uri );
                  if ( !t_uri.empty() ) logging_uri = t_uri;
            }
            
            if (!alreadyHasLoggingConfigURI && logging_uri.empty() ) {
                // Query the DomainManager for the logging configuration
                LOG_TRACE(ApplicationFactory_impl, "Checking DomainManager for LOGGING_CONFIG_URI");
                PropertyInterface *log_prop = _appFact._domainManager->getPropertyFromId("LOGGING_CONFIG_URI");
                StringProperty *logProperty = (StringProperty *)log_prop;
                if (!logProperty->isNil()) {
                    logging_uri = logProperty->getValue();
                } else {
                    LOG_TRACE(ApplicationFactory_impl, "DomainManager LOGGING_CONFIG_URI is not set");
                }

                rh_logger::LoggerPtr dom_logger = _appFact._domainManager->getLogger();
                if ( dom_logger ) {
                  rh_logger::LevelPtr dlevel = dom_logger->getLevel();
                  if ( !dlevel ) dlevel = rh_logger::Logger::getRootLogger()->getLevel();
                  CF::DataType prop;
                  prop.id = "DEBUG_LEVEL";
                  prop.value <<= static_cast<CORBA::Long>(ossie::logging::ConvertRHLevelToDebug( dlevel ));
                  component->addExecParameter(prop);
                }
            }

            // if we have a uri but no property, add property to component's exec param list
            if ( logcfg_prop == NULL && !logging_uri.empty() ) {
                CF::DataType prop;
                prop.id = "LOGGING_CONFIG_URI";
                prop.value <<= logging_uri.c_str();
                LOG_DEBUG(ApplicationFactory_impl, "logcfg_prop == NULL " << prop.id << " / " << logging_uri );
                component->addExecParameter(prop);
            }

            if (!logging_uri.empty()) {
                if (logging_uri.substr(0, 4) == "sca:") {
                    string fileSysIOR = ossie::corba::objectToString(_appFact._domainManager->_fileMgr);
                    logging_uri += ("?fs=" + fileSysIOR);
                    LOG_TRACE(ApplicationFactory_impl, "Adding file system IOR " << logging_uri);
                }

                LOG_DEBUG(ApplicationFactory_impl, " LOGGING_CONFIG_URI :" << logging_uri);
                CORBA::Any loguri;
                loguri <<= logging_uri.c_str();
                // this overrides all instances of the property called LOGGING_CONFIG_URI
                LOG_TRACE(ApplicationFactory_impl, "override ....... uri " << logging_uri );
                component->overrideProperty("LOGGING_CONFIG_URI", loguri);
            }
            
            attemptComponentExecution(_appReg, deployment);
        }
    }
}

void createHelper::attemptComponentExecution (CF::ApplicationRegistrar_ptr registrar,
                                              ossie::ComponentDeployment* deployment)
{
    const ossie::ComponentInfo* component = deployment->getComponent();
    const ossie::ImplementationInfo* implementation = deployment->getImplementation();

    // Get executable device reference
    boost::shared_ptr<DeviceNode> device = deployment->getAssignedDevice();
    CF::ExecutableDevice_var execdev = ossie::corba::_narrowSafe<CF::ExecutableDevice>(device->device);
    if (CORBA::is_nil(execdev)){
        std::ostringstream message;
        message << "component " << component->getIdentifier() << " was assigned to non-executable device "
                << device->identifier;
        throw std::logic_error(message.str());
    }

    // Build up the list of command line parameters
    redhawk::PropertyMap execParameters(component->getCommandLineParameters());
    const std::string& nic = deployment->getNicAssignment();
    if (!nic.empty()) {
        execParameters["NIC"] = nic;
    }

    // Add specialized CPU reservation if given
    std::map<std::string,float>::iterator reservation = specialized_reservations.find(component->getIdentifier());
    if (reservation == specialized_reservations.end()) {
        reservation = specialized_reservations.find(component->getUsageName());
    }
    if (reservation != specialized_reservations.end()) {
        execParameters["RH::GPP::MODIFIED_CPU_RESERVATION_VALUE"] = reservation->second;
    }

    // Add the required parameters specified in SR:163
    // Naming Context IOR, Name Binding, and component identifier
    execParameters["COMPONENT_IDENTIFIER"] = component->getIdentifier();
    execParameters["NAME_BINDING"] = component->getNamingServiceName();
    execParameters["DOM_PATH"] = _baseNamingContext;
    execParameters["PROFILE_NAME"] = component->getSpdFileName();

    // Add the Naming Context IOR last to make it easier to parse the command line
    execParameters["NAMING_CONTEXT_IOR"] = ossie::corba::objectToString(registrar);

    // Get entry point
    std::string entryPoint = deployment->getEntryPoint();
    if (entryPoint.empty()) {
        LOG_WARN(ApplicationFactory_impl, "executing using code file as entry point; this is non-SCA compliant behavior; entrypoint must be set");
        entryPoint = deployment->getLocalFile();
    }

    // Get the complete list of dependencies to include in executeLinked
    std::vector<std::string> resolved_softpkg_deps = deployment->getDependencyLocalFiles();
    CF::StringSequence dep_seq;
    dep_seq.length(resolved_softpkg_deps.size());
    for (unsigned int p=0;p!=dep_seq.length();p++) {
        dep_seq[p]=CORBA::string_dup(resolved_softpkg_deps[p].c_str());
    }

    CF::ExecutableDevice::ProcessID_Type tempPid = -1;

    // attempt to execute the component
    try {
        LOG_TRACE(ApplicationFactory_impl, "executing " << entryPoint << " on device " << device->label);
        for (redhawk::PropertyMap::iterator prop = execParameters.begin(); prop != execParameters.end(); ++prop) {
            LOG_TRACE(ApplicationFactory_impl, " exec param " << prop->getId() << " " << prop->getValue().toString());
        }

        // Get options list
        redhawk::PropertyMap options = deployment->getOptions(); 
        for (redhawk::PropertyMap::iterator opt = options.begin(); opt != options.end(); ++opt) {
            LOG_TRACE(ApplicationFactory_impl, " RESOURCE OPTION: " << opt->getId()
                      << " " << opt->getValue().toString());
        }

        // call 'execute' on the ExecutableDevice to execute the component
        tempPid = execdev->executeLinked(entryPoint.c_str(), options, execParameters, dep_seq);
    } catch( CF::InvalidFileName& _ex ) {
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << "InvalidFileName when calling 'execute' on device with device id: '" << device->identifier << "' for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " with error: <" << _ex.msg << ">;";
        eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    } catch( CF::Device::InvalidState& _ex ) {
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << "InvalidState when calling 'execute' on device with device id: '" << device->identifier << "' for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " with error: <" << _ex.msg << ">;";
        eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    } catch( CF::ExecutableDevice::InvalidParameters& _ex ) {
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << "InvalidParameters when calling 'execute' on device with device id: '" << device->identifier << "' for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " with invalid params: <";
        for (unsigned int propIdx = 0; propIdx < _ex.invalidParms.length(); propIdx++){
            eout << "(" << _ex.invalidParms[propIdx].id << "," << ossie::any_to_string(_ex.invalidParms[propIdx].value) << ")";
        }
        eout << " > error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    } catch( CF::ExecutableDevice::InvalidOptions& _ex ) {
        std::string component_version(component->spd.getSoftPkgType());
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << "InvalidOptions when calling 'execute' on device with device id: '" << device->identifier << "' for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " with invalid options: <";
        for (unsigned int propIdx = 0; propIdx < _ex.invalidOpts.length(); propIdx++){
            eout << "(" << _ex.invalidOpts[propIdx].id << "," << ossie::any_to_string(_ex.invalidOpts[propIdx].value) << ")";
        }
        eout << " > error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    } catch (CF::ExecutableDevice::ExecuteFail& ex) {
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << "ExecuteFail when calling 'execute' on device with device id: '" << device->identifier << "' for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " with message: '" << ex.msg << "'";
        eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    } CATCH_THROW_LOG_ERROR(
            ApplicationFactory_impl, "Caught an unexpected error when calling 'execute' on device with device id: '"
            << device->identifier << "' for component: '" << component->getName()
            << "' with component id: '" << component->getIdentifier() << "' "
            << " with implementation id: '" << implementation->getId() << "'"
            << " in waveform '" << _waveformContextName<<"'"
            << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__,
            CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, "Caught an unexpected error when calling 'execute' on device"));

    // handle pid output
    if (tempPid < 0) {
        std::string added_message = this->createVersionMismatchMessage(component_version);
        ostringstream eout;
        eout << added_message;
        eout << "Failed to 'execute' component for component: '";
        eout << component->getName() << "' with component id: '" << component->getIdentifier() << "' ";
        eout << " with implementation id: '" << implementation->getId() << "'";
        eout << " in waveform '" << _waveformContextName<<"'";
        eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        LOG_TRACE(ApplicationFactory_impl, eout.str())
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EAGAIN, eout.str().c_str());
    } else {
        _application->setComponentPid(component->getIdentifier(), tempPid);
    }
}


void createHelper::applyApplicationAffinityOptions(const DeploymentList& deployments) {

    if ( _app_affinity.length() > 0  ) {
      // log deployments with application affinity 
      for ( uint32_t i=0; i < _app_affinity.length(); i++ ) {
          CF::DataType dt = _app_affinity[i];
          LOG_INFO(ApplicationFactory_impl, " Applying Application Affinity: directive id:"  <<  dt.id << "/" <<  ossie::any_to_string( dt.value )) ;
      }
    
      //
      // Promote NIC affinity for all components deployed on the same device
      //
      boost::shared_ptr<ossie::DeviceNode> deploy_on_device;
      for (unsigned int rc_idx = 0; rc_idx < deployments.size(); rc_idx++) {
          ossie::ComponentDeployment* deployment = deployments[rc_idx];
          if (!(deployment->getNicAssignment().empty())) {
              deploy_on_device = deployment->getAssignedDevice();
          }
      }

      if (deploy_on_device) {
          for (unsigned int rc_idx = 0; rc_idx < deployments.size (); rc_idx++) {
              ossie::ComponentDeployment* deployment = deployments[rc_idx];
              boost::shared_ptr<ossie::DeviceNode> dev = deployment->getAssignedDevice();
              // for matching device deployments then apply nic affinity settings
              if (dev->identifier == deploy_on_device->identifier) {
                  deployment->mergeAffinityOptions(_app_affinity);
              }
          }
      }
    }
}


void createHelper::waitForComponentRegistration(const DeploymentList& deployments)
{
    // Wait for all components to be registered before continuing
    int componentBindingTimeout = _appFact._domainManager->getComponentBindingTimeout();
    LOG_TRACE(ApplicationFactory_impl, "Waiting " << componentBindingTimeout << "s for all components register");

    // Track only SCA-compliant components; non-compliant components will never
    // register with the application, nor do they need to be initialized
    std::set<std::string> expected_components;
    for (DeploymentList::const_iterator ii = deployments.begin(); ii != deployments.end(); ++ii) {
        ossie::ComponentInfo* component = (*ii)->getComponent();
        if (component->isScaCompliant()) {
            expected_components.insert(component->getIdentifier());
        }
    }

    // Record current time, to measure elapsed time in the event of a failure
    time_t start = time(NULL);

    if (!_application->waitForComponents(expected_components, componentBindingTimeout)) {
        // For reference, determine much time has really elapsed.
        time_t elapsed = time(NULL)-start;
        LOG_ERROR(ApplicationFactory_impl, "Timed out waiting for component to bind to naming context (" << elapsed << "s elapsed)");
        ostringstream eout;
        for (unsigned int req_idx = 0; req_idx < deployments.size(); req_idx++) {
            ossie::ComponentDeployment* deployment = deployments[req_idx];
            const ossie::ComponentInfo* component = deployment->getComponent();
            if (expected_components.count(component->getIdentifier())) {
                eout << "Timed out waiting for component to register: '" << component->getName()
                     << "' with component id: '" << component->getIdentifier()
                     << " assigned to device: '" << deployment->getAssignedDevice()->identifier;
                break;
            }
        }
        eout << " in waveform '" << _waveformContextName<<"';";
        eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
        throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
    }
}

/* Initializes the components
 *  - Make sure internal lists are up to date
 *  - Ensure components have started and are bound to Naming Service
 *  - Initialize each component
 */
void createHelper::initializeComponents(const DeploymentList& deployments)
{
    // Install the different components in the system
    LOG_TRACE(ApplicationFactory_impl, "initializing " << deployments.size() << " waveform components");

    CF::Components_var app_registeredComponents = _application->registeredComponents();

    for (unsigned int rc_idx = 0; rc_idx < deployments.size (); rc_idx++) {
        ossie::ComponentDeployment* deployment = deployments[rc_idx];
        const ossie::ComponentInfo* component = deployment->getComponent();

        // If the component is non-SCA compliant then we don't expect anything beyond this
        if (!component->isScaCompliant()) {
            LOG_TRACE(ApplicationFactory_impl, "Component is non SCA-compliant, continuing to next component");
            continue;
        }

        if (!component->isResource()) {
            LOG_TRACE(ApplicationFactory_impl, "Component is not a resource, continuing to next component");
            continue;
        }

        // Find the component on the Application
        const std::string componentId = component->getIdentifier();
        CF::Resource_var resource = CF::Resource::_nil();
        for (unsigned int comp_idx=0; comp_idx<app_registeredComponents->length(); comp_idx++) {
            std::string comp_id = std::string(app_registeredComponents[comp_idx].identifier);
            if (comp_id == componentId) {
                resource = ossie::corba::_narrowSafe<CF::Resource>(app_registeredComponents[comp_idx].componentObject);
                break;
            }
        }
        if (CORBA::is_nil(resource)) {
            ostringstream eout;
            eout << "CF::Resource::_narrow failed with Unknown Exception for component: '" << component->getName()
                 << "' with component id: '" << componentId
                 << " assigned to device: '"<<deployment->getAssignedDevice()->identifier<<"'";
            eout << " in waveform '" << _waveformContextName<<"';";
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
        }

        deployment->setResourcePtr(resource);

        int initAttempts=3;
        while ( initAttempts > 0 ) {
            initAttempts--;
            if ( ossie::corba::objectExists(resource) == true ) { initAttempts = 0; continue; }
            LOG_DEBUG(ApplicationFactory_impl, "Retrying component ping............ comp:" << component->getIdentifier() << " waveform: " << _waveformContextName);
            usleep(1000);
        }


        //
        // call resource's initializeProperties method to handle any properties required for construction
        //
        LOG_DEBUG(ApplicationFactory_impl, "Initialize properties for component " << componentId);
        if (component->isResource () && component->isConfigurable ()) {
          CF::Properties partialStruct = component->containsPartialStructConstruct();
          if (partialStruct.length() != 0) {
            ostringstream eout;
            eout << "Failed to 'configure' Assembly Controller: '";
            eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<< deployment->getAssignedDevice()->identifier << "' ";
            eout << " in waveform '"<< _waveformContextName<<"';";
            eout <<  "This component contains structure"<<partialStruct[0].id<<" with a mix of defined and nil values.";
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
          }
          try {
            // Try to set the initial values for the component's properties
            CF::Properties initProps = component->getInitializeProperties();
            resource->initializeProperties(initProps);
          } catch(CF::PropertySet::InvalidConfiguration& e) {
            ostringstream eout;
            eout << "Failed to initialize component properties: '";
            eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<<deployment->getAssignedDevice()->identifier << "' ";
            eout << " in waveform '"<< _waveformContextName<<"';";
            eout <<  "InvalidConfiguration with this info: <";
            eout << e.msg << "> for these invalid properties: ";
            for (unsigned int propIdx = 0; propIdx < e.invalidProperties.length(); propIdx++){
              eout << "(" << e.invalidProperties[propIdx].id << ",";
              eout << ossie::any_to_string(e.invalidProperties[propIdx].value) << ")";
            }
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::ApplicationFactory::InvalidInitConfiguration(e.invalidProperties);
          } catch(CF::PropertySet::PartialConfiguration& e) {
            ostringstream eout;
            eout << "Failed to initialize component properties: '";
            eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<<deployment->getAssignedDevice()->identifier << "' ";
            eout << " in waveform '"<< _waveformContextName<<"';";
            eout << "PartialConfiguration for these invalid properties: ";
            for (unsigned int propIdx = 0; propIdx < e.invalidProperties.length(); propIdx++){
              eout << "(" << e.invalidProperties[propIdx].id << ",";
              eout << ossie::any_to_string(e.invalidProperties[propIdx].value) << ")";
            }
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::ApplicationFactory::InvalidInitConfiguration(e.invalidProperties);
          } catch( ... ) {
            ostringstream eout;
            std::string component_version(component->spd.getSoftPkgType());
            std::string added_message = this->createVersionMismatchMessage(component_version);
            eout << added_message;
            eout << "Failed to initialize component properties: '";
            eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<<deployment->getAssignedDevice()->identifier << "' ";
            eout << " in waveform '"<< _waveformContextName<<"';";
            eout << "'initializeProperties' failed with Unknown Exception";
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, eout.str().c_str());
          }
        }

        LOG_TRACE(ApplicationFactory_impl, "Initializing component " << componentId);
        try {
            resource->initialize();
        } catch (const CF::LifeCycle::InitializeError& error) {
            // Dump the detailed initialization failure to the log
            ostringstream logmsg;
            std::string component_version(component->spd.getSoftPkgType());
            std::string added_message = this->createVersionMismatchMessage(component_version);
            logmsg << added_message;
            logmsg << "Initializing component " << componentId << " failed";
            for (CORBA::ULong index = 0; index < error.errorMessages.length(); ++index) {
                logmsg << std::endl << error.errorMessages[index];
            }
            LOG_ERROR(ApplicationFactory_impl, logmsg.str());

            ostringstream eout;
            eout << added_message;
            eout << "Unable to initialize component " << componentId;
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
        } catch (const CORBA::SystemException& exc) {
            ostringstream eout;
            std::string component_version(component->spd.getSoftPkgType());
            std::string added_message = this->createVersionMismatchMessage(component_version);
            eout << added_message;
            eout << "CORBA " << exc._name() << " exception initializing component " << componentId;
            LOG_ERROR(ApplicationFactory_impl, eout.str());
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
        }
    }
}

void createHelper::configureComponents(const DeploymentList& deployments)
{
    DeploymentList configure_list;
    ossie::ComponentDeployment* ac_deployment = 0;
    for (DeploymentList::const_iterator depl = deployments.begin(); depl != deployments.end(); ++depl) {
        const ossie::ComponentInfo* component = (*depl)->getComponent();
        if (!component->isScaCompliant()) {
            // If the component is non-SCA compliant then we don't expect anything beyond this
            LOG_TRACE(ApplicationFactory_impl, "Skipping configure of non SCA-compliant component "
                      << component->getIdentifier());
        } else if (!component->isResource()) {
            LOG_TRACE(ApplicationFactory_impl, "Skipping configure of non-resource component "
                      << component->getIdentifier());
        } else {
            // The component is configurable; if it's the assembly controller,
            // save it for the end
            if (component->isAssemblyController()) {
                ac_deployment = *depl;
            } else {
                configure_list.push_back(*depl);
            }
        }
    }
    // Configure the assembly controller last, if it's configurable
    if (ac_deployment) {
        configure_list.push_back(ac_deployment);
    }

    for (DeploymentList::iterator depl = configure_list.begin(); depl != configure_list.end(); ++depl) {
        const ossie::ComponentInfo* component = (*depl)->getComponent();
        
        // Assuming 1 instantiation for each componentplacement
        if (component->isNamingService()) {

            CF::Resource_var _rsc = (*depl)->getResourcePtr();

            if (CORBA::is_nil(_rsc)) {
                LOG_ERROR(ApplicationFactory_impl, "Could not get component reference");
                ostringstream eout;
                std::string component_version(component->spd.getSoftPkgType());
                std::string added_message = this->createVersionMismatchMessage(component_version);
                eout << added_message;
                eout << "Could not get component reference for component: '" 
                     << component->getName() << "' with component id: '" 
                     << component->getIdentifier() << " assigned to device: '"
                     << (*depl)->getAssignedDevice()->identifier<<"'";
                eout << " in waveform '" << _waveformContextName<<"';";
                eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
                throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
            }

            CF::Properties partialStruct = component->containsPartialStructConfig();
            bool partialWarn = false;
            if (partialStruct.length() != 0) {
                ostringstream eout;
                eout <<  "Component " << component->getIdentifier() << " contains structure"<< partialStruct[0].id <<" with a mix of defined and nil values. The behavior for the component is undefined";
                LOG_WARN(ApplicationFactory_impl, eout.str());
                partialWarn = true;
            }
            try {
                // try to configure the component
                _rsc->configure (component->getNonNilConfigureProperties());
            } catch(CF::PropertySet::InvalidConfiguration& e) {
                ostringstream eout;
                eout << "Failed to 'configure' component: '";
                eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<<(*depl)->getAssignedDevice()->identifier << "' ";
                eout << " in waveform '"<< _waveformContextName<<"';";
                eout <<  "InvalidConfiguration with this info: <";
                eout << e.msg << "> for these invalid properties: ";
                for (unsigned int propIdx = 0; propIdx < e.invalidProperties.length(); propIdx++){
                    eout << "(" << e.invalidProperties[propIdx].id << ",";
                    eout << ossie::any_to_string(e.invalidProperties[propIdx].value) << ")";
                }
                if (partialWarn) {
                    eout << ". Note that this component contains a property with a mix of defined and nil values.";
                }
                eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
                LOG_ERROR(ApplicationFactory_impl, eout.str());
                throw CF::ApplicationFactory::InvalidInitConfiguration(e.invalidProperties);
            } catch(CF::PropertySet::PartialConfiguration& e) {
                ostringstream eout;
                eout << "Failed to instantiate component: '";
                eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<<(*depl)->getAssignedDevice()->identifier << "' ";
                eout << " in waveform '"<< _waveformContextName<<"';";
                eout << "Failed to 'configure' component; PartialConfiguration for these invalid properties: ";
                for (unsigned int propIdx = 0; propIdx < e.invalidProperties.length(); propIdx++){
                    eout << "(" << e.invalidProperties[propIdx].id << ",";
                    eout << ossie::any_to_string(e.invalidProperties[propIdx].value) << ")";
                }
                if (partialWarn) {
                    eout << ". Note that this component contains a property with a mix of defined and nil values.";
                }
                eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
                LOG_ERROR(ApplicationFactory_impl, eout.str());
                throw CF::ApplicationFactory::InvalidInitConfiguration(e.invalidProperties);
            } catch( ... ) {
                ostringstream eout;
                eout << "Failed to instantiate component: '";
                eout << component->getName() << "' with component id: '" << component->getIdentifier() << " assigned to device: '"<< (*depl)->getAssignedDevice()->identifier << "' ";
                eout << " in waveform '"<< _waveformContextName<<"';";
                eout << "'configure' failed with Unknown Exception";
                if (partialWarn) {
                    eout << ". Note that this component contains a property with a mix of defined and nil values.";
                }
                eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
                LOG_ERROR(ApplicationFactory_impl, eout.str());
                throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EINVAL, eout.str().c_str());
            }
        }
    }
}

/* Connect the components
 *  - Connect the components
 */
void createHelper::connectComponents(ossie::ApplicationDeployment& appDeployment,
                                     std::vector<ConnectionNode>& connections,
                                     string base_naming_context)
{
    const std::vector<Connection>& _connection = _appFact._sadParser.getConnections ();

    // Create an AppConnectionManager to resolve and track all connections in the application.
    using ossie::AppConnectionManager;
    AppConnectionManager connectionManager(_appFact._domainManager, &appDeployment, &appDeployment, base_naming_context);

    // Create all resource connections
    LOG_TRACE(ApplicationFactory_impl, "Establishing " << _connection.size() << " waveform connections")
    for (int c_idx = _connection.size () - 1; c_idx >= 0; c_idx--) {
        const Connection& connection = _connection[c_idx];

        LOG_TRACE(ApplicationFactory_impl, "Processing connection " << connection.getID());

        // Attempt to resolve the connection; if any connection fails, application creation fails.
        if (!connectionManager.resolveConnection(connection)) {
            LOG_ERROR(ApplicationFactory_impl, "Unable to make connection " << connection.getID());
            ostringstream eout;
            eout << "Unable to make connection " << connection.getID();
            eout << " in waveform '"<< _waveformContextName<<"';";
            eout << " error occurred near line:" <<__LINE__ << " in file:" <<  __FILE__ << ";";
            throw CF::ApplicationFactory::CreateApplicationError(CF::CF_EIO, eout.str().c_str());
        }
    }

    // Copy all established connections into the connection array
    const std::vector<ConnectionNode>& establishedConnections = connectionManager.getConnections();
    std::copy(establishedConnections.begin(), establishedConnections.end(), std::back_inserter(connections));
}

std::vector<CF::Resource_var> createHelper::getStartOrder(const DeploymentList& deployments)
{
    // Now that all of the components are known, bin the start orders based on
    // the values in the SAD. Using a multimap, keyed on the start order value,
    // accounts for duplicate keys and allows assigning the effective order
    // easily by iterating through all entries.
    typedef std::multimap<int,ossie::ComponentDeployment*> StartOrderMap;
    StartOrderMap start_map;
    for (size_t index = 0; index < deployments.size(); ++index) {
        ossie::ComponentInfo* component = deployments[index]->getComponent();
        if (!component->isAssemblyController() && component->hasStartOrder()) {
            // Only track start order if it was provided, and the component is
            // not the assembly controller
            start_map.insert(std::make_pair(component->getStartOrder(), deployments[index]));
        }
    }

    // Build the start order vector in the right order
    std::vector<CF::Resource_var> start_order;
    int index = 1;
    LOG_TRACE(ApplicationFactory_impl, "Assigning start order");
    for (StartOrderMap::iterator ii = start_map.begin(); ii != start_map.end(); ++ii, ++index) {
        LOG_TRACE(ApplicationFactory_impl, index << ": "
                  << ii->second->getComponent()->getInstantiationIdentifier());
        start_order.push_back(ii->second->getResourcePtr());
    }
    return start_order;
}

createHelper::createHelper (
        const ApplicationFactory_impl& appFact,
        string                         waveformContextName,
        string                         baseNamingContext,
        CosNaming::NamingContext_ptr   waveformContext,
        CosNaming::NamingContext_ptr   domainContext ):

    _appFact(appFact),
    _allocationMgr(_appFact._domainManager->_allocationMgr),
    _allocations(*_allocationMgr),
    _isComplete(false),
    _application(0)
{
    this->_waveformContextName = waveformContextName;
    this->_baseNamingContext   = baseNamingContext;
    this->_waveformContext     = CosNaming::NamingContext::_duplicate(waveformContext);
    this->_domainContext     =  domainContext;
}

createHelper::~createHelper()
{
    if (!_isComplete) {
        _cleanupFailedCreate();
    }
    if (_application) {
        _application->_remove_ref();
    }
}

void createHelper::_cleanupFailedCreate()
{
    if (_application) {
        _application->releaseComponents();
        _application->terminateComponents();
        _application->unloadComponents();
        _application->_cleanupActivations();
    }

    LOG_TRACE(ApplicationFactory_impl, "Removing all bindings from naming context");
    try {
      if ( _appFact._domainManager && !_appFact._domainManager->bindToDomain() ) {
        ossie::corba::unbindAllFromContext(_waveformContext);
      }
    } CATCH_LOG_WARN(ApplicationFactory_impl, "Could not unbind contents of naming context");

    CosNaming::Name DNContextname;
    DNContextname.length(1);
    DNContextname[0].id = _waveformContextName.c_str();
    LOG_TRACE(ApplicationFactory_impl, "Unbinding the naming context")
    try {
        _appFact._domainContext->unbind(DNContextname);
    } catch ( ... ) {
    }

    LOG_TRACE(ApplicationFactory_impl, "Destroying naming context");
    try {
        _waveformContext->destroy();
    } CATCH_LOG_WARN(ApplicationFactory_impl, "Could not destroy naming context");
}
