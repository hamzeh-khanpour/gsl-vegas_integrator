/*
 * TCSGeneratorService.cpp
 *
 *  Created on: Feb 8, 2021
 *      Author: Pawel Sznajder (NCBJ)
 */

#include "../../include/services/TCSGeneratorService.h"

#include <ElementaryUtils/logger/CustomException.h>
#include <ElementaryUtils/parameters/GenericType.h>
#include <ElementaryUtils/parameters/Parameters.h>
#include <ElementaryUtils/string_utils/Formatter.h>
#include <ElementaryUtils/string_utils/StringUtils.h>
#include <NumA/linear_algebra/vector/Vector3D.h>
#include <partons/beans/automation/BaseObjectData.h>
#include <partons/beans/observable/ObservableResult.h>
#include <partons/BaseObjectRegistry.h>
#include <partons/FundamentalPhysicalConstants.h>
#include <partons/ModuleObjectFactory.h>
#include <partons/Partons.h>
#include <partons/utils/type/PhysicalType.h>
#include <partons/utils/type/PhysicalUnit.h>
#include <stddef.h>
#include <chrono>
#include <cmath>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>

#include "../../include/automation/MonteCarloTask.h"
#include "../../include/beans/containers/KinematicRange.h"
#include "../../include/beans/types/EventAttributeType.h"
#include "../../include/beans/types/ParticleType.h"
#include "../../include/Epic.h"
#include "../../include/managers/ModuleObjectFactory.h"
#include "../../include/modules/event_generator/EventGeneratorModule.h"
#include "../../include/modules/radiative_corrections/RCModule.h"
#include "../../include/modules/writer/WriterModule.h"

namespace EPIC {

const std::string TCSGeneratorService::TCS_GENERATOR_SERVICE_SUBPROCESSTYPE =
        "subprocess_type";

const unsigned int TCSGeneratorService::classId =
        PARTONS::Partons::getInstance()->getBaseObjectRegistry()->registerBaseObject(
                new TCSGeneratorService("TCSGeneratorService"));

TCSGeneratorService::TCSGeneratorService(const std::string &className) :
        GeneratorService<TCSKinematicRanges, PARTONS::TCSProcessModule,
                TCSKinematicModule, TCSRCModule>(className) {
    m_subProcessType = PARTONS::VCSSubProcessType::ALL;
}

TCSGeneratorService::TCSGeneratorService(const TCSGeneratorService &other) :
        GeneratorService<TCSKinematicRanges, PARTONS::TCSProcessModule,
                TCSKinematicModule, TCSRCModule>(other) {
    m_subProcessType = other.m_subProcessType;
}

TCSGeneratorService *TCSGeneratorService::clone() const {
    return new TCSGeneratorService(*this);
}

TCSGeneratorService::~TCSGeneratorService() {
}

double TCSGeneratorService::getEventDistribution(
        std::vector<double> &kin) const {

    //transform
    transformVariables(kin);

    //observed kinematics
    TCSKinematic partonsKinObs(kin.at(5), kin.at(6), kin.at(0), kin.at(1),
            m_experimentalConditions.getLeptonEnergyFixedTargetEquivalent(),
            kin.at(2), kin.at(3), kin.at(4));

    //check range
    if (!m_kinematicRanges.inRange(m_experimentalConditions, partonsKinObs))
        return 0.;

    //rc
    std::vector<double> rcVariables;
    rcVariables.insert(std::end(rcVariables), std::begin(kin) + 7,
            std::end(kin));

    std::tuple<double, ExperimentalConditions, TCSKinematic> rcTrue =
            m_pRCModule->evaluate(m_experimentalConditions, partonsKinObs,
                    rcVariables);

    //check if zero
    if (std::get < 0 > (rcTrue) == 0.)
        return 0.;

    //check if valid
    if (!m_pKinematicModule->checkIfValid(std::get < 1 > (rcTrue),
            std::get < 2 > (rcTrue))) {
        return 0.;
    }

    //evaluate
    double result =
            getFlux(std::get < 2 > (rcTrue)) * std::get < 0
                    > (rcTrue)
                            * m_pProcessModule->compute(
                                    std::get < 1 > (rcTrue).getLeptonHelicity(),
                                    std::get < 1
                                            > (rcTrue).getHadronPolarisation(),
                                    std::get < 2
                                            > (rcTrue).toPARTONSTCSObservableKinematic(),
                                    m_pProcessModule->getListOfAvailableGPDTypeForComputation(),
                                    m_subProcessType).getValue().makeSameUnitAs(
                                    PARTONS::PhysicalUnit::NB).getValue();

    //jacobian
    result *= getJacobian(kin);

    //check if nan
    if (std::isnan(result)) {

        warn(__func__,
                "Value is NaN, setting zero instead, look for previous messages for a reason");
        result = 0.;
    }

    return result;
}

void TCSGeneratorService::isServiceWellConfigured() const {

    GeneratorService<TCSKinematicRanges, PARTONS::TCSProcessModule,
            TCSKinematicModule, TCSRCModule>::isServiceWellConfigured();

    if (m_subProcessType != PARTONS::VCSSubProcessType::ALL
            && m_subProcessType != PARTONS::VCSSubProcessType::BH
            && m_subProcessType != PARTONS::VCSSubProcessType::TCS) {
        throw ElemUtils::CustomException(getClassName(), __func__,
                ElemUtils::Formatter()
                        << "Wrong subprocess type, must be ALL, BH or TCS, is: "
                        << PARTONS::VCSSubProcessType(m_subProcessType).toString());
    }
}

void TCSGeneratorService::run() {

    //check if well configured
    isServiceWellConfigured();

    //kinematic ranges
    std::vector<KinematicRange> ranges = m_pKinematicModule->getKinematicRanges(m_experimentalConditions, m_kinematicRanges);

    transformRanges(ranges);

    ranges.insert(std::end(ranges),
            std::begin(m_pRCModule->getVariableRanges()),
            std::end(m_pRCModule->getVariableRanges()));

    //Initialize generator
    m_debugTimeInitialization.first = std::chrono::steady_clock::now();
    m_pEventGeneratorModule->initialise(ranges, *this);
    m_debugTimeInitialization.second = std::chrono::steady_clock::now();

    //open writer
    m_pWriterModule->open();

    //loop over events
    m_debugTimeGeneration.first = std::chrono::steady_clock::now();

    for (size_t i = 0; i < m_generalConfiguration.getNEvents(); i++) {

        //generate kinematics
        std::pair<std::vector<double>, double> eventVec =
                m_pEventGeneratorModule->generateEvent();

        //transform variables
        transformVariables(eventVec.first);

        //histogram
        fillHistograms(eventVec.first);

        //create kinematics object
        TCSKinematic partonsKinObs(eventVec.first.at(5), eventVec.first.at(6),
                eventVec.first.at(0), eventVec.first.at(1),
                m_experimentalConditions.getLeptonEnergyFixedTargetEquivalent(),
                eventVec.first.at(2), eventVec.first.at(3),
                eventVec.first.at(4));

        //rc
        std::vector<double> rcVariables;
        rcVariables.insert(std::end(rcVariables),
                std::begin(eventVec.first) + 7, std::end(eventVec.first));

        std::tuple<double, ExperimentalConditions, TCSKinematic> rcTrue =
                m_pRCModule->evaluate(m_experimentalConditions, partonsKinObs,
                        rcVariables);

        //target polarisation
        checkTargetPolarisation(std::get<1>(rcTrue));

        //create event
        Event event = m_pKinematicModule->evaluate(std::get < 1 > (rcTrue),
                std::get < 2 > (rcTrue));

        //rc
        m_pRCModule->updateEvent(event, rcVariables);

        //add attributes
        event.addAttribute(EventAttributeType::ID, i);
        event.addAttribute(EventAttributeType::WEIGHT, eventVec.second);

        //write
        m_pWriterModule->write(event);
    }

    m_debugTimeGeneration.second = std::chrono::steady_clock::now();

    //general configuration
    printAndSaveGenerationConfiguration();

    //close
    m_pWriterModule->close();
}

double TCSGeneratorService::getFlux(const TCSKinematic& kin) const {

    double Q2 = kin.getQ2();
    double y = kin.getY();

    double Q2Min =
            pow(
                    y
                            * ParticleType(
                                    m_experimentalConditions.getLeptonType()).getMass(),
                    2) / (1. - y);

    return PARTONS::Constant::FINE_STRUCTURE_CONSTANT / (2 * M_PI * Q2)
            * ((1. + pow(1. - y, 2)) / y - 2 * (1. - y) * Q2Min / (y * Q2));
}

void TCSGeneratorService::getAdditionalGeneralConfigurationFromTask(
        const MonteCarloTask &task) {

    //formatter
    ElemUtils::Formatter formatter;

    formatter << "Additional general configuration:\n";
    formatter << '\n';

    // subprocess
    if (task.getGeneralConfiguration().getParameters().isAvailable(
            TCS_GENERATOR_SERVICE_SUBPROCESSTYPE)) {
        m_subProcessType =
                PARTONS::VCSSubProcessType(
                        task.getGeneralConfiguration().getParameters().getLastAvailable().getString()).getType();
    }

    formatter << "Subprocess: "
            << PARTONS::VCSSubProcessType(m_subProcessType).toString() << '\n';

    info(__func__, formatter.str());
}

void TCSGeneratorService::getKinematicRangesFromTask(
        const MonteCarloTask &task) {

    m_kinematicRanges = TCSKinematicRanges::getTCSKinematicRangesfromTask(task);

    info(__func__,
            ElemUtils::Formatter() << "Kinematic ranges:\n"
                    << m_kinematicRanges.toString() << '\n');
}

void TCSGeneratorService::getProcessModuleFromTask(const MonteCarloTask &task) {

    // check if available
    if (ElemUtils::StringUtils::equals(
            task.getComputationConfiguration().getModuleType(),
            PARTONS::TCSProcessModule::TCS_PROCESS_MODULE_CLASS_NAME)) {

        // configure
        m_pProcessModule =
                PARTONS::Partons::getInstance()->getModuleObjectFactory()->newTCSProcessModule(
                        task.getComputationConfiguration().getModuleClassName());

        m_pProcessModule->configure(
                task.getComputationConfiguration().getParameters());

        m_pProcessModule->prepareSubModules(
                task.getComputationConfiguration().getSubModules());
    } else {
        throw ElemUtils::CustomException(getClassName(), __func__,
                ElemUtils::Formatter() << "You have not provided any "
                        << PARTONS::TCSProcessModule::TCS_PROCESS_MODULE_CLASS_NAME);
    }

    info(__func__,
            ElemUtils::Formatter() << "Process module: "
                    << m_pProcessModule->getClassName());
}

void TCSGeneratorService::getKinematicModuleFromTask(
        const MonteCarloTask &task) {

    // check if available
    if (ElemUtils::StringUtils::equals(
            task.getKinematicConfiguration().getModuleType(),
            TCSKinematicModule::TCS_KINEMATIC_MODULE_CLASS_NAME)) {

        // configure
        m_pKinematicModule =
                Epic::getInstance()->getModuleObjectFactory()->newTCSKinematicModule(
                        task.getKinematicConfiguration().getModuleClassName());

        m_pKinematicModule->configure(
                task.getKinematicConfiguration().getParameters());

        m_pKinematicModule->prepareSubModules(
                task.getKinematicConfiguration().getSubModules());
    } else {
        throw ElemUtils::CustomException(getClassName(), __func__,
                ElemUtils::Formatter() << "You have not provided any "
                        << TCSKinematicModule::TCS_KINEMATIC_MODULE_CLASS_NAME);
    }

    info(__func__,
            ElemUtils::Formatter() << "Kinematic module: "
                    << m_pKinematicModule->getClassName());
}

void TCSGeneratorService::getRCModuleFromTask(const MonteCarloTask &task) {

    // check if available
    if (ElemUtils::StringUtils::equals(
            task.getRCConfiguration().getModuleType(),
            TCSRCModule::TCS_RC_MODULE_CLASS_NAME)) {

        // configure
        m_pRCModule =
                Epic::getInstance()->getModuleObjectFactory()->newTCSRCModule(
                        task.getRCConfiguration().getModuleClassName());

        m_pRCModule->configure(task.getRCConfiguration().getParameters());

        m_pRCModule->prepareSubModules(
                task.getRCConfiguration().getSubModules());
    } else {
        throw ElemUtils::CustomException(getClassName(), __func__,
                ElemUtils::Formatter() << "You have not provided any "
                        << TCSRCModule::TCS_RC_MODULE_CLASS_NAME);
    }

    info(__func__,
            ElemUtils::Formatter() << "Radiative correction module: "
                    << m_pRCModule->getClassName());
}

void TCSGeneratorService::addAdditionalGenerationConfiguration(
        GenerationInformation& generationInformation) {

    GeneratorService<TCSKinematicRanges, PARTONS::TCSProcessModule,
            TCSKinematicModule, TCSRCModule>::addAdditionalGenerationConfiguration(
            generationInformation);

    generationInformation.addAdditionalInfo(
            std::make_pair("suprocesses_type",
                    PARTONS::VCSSubProcessType(m_subProcessType).toString()));
}

void TCSGeneratorService::bookHistograms(){

    m_histograms.push_back(
            new TH1D("h_t", "t variable", 100,
                    m_kinematicRanges.getRangeT().getMin(),
                    m_kinematicRanges.getRangeT().getMax()));
    m_histograms.push_back(
            new TH1D("h_QPrim2", "Q'^{2} variable", 100,
                    m_kinematicRanges.getRangeQ2Prim().getMin(),
                    m_kinematicRanges.getRangeQ2Prim().getMax()));
    m_histograms.push_back(
            new TH1D("h_phiS", "#phi_{S} angle", 100,
                    m_kinematicRanges.getRangePhiS().getMin(),
                    m_kinematicRanges.getRangePhiS().getMax()));
    m_histograms.push_back(
            new TH1D("h_phiL", "#phi_{l} angle", 100,
                    m_kinematicRanges.getRangePhiL().getMin(),
                    m_kinematicRanges.getRangePhiL().getMax()));
    m_histograms.push_back(
            new TH1D("h_thetaL", "#theta_{l} angle", 100,
                    m_kinematicRanges.getRangeThetaL().getMin(),
                    m_kinematicRanges.getRangeThetaL().getMax()));
    m_histograms.push_back(
            new TH1D("h_y", "y variable", 100,
                    m_kinematicRanges.getRangeY().getMin(),
                    m_kinematicRanges.getRangeY().getMax()));
    m_histograms.push_back(
            new TH1D("h_Q2", "Q^{2} variable", 100,
                    m_kinematicRanges.getRangeQ2().getMin(),
                    m_kinematicRanges.getRangeQ2().getMax()));

    const std::vector<KinematicRange>& rcRanges = m_pRCModule->getVariableRanges();
    std::vector<KinematicRange>::const_iterator it;

    for(it = rcRanges.begin(); it != rcRanges.end(); it++){

        std::stringstream ssA;
        ssA << "h_rc" << int(it - rcRanges.begin());

        std::stringstream ssB;
        ssB << "RC variable " << int(it - rcRanges.begin());

        m_histograms.push_back(
                new TH1D(ssA.str().c_str(), ssB.str().c_str(), 100,
                        it->getMin(), it->getMax()));
    }
}

void TCSGeneratorService::fillHistograms(const std::vector<double>& variables){

    std::vector<double>::const_iterator it;

    for(it = variables.begin(); it != variables.end(); it++){
        m_histograms.at(int(it - variables.begin()))->Fill(*it);
    }
}

void TCSGeneratorService::transformVariables(std::vector<double>& variables) const{

    variables.at(0) = -1 * exp(variables.at(0));
    variables.at(1) = exp(variables.at(1));
    variables.at(5) = exp(variables.at(5));
    variables.at(6) = exp(variables.at(6));
}

void TCSGeneratorService::transformRanges(std::vector<KinematicRange>& ranges) const{

    ranges.at(0).setMinMax(log(-1 * ranges.at(0).getMax()), log(-1 * ranges.at(0).getMin()));
    ranges.at(1).setMinMax(log(ranges.at(1).getMin()), log(ranges.at(1).getMax()));
    ranges.at(5).setMinMax(log(ranges.at(5).getMin()), log(ranges.at(5).getMax()));
    ranges.at(6).setMinMax(log(ranges.at(6).getMin()), log(ranges.at(6).getMax()));
}

double TCSGeneratorService::getJacobian(const std::vector<double>& variables) const{
    return -1 * variables.at(0) * variables.at(1) * variables.at(5) * variables.at(6);
}

} /* namespace EPIC */
